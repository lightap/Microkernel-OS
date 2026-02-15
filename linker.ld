/* linker.ld - Linker script for the microkernel */
/* Kernel is loaded at 1 MB (standard for Multiboot) */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    /* Multiboot header MUST be within first 8KB of the file */
    .multiboot BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
    }

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /* Kernel end marker */
    _kernel_end = .;
}
