/* user.ld */
ENTRY(_start)
SECTIONS
{
    . = 0xC0000000; /* Must match USER_BASE */
    .text   : { *(.text) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data) }
    .bss    : { *(.bss) *(COMMON) }
}