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

#include <mlibc/debug.hpp>

#ifdef MLIBC_BUILDING_RTLD
#define THREAD_LOCAL_COND
#else
#define THREAD_LOCAL_COND thread_local
#endif

namespace mlibc {

#ifdef MLIBC_BUILDING_RTLD
constexpr int MAX_FDS = 64; // RTLD doesn't need that many files.  Increase if you need more than like 64 libraries
#else
constexpr int MAX_FDS = 1024;
#endif

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
	OSOutputDebugString("\n", 1);
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
	
	void* Pointer = NULL;
	BSTATUS Status = OSAllocateVirtualMemory(
		CURRENT_PROCESS_HANDLE,
		&Pointer,
		&Size,
		MEM_COMMIT | MEM_RESERVE,
		PAGE_READ | PAGE_WRITE
	);
	
	if (FAILED(Status))
	{
	#ifdef MLIBC_BUILDING_RTLD
		sys_libc_log("Interpreter sys_anon_allocate fail\n");
	#else
		sys_libc_log("Libc sys_anon_allocate fail\n");
	#endif
	}
	
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	*pointer = Pointer;
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

static HANDLE FileTable[MAX_FDS];
static OS_CRITICAL_SECTION FileTableLock;
static THREAD_LOCAL_COND HANDLE g_currentDirectory = HANDLE_NONE;

#ifdef MLIBC_BUILDING_RTLD

static bool FileTableInitialized = false;

#define INITIALIZE_FTL_IF_NEEDED() do { \
	if (!FileTableInitialized) {     \
		FileTableInitialized = true; \
		InitializeFileTableCS();     \
	}                                \
} while (0)

#else

#define INITIALIZE_FTL_IF_NEEDED()

// constructor attribute applies to the below function
__attribute__((constructor))

#endif

static void InitializeFileTableCS()
{
	BSTATUS Status = OSInitializeCriticalSection(&FileTableLock);
	if (FAILED(Status))
		sys_libc_panic();
}

// This exits with the output file pointer locked, if it succeeds.
static BSTATUS AllocateFD(int* FdOut, HANDLE Handle)
{
	INITIALIZE_FTL_IF_NEEDED();
	OSEnterCriticalSection(&FileTableLock);
	
	for (int i = 0; i < MAX_FDS; i++)
	{
		if (FileTable[i] != HANDLE_NONE)
			continue;
		
		// critical section initialized, mark as occupied and return
		FileTable[i] = Handle;
		*FdOut = i;
		
		OSLeaveCriticalSection(&FileTableLock);
		return STATUS_SUCCESS;
	}
	
	return STATUS_TOO_MANY_HANDLES;
}

static BSTATUS ReleaseFD(int Fd)
{
	INITIALIZE_FTL_IF_NEEDED();
	if (Fd < 0 || Fd >= MAX_FDS)
		return STATUS_INVALID_HANDLE;
	
	OSEnterCriticalSection(&FileTableLock);
	if (FileTable[Fd] == HANDLE_NONE)
	{
		OSLeaveCriticalSection(&FileTableLock);
		return STATUS_INVALID_HANDLE;
	}
	
	FileTable[Fd] = 0;
	OSLeaveCriticalSection(&FileTableLock);
	return STATUS_SUCCESS;
}

// This exits with the output file pointer locked, if it succeeds.
static BSTATUS FindFileByFD(int Fd, PHANDLE OutHandle)
{
	INITIALIZE_FTL_IF_NEEDED();
	if (Fd < 0 || Fd >= MAX_FDS)
		return STATUS_INVALID_HANDLE;
	
	OSEnterCriticalSection(&FileTableLock);
	
	if (FileTable[Fd] == HANDLE_NONE)
	{
		OSLeaveCriticalSection(&FileTableLock);
		return STATUS_INVALID_HANDLE;
	}
	
	*OutHandle = FileTable[Fd];
	OSLeaveCriticalSection(&FileTableLock);
	return STATUS_SUCCESS;
}

int sys_open(const char* pathname, int flags, mode_t mode, int* fd)
{
	INITIALIZE_FTL_IF_NEEDED();
	BSTATUS Status;
	int Fd = 0;
	
	// fd allocated, now open it
	OBJECT_ATTRIBUTES Attributes;
	Attributes.ObjectName = pathname;
	Attributes.ObjectNameLength = strlen(pathname);
	Attributes.RootDirectory = g_currentDirectory;
	Attributes.OpenFlags = 0;
	
	// TODO: flags and mode ignored for now
	(void) flags;
	(void) mode;
	
	HANDLE Handle;
	Status = OSOpenFile(&Handle, &Attributes);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	Status = AllocateFD(&Fd, Handle);
	if (FAILED(Status))
	{
		OSClose(Handle);
		return TranslateStatus(Status);
	}
	
	*fd = Fd;
	return 0;
}

