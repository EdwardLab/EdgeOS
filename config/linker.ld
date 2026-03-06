OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x08000000;
    _kernel_start = .;

    .multiboot2 : ALIGN(8)
    {
        KEEP(*(.multiboot2))
    }

    .bootstrap.text : ALIGN(4096)
    {
        *(.bootstrap.text)
    }

    .text : ALIGN(4096)
    {
        *(.text*)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata*)
    }

    .data : ALIGN(4096)
    {
        *(.data*)
    }

    .bss : ALIGN(4096)
    {
        *(COMMON)
        *(.bss*)
    }

    .bootstrap.bss (NOLOAD) : ALIGN(4096)
    {
        *(.bootstrap.bss)
    }

    _kernel_end = .;
}
