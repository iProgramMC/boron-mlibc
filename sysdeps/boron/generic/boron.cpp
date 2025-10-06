#include <boron/boron.h>
#include <boron/svcs.h>

#include <errno.h>
#include <string.h>

#include <abi-bits/mode_t.h>
#include <abi-bits/seek-whence.h>
#include <abi-bits/vm-flags.h>
#include <abi-bits/stat.h>

#include <bits/off_t.h>
#include <bits/ssize_t.h>

#include <mlibc/fsfd_target.hpp>

typedef struct
{
	bool Occupied;
	HANDLE Handle;
	uint64_t Offset;
	OS_CRITICAL_SECTION Lock;
}
OPEN_FILE, *POPEN_FILE;

#ifdef MLIBC_BUILDING_RTLD
#define THREAD_LOCAL_COND
#else
#define THREAD_LOCAL_COND thread_local
#endif

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

constexpr int TranslateStatus(BSTATUS Status)
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
	sys_libc_log("mlibc panic!!\n");
	OSExitProcess(1);
}

int sys_tcb_set(void* pointer)
{
	OSSetCurrentTeb(pointer);
	return 0;
}

int sys_futex_wait(int *pointer, int expected, const struct timespec *time)
{
	(void) pointer;
	(void) expected;
	(void) time;
	
	sys_libc_log("sys_futex_wait NYI\n");
	return -1;
}

