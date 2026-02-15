; crt0.asm — User-space C runtime entry point
; Each server binary starts here. Calls main(), then SYS_EXIT.

section .note.GNU-stack noalloc noexec nowrite progbits

section .text
global _start
extern main

_start:
    ; Stack is already set up by the kernel (user stack from ELF loader)
    call main

    ; If main() ever returns, exit cleanly
    mov eax, 6          ; SYS_EXIT
    mov ebx, 0          ; exit code 0
    int 0x80
    jmp $               ; Should never reach here
