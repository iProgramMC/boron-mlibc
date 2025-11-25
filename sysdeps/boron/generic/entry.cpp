#include <stdint.h>
#include <stdlib.h>
#include <bits/ensure.h>
#include <mlibc/elf/startup.h>
#include <boron/boron.h>
#include <abi-bits/auxv.h>
#include <elf.h>
#include <string.h>

#ifdef MLIBC_BUILDING_RTLD

// Unlike strlen(), this counts the amount of characters in an environment
// variable description.  Environment variables are separated with "\0" and
// the final environment variable finishes with two "\0" characters.
//
// NOTE: This is also used for the command line arguments.
static size_t RtlEnvironmentLength(const char* Description)
{
	size_t Length = 2;
	while (Description[0] != '\0' || Description[1] != '\0') {
		Description++;
		Length++;
	}
	
	return Length;
}

// Counts the number of entries in a command line or environment description.
static int RtlEnvironmentCount(const char* Description)
{
	int EntryCount = 0;
	while (*Description)
	{
		// Skip over all of the data in the description.
		// Obviously, we only care about the amount of entries.
		while (*Description)
			Description++;
		
		// Skip over this null character and increment the entry count.
		//
		// If the double null has been noticed at this point, the loop will break.
		Description++;
		EntryCount++;
	}
	
	return EntryCount;
}

static int const AuxvEntries[] = {
	AT_PHDR,
	AT_PHENT,
	AT_PHNUM,
	AT_ENTRY,
	AT_PAGESZ,
	AT_UID,
	AT_EUID,
	AT_GID,
	AT_EGID,
	AT_NULL,
};

// Calculates the expected size of the stack's initial frame.
extern "C" size_t __mlibc_boron_calculateStackSize()
{
	// What the stack needs to contain:
	//
	// - Argument Count
	// - Argument List (list of pointers terminated by NULL)
	// - Environment List (list of pointers terminated by NULL)
	// - Auxiliary Values (after the environment list's final entry)
	// - The size of the content of each argument and environment variable.
	size_t Count = 0;
	
	PPEB Peb = (PPEB) OSGetCurrentPeb();
	Count += sizeof(uintptr_t); // argc
	Count += (RtlEnvironmentCount(Peb->CommandLine) + 1) * sizeof(uintptr_t); // argv
	Count += (RtlEnvironmentCount(Peb->Environment) + 1) * sizeof(uintptr_t); // envp
	Count += ARRAY_COUNT(AuxvEntries) * sizeof(uintptr_t) * 2; // auxv
	Count += sizeof(uintptr_t) * 2; // padding
	Count += Peb->CommandLineSize;
	Count += Peb->EnvironmentSize;
	
	// Align to 16 bytes.
	if (Count & 0xF)
		Count += 0x10 - (Count & 0xF);
	
	return Count;
}

#ifdef __x86_64__
using Elf_Ehdr = Elf64_Ehdr;
#endif

extern "C" void __mlibc_boron_prepareStack(uintptr_t* StackTop)
{
	PPEB Peb = (PPEB) OSGetCurrentPeb();
	size_t ArgumentCount = RtlEnvironmentCount(Peb->CommandLine);
	size_t EnvironmentCount = RtlEnvironmentCount(Peb->Environment);
	
	*StackTop++ = ArgumentCount;
	
	char** ArgumentList = (char**) StackTop;
	StackTop += ArgumentCount + 1;
	char** EnvironmentList = (char**) StackTop;
	StackTop += EnvironmentCount + 1;
	
	ArgumentList[ArgumentCount] = NULL;
	EnvironmentList[EnvironmentCount] = NULL;
	
	// Push auxv entries.
	Elf_Ehdr* ElfHeader = (Elf_Ehdr*) Peb->Loader.FileHeader;
	for (size_t i = 0; i < ARRAY_COUNT(AuxvEntries); i++)
	{
		uintptr_t Value = 0;
		switch (AuxvEntries[i])
		{
			case AT_PHDR:
				Value = (uintptr_t) Peb->Loader.ProgramHeaders;
				break;
			case AT_PHNUM:
				Value = ElfHeader->e_phnum;
				break;
			case AT_PHENT:
				Value = ElfHeader->e_phentsize;
				break;
			case AT_ENTRY:
				Value = Peb->Loader.ImageBase + ElfHeader->e_entry;
				break;
			case AT_PAGESZ:
				Value = 0x1000;
				break;
			case AT_BASE:
				Value = Peb->Loader.ImageBase;
				break;
			case AT_UID:
			case AT_EUID:
			case AT_GID:
			case AT_EGID:
				// TODO
				Value = 0;
				break;
			case AT_NULL:
				Value = 0;
				break;
		}
		
		*StackTop++ = AuxvEntries[i];
		*StackTop++ = Value;
	}
	
	if ((uintptr_t)StackTop & 0xF)
		StackTop++;
	
	char* ArgumentListValues = (char*) StackTop;
	memcpy(StackTop, Peb->CommandLine, Peb->CommandLineSize);
	StackTop += (Peb->CommandLineSize + sizeof(uintptr_t) - 1) / sizeof(uintptr_t);
	
	char* EnvironmentListValues = (char*) StackTop;
	memcpy(StackTop, Peb->Environment, Peb->EnvironmentSize);
	StackTop += (Peb->EnvironmentSize + sizeof(uintptr_t) - 1) / sizeof(uintptr_t);
	
	for (size_t i = 0; i < ArgumentCount; i++)
	{
		ArgumentList[i] = ArgumentListValues;
		ArgumentListValues += strlen(ArgumentListValues) + 1;
	}
	
	for (size_t i = 0; i < EnvironmentCount; i++)
	{
		EnvironmentList[i] = EnvironmentListValues;
		EnvironmentListValues += strlen(EnvironmentListValues) + 1;
	}
}

#else

extern "C" void __dlapi_enter(uintptr_t *);
extern "C" void __InitializeFileTable();

extern char **environ;

extern "C" void __mlibc_entry(int (*main_fn)(int argc, char *argv[], char *env[]), uintptr_t *entry_stack) {
	__dlapi_enter(entry_stack);
	__InitializeFileTable();
	auto result = main_fn(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ);
	exit(result);
}

#endif
