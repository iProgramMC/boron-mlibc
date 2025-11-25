#include "mlibc-asm/helpers.h"

PROC_START(_boron_start)
.cfi_undefined rip
	call relocateSelf

	call __mlibc_boron_calculateStackSize	
	sub  %rax, %rsp
	
	mov  %rsp, %rdi
	call __mlibc_boron_prepareStack

	mov  %rsp, %rdi
	call interpreterMain

	jmp *%rax
PROC_END(_boron_start)

GNU_STACK_NOTE()

