#include <boron/boron.h>
#include <boron/svcs.h>

#include <errno.h>
#include <string.h>

#include <abi-bits/mode_t.h>
#include <abi-bits/seek-whence.h>
#include <abi-bits/vm-flags.h>
#include <abi-bits/stat.h>
#include <abi-bits/pid_t.h>
#include <abi-bits/wait.h>

#include <abis/linux/resource.h>

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

// TODO: increase this limit.  But currently, the scheduler allows you to wait for up to 64 items at a time.
constexpr int MAX_CHILD_PROCESSES = MAXIMUM_WAIT_BLOCKS;

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
	EISDIR,  // STATUS_IS_A_DIRECTORY
	EIO,     // STATUS_HARDWARE_IO_ERROR
	EINVAL,  // STATUS_UNALIGNED_OPERATION
	EIEIO,   // STATUS_NOT_THIS_FILE_SYSTEM, should never be seen
	0,       // STATUS_END_OF_FILE, not a failure
	EAGAIN,  // STATUS_BLOCKING_OPERATION, equal to EWOULDBLOCK
	ENOTEMPTY, // STATUS_DIRECTORY_NOT_EMPTY
	EIEIO,   // STATUS_OUT_OF_FILE_BOUNDS, not used for userspace
	ENOTTY,  // STATUS_NOT_A_TERMINAL
	
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
	if (SUCCEEDED(Status))
		return 0;
	
	mlibc::infoLogger() << "mlibc::TranslateStatus(" << Status << "), RA: " << __builtin_return_address(0) << "." << frg::endlog;
	
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

// this function is called later

#endif

static void InitializeFileTableCS()
{
	BSTATUS Status = OSInitializeCriticalSection(&FileTableLock);
	if (FAILED(Status)) {
		sys_libc_log("ERROR: Cannot initialize file table lock.\n");
		sys_libc_panic();
	}
}

#ifndef MLIBC_BUILDING_RTLD

static void AssignStandardIOPointers()
{
	PPEB Peb = (PPEB) OSGetCurrentPeb();
	FileTable[0] = Peb->StandardIO[0];
	FileTable[1] = Peb->StandardIO[1];
	FileTable[2] = Peb->StandardIO[2];
}

#endif

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
	
	sys_libc_log("ERROR: too many handles opened!");
	OSLeaveCriticalSection(&FileTableLock);
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
	
	Status = OSClose(FileHandle);
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
	
	// MAP_FIXED allows overwriting of mmap regions.
	if (flags & MAP_FIXED)
		AllocationType |= MEM_FIXED | MEM_OVERRIDE;
	
#ifdef MAP_FIXED_NOREPLACE
	// MAP_FIXED_NOREPLACE doesn't allow overwriting mmap regions.
	if (flags & MAP_FIXED_NOREPLACE)
		AllocationType |= MEM_FIXED;
#endif
	
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
		
		if (FAILED(Status) && Status == STATUS_CONFLICTING_ADDRESSES && (~flags & MAP_FIXED))
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

int sys_isatty(int fd)
{
	HANDLE FileHandle = HANDLE_NONE;
	BSTATUS Status = FindFileByFD(fd, &FileHandle);
	if (FAILED(Status))
		return TranslateStatus(Status);
	
	Status = OSCheckIsTerminalFile(FileHandle);
	return TranslateStatus(Status);
}

int sys_sleep(time_t* psecs, long* pnanos)
{
	time_t secs = *psecs;
	long   nanos = *pnanos;
	
	// TODO: avoid overflow here
	int time = (int)(secs * 1000 + nanos / 1000000);
	BSTATUS Status = OSSleep(time);
	
	return TranslateStatus(Status);
}

static OS_CRITICAL_SECTION ChildProcessTableLock;
static HANDLE ChildProcessTable[MAX_CHILD_PROCESSES];
static int ChildProcessCount = 0;

void InitializeProcessTable()
{
	BSTATUS Status = OSInitializeCriticalSection(&ChildProcessTableLock);
	if (FAILED(Status)) {
		sys_libc_log("ERROR: Cannot initialize process table lock.\n");
		sys_libc_panic();
	}
}

int sys_fork(pid_t* outChildPid)
{
	OSEnterCriticalSection(&ChildProcessTableLock);
	if (ChildProcessCount >= MAX_CHILD_PROCESSES)
	{
		sys_libc_log("sys_fork: Reached child process limit.\n");
		OSLeaveCriticalSection(&ChildProcessTableLock);
		return EAGAIN;
	}
	
	HANDLE OutChildHandle = HANDLE_NONE;
	BSTATUS Status = OSForkProcessInternal(&OutChildHandle);
	
	if (Status == STATUS_IS_CHILD_PROCESS)
	{
		*outChildPid = 0;
		
		// Since we're inside the child process, we need to erase the child process
		// table.  waitpid(-1) shouldn't work inside the child process just as it
		// would inside the parent.
		for (int i = 0; i < ChildProcessCount; i++)
		{
			OSClose(ChildProcessTable[i]);
			ChildProcessTable[i] = 0;
		}
		
		ChildProcessCount = 0;
		
		OSLeaveCriticalSection(&ChildProcessTableLock);
		return 0;
	}
	
	if (SUCCEEDED(Status))
	{
		// TODO: Does forking *really* depend on a globally available ID?
		// If so, then we *might* just want to actually implement it the
		// proper way.
		
		// NOTE: adding +1 here because pid == 0 means that we're inside the
		// child process.
		*outChildPid = ChildProcessCount + 1;
		ChildProcessTable[ChildProcessCount++] = OutChildHandle;
	}
	
	OSLeaveCriticalSection(&ChildProcessTableLock);
	return TranslateStatus(Status);
}

