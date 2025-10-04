#include <boron/boron.h>
#include <boron/svcs.h>

namespace mlibc {

constexpr int MAX_FDS = 1024;

// Translates a status code to an errno.
const int g_statusToErrno[] = {
	0,       // STATUS_SUCCESS
	
	EINVAL,  // STATUS_INVALID_PARAMETER
	EFAULT,  // STATUS_ACCESS_VIOLATION
	ENOMEM,  // STATUS_INSUFFICIENT_MEMORY
	ENOSYS,  // STATUS_UNIMPLEMENTED
	EIEIO,   // STATUS_IPL_TOO_HIGH, should never be seen
	EIEIO,   // STATUS_REFAULT, should never be seen
	EIEIO,   // STATUS_REFAULT_SLEEP, should never be seen
	
	EIEIO,   // STATUS_WAITING, should never be seen
	EINTR,   // STATUS_ALERTED
	ETIMEDOUT, // STATUS_TIMEOUT
	EINTR,   // STATUS_KILLED, should probably never be seen
	EINTR,   // STATUS_KERNEL_APC, should probably never be seen
	
	EFAULT,  // STATUS_FAULT
	EFAULT,  // STATUS_NO_REMAP
	
	EINVAL,  // STATUS_NAME_INVALID
	EEXIST,  // STATUS_NAME_COLLISION
	ENOENT,  // STATUS_TYPE_MISMATCH
	ENOENT,  // STATUS_OBJECT_UNOWNED
	ENOENT,  // STATUS_NAME_NOT_FOUND
	ENOSYS,  // STATUS_UNSUPPORTED_FUNCTION -- ENOTSUP exists too but I'm not sure it's correct here?
	ENOENT,  // STATUS_PATH_INVALID
	0,       // STATUS_DIRECTORY_DONE, not a failure
	ELOOP,   // STATUS_LOOP_TOO_DEEP
	ENOENT,  // STATUS_UNASSIGNED_LINK
	ELOOP,   // STATUS_PATH_TOO_DEEP
	ENAMETOOLONG, // STATUS_NAME_TOO_LONG
	EINVAL,  // STATUS_NOT_LINKED
	EEXIST,  // STATUS_ALREADY_LINKED
	EBADF,   // STATUS_INVALID_HANDLE
			 
	EIEIO,   // STATUS_TABLE_NOT_EMPTY
	EIEIO,   // STATUS_DELETE_CANCELED
	EMFILE,  // STATUS_TOO_MANY_HANDLES
			 
	0,       // STATUS_PENDING, not a failure
	EINVAL,  // STATUS_INVALID_HEADER, should probably never be seen
	EIEIO,   // STATUS_SAME_FRAME, should never be seen
	EIEIO,   // STATUS_NO_MORE_FRAMES, should never be seen
	EAGAIN,  // STATUS_MORE_PROCESSING_REQUIRED
	ENOSPC,  // STATUS_INSUFFICIENT_SPACE
	ENOENT,  // STATUS_NO_SUCH_DEVICES
	EIEIO,   // STATUS_UNLOAD, should never be seen
	ENOTDIR, // STATUS_NOT_A_DIRECTORY
	EIO,     // STATUS_HARDWARE_IO_ERROR
	EINVAL,  // STATUS_UNALIGNED_OPERATION
	EIEIO,   // STATUS_NOT_THIS_FILE_SYSTEM, should never be seen
	0,       // STATUS_END_OF_FILE, not a failure
	EAGAIN,  // STATUS_BLOCKING_OPERATION, equal to EWOULDBLOCK
	
	ENOMEM,  // STATUS_INSUFFICIENT_VA_SPACE
	EINVAL,  // STATUS_VA_NOT_AT_BASE
	EINVAL,  // STATUS_MEMORY_NOT_RESERVED
	EINVAL,  // STATUS_MEMORY_COMMITTED
	EINVAL,  // STATUS_CONFLICTING_ADDRESSES
	
	ENOEXEC, // STATUS_INVALID_EXECUTABLE
	ENOEXEC, // STATUS_INVALID_ARCHITECTURE
	
	EAGAIN,  // STATUS_STILL_RUNNING
};

int TranslateStatus(BSTATUS Status)
{
	if (Status >= STATUS_RANGE_ABANDONED_WAIT &&
		Status < STATUS_RANGE_ABANDONED_WAIT + MAXIMUM_WAIT_BLOCKS)
		return EOWNERDEAD;
		
	if (Status >= STATUS_RANGE_WAIT &&
		Status < STATUS_RANGE_WAIT + MAXIMUM_WAIT_BLOCKS)
		return 0;
	
	if (Status < 0 || Status >= STATUS_MAX)
		return EIEIO;
	
	return g_statusToErrno[Status];
}

void sys_libc_log(const char* message)
{
	OSOutputDebugString(message, strlen(message));
}

[[noreturn]]
void sys_libc_panic()
{
	OSOutputDebugString("mlibc panic!!");
	OSExitProcess(1);
}

int sys_tcb_set(void* pointer)
{
	OSSetCurrentTeb(pointer);
}

int sys_futex_wait(int *pointer, int expected, const struct timespec *time)
{
	sys_libc_log("sys_futex_wait NYI\n");
	return -1;
}

int sys_futex_wake(int *pointer)
{
	sys_libc_log("sys_futex_wake NYI\n");
	return -1;
}

int sys_anon_allocate(size_t size, void** pointer)
{
	size_t Size = size;
	
	BSTATUS Status = OSAllocateVirtualMemory(
		CURRENT_PROCESS_HANDLE,
		pointer,
		&Size,
		MEM_COMMIT | MEM_RESERVE,
		PAGE_READ | PAGE_WRITE
	);
	
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	return 0;
}

int sys_anon_free(void* pointer, size_t size)
{
	BSTATUS Status = OSFreeVirtualMemory(
		CURRENT_PROCESS_HANDLE,
		pointer,
		size,
		MEM_RELEASE
	);
	
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	return 0;
}

// NOTE: DO NOT enter g_fileTableCS *after* you enter the CS of a file!

typedef struct
{
	bool Occupied;
	HANDLE Handle;
	uint64_t Offset;
	OS_CRITICAL_SECTION Lock;
}
OPEN_FILE, *POPEN_FILE;

static OPEN_FILE g_fileTable[MAX_FDS];
static OS_CRITICAL_SECTION g_fileTableCS;
static thread_local HANDLE g_currentDirectory = HANDLE_NONE;

__attribute__((constructor))
static void InitializeFileTableCS()
{
	BSTATUS Status = OSInitializeCriticalSection(&g_fileTableCS);
	if (FAILED(Status))
		sys_libc_panic();
}

static BSTATUS AllocateFD(int* FdOut)
{
	OSEnterCriticalSection(&g_fileTableCS);
	
	for (int i = 0; i < MAX_FDS; i++)
	{
		if (g_fileTable[i].Occupied)
			continue;
		
		BSTATUS Status = OSInitializeCriticalSection(g_fileTable[i].Lock);
		if (FAILED(Status))
		{
			OSLeaveCriticalSection(&g_fileTableCS);
			return Status;
		}
		
		// critical section initialized, mark as occupied and return
		g_fileTable[i].Occupied = true;
		g_fileTable[i].Offset = 0;
		
		*FdOut = i;
		return STATUS_SUCCESS;
	}
	
	return STATUS_TOO_MANY_HANDLES;
}

static void ReleaseFD(int Fd)
{
	if (Fd < 0 || Fd >= MAX_FDS) {
		sys_libc_log("ReleaseFD: out of range for Fd!\n");
		sys_libc_panic();
	}
	
	OSEnterCriticalSection(&g_fileTableCS);
	
	// Mark the place as not occupied.
	OSEnterCriticalSection(&g_fileTable[Fd].Lock);
	g_fileTable[i].Occupied = false;
	OSLeaveCriticalSection(&g_fileTable[Fd].Lock);
	
	// After this song and dance, any read() or write()
	// on this FD should fail with STATUS_INVALID_HANDLE.
	//
	// However, we still have the global file table locked,
	// so no open() operations can go through either.
	
	if (g_fileTable[Fd].Handle != HANDLE_NONE)
		OSClose(g_fileTable[Fd].Handle);
	
	g_fileTable[Fd].Handle = HANDLE_NONE;
	g_fileTable[Fd].Offset = 0;
	
	// Deinitialize this file table's critical section
	OSDeleteCriticalSection(&g_fileTable[Fd].Lock);
	
	OSLeaveCriticalSection(&g_fileTableCS);
}

// This exits with the output file pointer locked, if it succeeds.
static BSTATUS FindFileByFD(int Fd, POPEN_FILE* OutFile)
{
	if (Fd < 0 || Fd >= MAX_FDS)
		return STATUS_INVALID_HANDLE;
	
	OSEnterCriticalSection(&g_fileTableCS);
	
	if (!g_fileTable[i].Occupied)
	{
		OSLeaveCriticalSection(&g_fileTableCS);
		return STATUS_INVALID_HANDLE;
	}
	
	OSEnterCriticalSection(&g_fileTable[i].Lock);
	OSLeaveCriticalSection(&g_fileTableCS);
	
	*OutFile = &g_fileTable[i];
	return STATUS_SUCCESS;
}

int sys_open(const char* pathname, int flags, mode_t mode, int* fd)
{
	BSTATUS Status;
	int Fd = 0;
	
	Status = AllocateFD(&Fd);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	// fd allocated, now open it
	OBJECT_ATTRIBUTES Attributes;
	Attributes.ObjectName = pathname;
	Attributes.ObjectNameLength = strlen(pathname);
	Attributes.RootDirectory = g_currentDirectory;
	Attributes.OpenFlags = 0;
	
	// TODO: flags and mode ignored
	
	Status = OSOpenFile(&Handle, &Attributes);
	if (FAILED(Status))
	{
		ReleaseFD(Fd);
		return TranslateStatus(Status);
	}
}


} // namespace mlibc