int sys_futex_wake(int *pointer)
{
	(void) pointer;
	
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

static OPEN_FILE g_fileTable[MAX_FDS];
static OS_CRITICAL_SECTION g_fileTableCS;
static THREAD_LOCAL_COND HANDLE g_currentDirectory = HANDLE_NONE;

__attribute__((constructor))
static void InitializeFileTableCS()
{
	BSTATUS Status = OSInitializeCriticalSection(&g_fileTableCS);
	if (FAILED(Status))
		sys_libc_panic();
}

// This exits with the output file pointer locked, if it succeeds.
static BSTATUS AllocateFD(int* FdOut)
{
	OSEnterCriticalSection(&g_fileTableCS);
	
	for (int i = 0; i < MAX_FDS; i++)
	{
		if (g_fileTable[i].Occupied)
			continue;
		
		BSTATUS Status = OSInitializeCriticalSection(&g_fileTable[i].Lock);
		if (FAILED(Status))
		{
			OSLeaveCriticalSection(&g_fileTableCS);
			return Status;
		}
		
		// critical section initialized, mark as occupied and return
		g_fileTable[i].Occupied = true;
		g_fileTable[i].Offset = 0;
		
		*FdOut = i;
		
		OSEnterCriticalSection(&g_fileTable[i].Lock);
		OSLeaveCriticalSection(&g_fileTableCS);
		
		return STATUS_SUCCESS;
	}
	
	return STATUS_TOO_MANY_HANDLES;
}

static BSTATUS ReleaseFD(int Fd)
{
	if (Fd < 0 || Fd >= MAX_FDS)
		return STATUS_INVALID_HANDLE;
	
	OSEnterCriticalSection(&g_fileTableCS);
	
	// Mark the place as not occupied.
	OSEnterCriticalSection(&g_fileTable[Fd].Lock);
	if (!g_fileTable[Fd].Occupied)
	{
		OSLeaveCriticalSection(&g_fileTable[Fd].Lock);
		OSLeaveCriticalSection(&g_fileTableCS);
		return STATUS_INVALID_HANDLE;
	}
	
	g_fileTable[Fd].Occupied = false;
	OSLeaveCriticalSection(&g_fileTable[Fd].Lock);
	
	// After this song and dance, any read() or write()
	// on this FD should fail with STATUS_INVALID_HANDLE.
	//
	// However, we still have the global file table locked,
	// so no open() operations can go through either.
	
	if (g_fileTable[Fd].Handle != HANDLE_NONE)
	{
		BSTATUS Status = OSClose(g_fileTable[Fd].Handle);
		if (FAILED(Status))
		{
			sys_libc_log("ERROR: OSClose returned a failure code!\n");
			sys_libc_panic();
		}
	}
	
	g_fileTable[Fd].Handle = HANDLE_NONE;
	g_fileTable[Fd].Offset = 0;
	
	// Deinitialize this file table's critical section
	OSDeleteCriticalSection(&g_fileTable[Fd].Lock);
	
	OSLeaveCriticalSection(&g_fileTableCS);
	return STATUS_SUCCESS;
}

// This exits with the output file pointer locked, if it succeeds.
static BSTATUS FindFileByFD(int Fd, POPEN_FILE* OutFile)
{
	if (Fd < 0 || Fd >= MAX_FDS)
		return STATUS_INVALID_HANDLE;
	
	OSEnterCriticalSection(&g_fileTableCS);
	
	if (!g_fileTable[Fd].Occupied)
	{
		OSLeaveCriticalSection(&g_fileTableCS);
		return STATUS_INVALID_HANDLE;
	}
	
	OSEnterCriticalSection(&g_fileTable[Fd].Lock);
	OSLeaveCriticalSection(&g_fileTableCS);
	
	*OutFile = &g_fileTable[Fd];
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
	
	// TODO: flags and mode ignored for now
	(void) flags;
	(void) mode;
	
	POPEN_FILE File = &g_fileTable[Fd];
	
	HANDLE Handle;
	Status = OSOpenFile(&Handle, &Attributes);
	if (FAILED(Status))
	{
		OSLeaveCriticalSection(&File->Lock);
		ReleaseFD(Fd);
		return TranslateStatus(Status);
	}
	
	// file opened, now initialize everything and return
	File->Handle = Handle;
	File->Offset = 0;
	File->Occupied = true;
	OSLeaveCriticalSection(&File->Lock);
	
	*fd = Fd;
	return 0;
}

int sys_read(int fd, void* buf, size_t count, ssize_t* bytes_read)
{
	POPEN_FILE File = NULL;
	BSTATUS Status = FindFileByFD(fd, &File);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	IO_STATUS_BLOCK Iosb;
	Status = OSReadFile(&Iosb, File->Handle, File->Offset, buf, count, 0);
	if (IOSUCCEEDED(Status))
	{
		*bytes_read = Iosb.BytesRead;
		File->Offset += Iosb.BytesRead;
	}
	
	OSLeaveCriticalSection(&File->Lock);
	return TranslateStatus(Status);
}

int sys_write(int fd, const void* buf, size_t count, ssize_t* bytes_read)
{
	POPEN_FILE File = NULL;
	BSTATUS Status = FindFileByFD(fd, &File);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	IO_STATUS_BLOCK Iosb;
	Status = OSWriteFile(&Iosb, File->Handle, File->Offset, buf, count, 0, NULL);
	if (IOSUCCEEDED(Status))
	{
		*bytes_read = Iosb.BytesWritten;
		File->Offset += Iosb.BytesWritten;
	}
	
	OSLeaveCriticalSection(&File->Lock);
	return TranslateStatus(Status);
}

int sys_seek(int fd, off_t offset, int whence, off_t* new_offset)
{
	POPEN_FILE File = NULL;
	BSTATUS Status = FindFileByFD(fd, &File);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	uint64_t Length = 0;
	switch (whence)
	{
		case SEEK_SET:
			File->Offset = offset;
			break;
		
		case SEEK_CUR:
			File->Offset += offset;
			break;
		
		case SEEK_END:
			Status = OSGetLengthFile(File->Handle, &Length);
			if (!IOFAILED(Status))
				File->Offset = (off_t)Length + offset;
			
			break;
		
		default:
			Status = STATUS_INVALID_PARAMETER;
			break;
	}
	
	if (!FAILED(Status))
		*new_offset = File->Offset;
	
	OSLeaveCriticalSection(&File->Lock);
	return TranslateStatus(Status);
}

int sys_close(int fd)
{
	return TranslateStatus(ReleaseFD(fd));
}

constexpr int FlagsToAllocationType(int flags)
{
	int AllocationType = MEM_RESERVE | MEM_COMMIT;
	
	// by default memory is private.
	if (flags & MAP_SHARED) AllocationType |= MEM_SHARED;
	
	return AllocationType;
}

constexpr int ConvertProtection(int prot)
{
	// TODO: For now, RWX everything.  I will implement sys_vm_protect later
	(void) prot;
	return PAGE_READ | PAGE_WRITE | PAGE_EXECUTE;
/*
	int Protection = 0;
	if (prot & PROT_READ)  Protection |= PAGE_READ;
	if (prot & PROT_WRITE) Protection |= PAGE_WRITE;
	if (prot & PROT_EXEC)  Protection |= PAGE_EXECUTE;
	return Protection;
*/
}

int sys_vm_map(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window)
{
	// MAP_FIXED not supported, because fixed mapping is not implemented
	// in a POSIX compliant way.  Use mobile mmap instead.
	if (flags & MAP_FIXED)
		return TranslateStatus(STATUS_UNIMPLEMENTED);
	
	void* BaseAddress = hint;
	size_t ViewSize = size;
	BSTATUS Status = STATUS_SUCCESS;
	if (~flags & MAP_ANONYMOUS)
	{
		POPEN_FILE OpenFile = NULL;
		Status = FindFileByFD(fd, &OpenFile);
		if (FAILED(Status))
			return TranslateStatus(Status);
		
		// first, try to map while specifying the hint
		Status = OSMapViewOfObject(
			CURRENT_PROCESS_HANDLE,
			OpenFile->Handle,
			&BaseAddress,
			size,
			FlagsToAllocationType(flags),
			offset,
			ConvertProtection(prot)
		);
		
		if (FAILED(Status) && Status != STATUS_INVALID_HANDLE)
		{
			// try without specifying the hint
			BaseAddress = NULL;
			
			Status = OSMapViewOfObject(
				CURRENT_PROCESS_HANDLE,
				OpenFile->Handle,
				&BaseAddress,
				size,
				FlagsToAllocationType(flags),
				offset,
				ConvertProtection(prot)
			);
		}
		
		OSLeaveCriticalSection(&OpenFile->Lock);
	}
	else
	{
		Status = OSAllocateVirtualMemory(
			CURRENT_PROCESS_HANDLE,
			&BaseAddress,
			&ViewSize,
			FlagsToAllocationType(flags),
			ConvertProtection(prot)
		);
		
		if (FAILED(Status) && Status != STATUS_INVALID_HANDLE)
		{
			// try without specifying the hint
			BaseAddress = NULL;
			
			Status = OSAllocateVirtualMemory(
				CURRENT_PROCESS_HANDLE,
				&BaseAddress,
				&ViewSize,
				FlagsToAllocationType(flags),
				ConvertProtection(prot)
			);
		}
	}
	
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	*window = BaseAddress;
	return 0;
}

int sys_vm_unmap(void* pointer, size_t size)
{
	// TODO: Allow partial unmapping.
	BSTATUS Status = OSFreeVirtualMemory(
		CURRENT_PROCESS_HANDLE,
		pointer,
		size,
		MEM_RELEASE
	);
	
	return TranslateStatus(Status);
}

int sys_vm_protect(void* pointer, size_t size, int prot)
{
	// TODO: OSChangeProtectionVirtualMemory or something
	(void) pointer;
	(void) size;
	(void) prot;
	
	return 0;
}

#ifndef MLIBC_BUILDING_RTLD

void sys_exit(int code)
{
	OSExitProcess(code);
}

int sys_clock_get(int clock, time_t* secs, long* nanos)
{
	(void) clock;
	
	// TODO: Implement RTC time inside the Boron kernel and get it as a syscall here.
	// For now, hardcode 1 January 2025
	constexpr time_t BaseDate = 1735689600;
	
	uint64_t ticks = 0, frequency = 1;
	OSGetTickCount(&ticks);
	OSGetTickFrequency(&frequency);
	
	uint64_t seconds = ticks / frequency;
	*secs = BaseDate + (time_t) seconds;
	
	ticks -= seconds * frequency;
	ticks *= 1000000000;
	*nanos = (long)(ticks / frequency);
	return 0;
}

#endif

} // namespace mlibc