int sys_read(int fd, void* buf, size_t count, ssize_t* bytes_read)
{
	INITIALIZE_FTL_IF_NEEDED();
	HANDLE FileHandle = HANDLE_NONE;
	BSTATUS Status = FindFileByFD(fd, &FileHandle);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	IO_STATUS_BLOCK Iosb;
	Status = OSReadFile(&Iosb, FileHandle, 0, buf, count, IO_RW_SHARED_FILE_OFFSET);
	if (IOSUCCEEDED(Status))
		*bytes_read = Iosb.BytesRead;
	
	return TranslateStatus(Status);
}

#ifndef MLIBC_BUILDING_RTLD

int sys_write(int fd, const void* buf, size_t count, ssize_t* bytes_written)
{
	INITIALIZE_FTL_IF_NEEDED();
	HANDLE FileHandle = HANDLE_NONE;
	BSTATUS Status = FindFileByFD(fd, &FileHandle);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	IO_STATUS_BLOCK Iosb;
	Status = OSWriteFile(&Iosb, FileHandle, 0, buf, count, IO_RW_SHARED_FILE_OFFSET, NULL);
	if (IOSUCCEEDED(Status))
		*bytes_written = Iosb.BytesWritten;
	
	return TranslateStatus(Status);
}

#endif

int sys_seek(int fd, off_t offset, int whence, off_t* new_offset)
{
	INITIALIZE_FTL_IF_NEEDED();
	HANDLE FileHandle = HANDLE_NONE;
	BSTATUS Status = FindFileByFD(fd, &FileHandle);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	int Whence = -1;
	switch (whence) {
		case SEEK_CUR:
			Whence = IO_SEEK_CUR;
			break;
		case SEEK_SET:
			Whence = IO_SEEK_SET;
			break;
		case SEEK_END:
			Whence = IO_SEEK_END;
			break;
	}
	
	if (Whence == -1)
		return TranslateStatus(STATUS_INVALID_PARAMETER);
	
	uint64_t NewOffset = 0;
	Status = OSSeekFile(FileHandle, (int64_t) offset, Whence, &NewOffset);
	
	if (!FAILED(Status))
		*new_offset = (off_t) NewOffset;
	
	return TranslateStatus(Status);
}

int sys_close(int fd)
{
	INITIALIZE_FTL_IF_NEEDED();
	HANDLE FileHandle = HANDLE_NONE;
	BSTATUS Status = FindFileByFD(fd, &FileHandle);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	Status = OSClose(Status);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	ReleaseFD(fd);
	return Status;
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
	mlibc::infoLogger() << "sys_vm_map(" << hint << ", " << size << ", " << prot << ", "
		<< flags << ", " << fd << ", " << offset << ", " << window << ")" << frg::endlog;
	INITIALIZE_FTL_IF_NEEDED();
	
	// Note: MAP_FIXED not supported in a complete way, because overwriting mappings
	// is not allowed.  This isn't POSIX compliant.
	void* BaseAddress = hint;
	size_t ViewSize = size;
	BSTATUS Status = STATUS_SUCCESS;
	if (~flags & MAP_ANONYMOUS)
	{
		HANDLE FileHandle;
		Status = FindFileByFD(fd, &FileHandle);
		if (FAILED(Status))
		{
			mlibc::infoLogger() << "\tfailed with status " << Status << frg::endlog;
			return TranslateStatus(Status);
		}
		
		// first, try to map while specifying the hint
		Status = OSMapViewOfObject(
			CURRENT_PROCESS_HANDLE,
			FileHandle,
			&BaseAddress,
			size,
			FlagsToAllocationType(flags),
			offset,
			ConvertProtection(prot)
		);
		
		if (FAILED(Status) && Status == STATUS_CONFLICTING_ADDRESSES && (~flags & MAP_FIXED))
		{
			mlibc::infoLogger() << "\tfailed with status " << Status << ", trying without the hint" << frg::endlog;
			
			// try without specifying the hint
			BaseAddress = NULL;
			
			Status = OSMapViewOfObject(
				CURRENT_PROCESS_HANDLE,
				FileHandle,
				&BaseAddress,
				size,
				FlagsToAllocationType(flags),
				offset,
				ConvertProtection(prot)
			);
		}
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
		
		if (FAILED(Status) && Status == STATUS_CONFLICTING_ADDRESSES)
		{
			// try without specifying the hint
			BaseAddress = NULL;
			
			mlibc::infoLogger() << "\tfailed with status " << Status << ", trying without the hint" << frg::endlog;
			
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
	{
		mlibc::infoLogger() << "\tfailed with status " << Status << frg::endlog;
		return TranslateStatus(Status);
	}
	
	*window = BaseAddress;
	return 0;
}

#ifndef MLIBC_BUILDING_RTLD

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
