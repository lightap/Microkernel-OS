; boot.asm - Multiboot entry point for the microkernel
; Updated for ring 3 support with TSS

section .note.GNU-stack noalloc noexec nowrite progbits

section .multiboot
align 4
    MAGIC       equ 0x1BADB002
    ALIGN_FLAG  equ 1 << 0
    MEMINFO     equ 1 << 1
    FLAGS       equ ALIGN_FLAG | MEMINFO
    CHECKSUM    equ -(MAGIC + FLAGS)

    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 16
stack_bottom:
    resb 262144         ; 256 KiB kernel boot stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

; --- GDT flush ---
global gdt_flush
gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]
    mov ax, 0x10        ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush     ; far jump to load kernel code segment
.flush:
    ret

; --- TSS flush --- load the TSS selector into the task register
global tss_flush
tss_flush:
    mov ax, 0x28        ; TSS is GDT entry 5: 5 * 8 = 0x28
    ltr ax
    ret

; --- IDT load ---
global idt_load
idt_load:
    mov eax, [esp+4]
    lidt [eax]
    ret

; --- ISR stubs (CPU exceptions + syscall) ---
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    cli
    push dword 0
    push dword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    cli
    push dword %1
    jmp isr_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; Syscall interrupt (INT 0x80 = ISR 128)
ISR_NOERRCODE 128

; --- IRQ stubs ---
%macro IRQ 2
global irq%1
irq%1:
    cli
    push dword 0
    push dword %2
    jmp irq_common_stub
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; --- Common ISR stub ---
; Handles CPU exceptions and INT 0x80 (syscall).
; Works for both ring 0 and ring 3 callers — the CPU automatically
; pushes/pops SS:ESP for cross-ring transitions via iret.
extern isr_handler
isr_common_stub:
    pusha                   ; push EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    mov ax, ds
    push eax                ; save caller's DS
    mov ax, 0x10            ; load kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp                ; arg: pointer to registers_t
    call isr_handler
    add esp, 4              ; pop arg
    pop eax                 ; restore caller's DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8              ; skip int_no, err_code
    iret                    ; returns to ring 0 or ring 3 depending on CS on stack

; --- Common IRQ stub (supports preemptive task switching) ---
; When the CPU takes an IRQ from ring 3, it automatically:
;   1. Loads SS:ESP from TSS (switches to kernel stack)
;   2. Pushes user SS, user ESP, EFLAGS, user CS, user EIP
; On iret back to ring 3, it pops all 5 values.
; On iret back to ring 0, it only pops EIP, CS, EFLAGS.
; This stub handles both cases transparently.
extern irq_handler
irq_common_stub:
    pusha
    mov ax, ds
    push eax                ; save caller's data segment
    mov ax, 0x10            ; load kernel data segment for handler
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp                ; arg: pointer to registers_t on stack
    call irq_handler        ; returns new_esp in eax (0 = no switch)
    test eax, eax
    jz .irq_no_switch
    mov esp, eax            ; switch to new task's saved context
    jmp .irq_restore
.irq_no_switch:
    add esp, 4              ; pop the pushed esp argument
.irq_restore:
    pop ebx                 ; restore task's data segment
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    popa
    add esp, 8              ; skip int_no, err_code
    iret                    ; ring 0 or ring 3 return (CPU checks CS on stack)
