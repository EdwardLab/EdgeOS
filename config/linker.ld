ENTRY(_start)

SECTIONS
{
    __kernel_section_start = .;
    .text 0x0100000 : {
        __kernel_text_section_start = .;
        *(.multiboot)
        *(.text)
        . = ALIGN(4096);
        __kernel_text_section_end = .;
    }

    .data : {
        __kernel_data_section_start = .;
        *(.data)
        . = ALIGN(4096);
        __kernel_data_section_end = .;
    }

    .rodata : {
        __kernel_rodata_section_start = .;
        *(.rodata)
        __kernel_rodata_section_end = .;
    }

    .bss : {
        __kernel_bss_section_start = .;
        *(.bss)
        . = ALIGN(4096);
        __kernel_bss_section_end = .;
    }

    .page_tables 0x1000 : ALIGN(4096) {
        . = ALIGN(4096);
        __kernel_page_tables_start = .;
        . += 4096 * 2;           /* 4 KB page directory + 4 KB first page table */
        . = ALIGN(4096);
        __kernel_page_tables_end = .;
    }

    end = .; _end = .; __end = .;
    __kernel_section_end = .;
}