int sys_waitpid(pid_t pid, int* out_status, int flags, struct rusage* ru, pid_t* ret_pid)
{
	BSTATUS Status = STATUS_SUCCESS;
	
	if (ru) {
		mlibc::infoLogger() << "mlibc: sys_waitpid: struct rusage is unsupported" << frg::endlog;
		return ENOSYS;
	}
	
	*ret_pid = -1;
	
	if (ChildProcessCount == 0) {
		return ECHILD;
	}
	
	int Timeout = WAIT_TIMEOUT_INFINITE;
	
	if (flags & WNOHANG) {
		// poll mode
		Timeout = 0;
	}
	
	if (pid < 0)
	{
		// TODO: process group ID implementation
		// for now just wait for any of the pids
		pid = 0;
	}
	
	HANDLE WaitedHandles[MAX_CHILD_PROCESSES];
	HANDLE OriginalHandles[MAX_CHILD_PROCESSES];
	int WaitedHandleCount = 0;
	
	OSEnterCriticalSection(&ChildProcessTableLock);
	if (pid == 0)
	{
		for (int i = 0; i < ChildProcessCount; i++)
		{
			OriginalHandles[i] = ChildProcessTable[i];
			Status = OSDuplicateHandle(ChildProcessTable[i], CURRENT_PROCESS_HANDLE, &WaitedHandles[i], 0);
			if (SUCCEEDED(Status))
				continue;

			mlibc::infoLogger() << "mlibc: sys_waitpid: (multi-wait) OSDuplicateHandle failed with status " << Status << frg::endlog;
			WaitedHandleCount = i;
			OSLeaveCriticalSection(&ChildProcessTableLock);
			goto CloseEverythingAndFail;
		}
		
		WaitedHandleCount = ChildProcessCount;
	}
	else if (pid > ChildProcessCount)
	{
		OSLeaveCriticalSection(&ChildProcessTableLock);
		return ECHILD;
	}
	else
	{
		OriginalHandles[0] = ChildProcessTable[pid - 1];
		Status = OSDuplicateHandle(ChildProcessTable[pid - 1], CURRENT_PROCESS_HANDLE, &WaitedHandles[0], 0);
		if (FAILED(Status))
		{
			mlibc::infoLogger() << "mlibc: sys_waitpid: (single-wait) OSDuplicateHandle failed with status " << Status << frg::endlog;
			OSLeaveCriticalSection(&ChildProcessTableLock);
			goto CloseEverythingAndFail;
		}
		
		WaitedHandleCount = 1;
	}
	
	OSLeaveCriticalSection(&ChildProcessTableLock);
	
	// Handles duplicated, now wait for a process to exit.
	Status = OSWaitForMultipleObjects(
		WaitedHandleCount,
		WaitedHandles,
		WAIT_ANY_OBJECT,
		true,
		Timeout
	);
	
	if (Status >= STATUS_RANGE_WAIT && Status < STATUS_RANGE_WAIT + MAXIMUM_WAIT_BLOCKS)
	{
		// This specific process has exited, so take a look at which.
		int ProcessIndex = Status - STATUS_RANGE_WAIT;
		HANDLE Process = WaitedHandles[ProcessIndex];
		
		int ExitCode = 1;
		Status = OSGetExitCodeProcess(Process, &ExitCode);
		if (FAILED(Status)) {
			mlibc::infoLogger() << "mlibc: sys_waitpid: OSGetExitCodeProcess returned status " << RtlGetStatusString(Status) << frg::endlog;
		}
		
		// TODO: Convert exit codes into POSIX-compatible status codes inspectable
		// with macros such as WIFEXITED, WEXITSTATUS etc.
		
		// According to the Linux ABI, the status code is composed of the following:
		// - 8 bits: Signal number (or 0 if exited normally)
		// - 8 bits: Return value from main() (if signal number is 0)
		//
		// Additionally status == 0xFFFF if the process is "continued", and the signal
		// number is 0x7F if the process is "stopped".  We'll implement signals later.
		int OutStatus = ExitCode & 0xFF;
		if (ExitCode && !OutStatus)
			OutStatus = 1;
		
		// TODO: the PID could be outdated if they forked again
		*out_status = OutStatus << 8;
		*ret_pid = ProcessIndex + 1;
		Status = STATUS_SUCCESS;
		
		// Now we need to remove the child process from the child process table.
		OSEnterCriticalSection(&ChildProcessTableLock);
		
		bool Removed = false;
		for (int i = 0; i < ChildProcessCount; i++)
		{
			if (ChildProcessTable[i] != OriginalHandles[ProcessIndex])
				continue;
			
			Removed = true;
			ChildProcessTable[i] = ChildProcessTable[ChildProcessCount - 1];
			ChildProcessCount--;
		}
		
		OSClose(OriginalHandles[ProcessIndex]);
		
		if (!Removed)
			mlibc::infoLogger() << "mlibc: sys_waitpid: Child process table was mutated. Can't find the original handle anymore." << frg::endlog;
		
		OSLeaveCriticalSection(&ChildProcessTableLock);
	}
	
CloseEverythingAndFail:
	for (int i = 0; i < WaitedHandleCount; i++)
		OSClose(WaitedHandles[i]);
	
	return TranslateStatus(Status);
}

#endif

} // namespace mlibc

#ifndef MLIBC_BUILDING_RTLD

extern "C" void __InitializeLibrary()
{
	mlibc::InitializeFileTableCS();
	mlibc::AssignStandardIOPointers();
	mlibc::InitializeProcessTable();
}

#endif
