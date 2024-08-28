section .multiboot
    align 4
    dd 0x1BADB002               ; magic number
    dd 0x03                     ; flags
    dd -(0x1BADB002 + 0x03)     ; checksum

    dd 0                        ; Header address field (ignored by GRUB)
    dd 0                        ; Load address field (ignored by GRUB)
    dd 0                        ; Load end address field (ignored by GRUB)
    dd 0                        ; BSS end address field (ignored by GRUB)
    dd 0                        ; Entry point field (ignored by GRUB)
    
    dd 1                        ; Mode type (0 for text mode, 1 for graphics mode)
    dd 1024                     ; Width
    dd 768                      ; Height
    dd 32                       ; Bits per pixel


section .text
    global _start

_start:
    cli                  
    mov esp, stack_top        

    ; Load GDT (Global Descriptor Table)
    lgdt [gdt_descriptor]

    ; Enable protected mode
    mov eax, cr0
    or eax, 0x1                 ; Enable protected mode by setting PE bit
    mov cr0, eax

    ; Perform a far jump to flush the pipeline and switch to the code segment in protected mode
    jmp CODE_SEG:.protected_mode_entry

.protected_mode_entry:

    ; Set up segment registers
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up paging
    call setup_paging

    ; Enable paging
    mov eax, cr0
    or eax, 0x80000000          ; Set the paging bit (PG) in CR0
    mov cr0, eax

    ; Reload segment registers after enabling paging
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Call the kernel main function
    extern kmain
    call kmain    

halt:
    hlt
    jmp halt

setup_paging:
    ; Clear the page directory
    xor eax, eax
    mov edi, 0x1000             ; Page directory address (aligned to 4KB)
    mov ecx, 1024
    rep stosd                   ; Zero out the page directory

    ; Set up the first page table, mapping physical addresses 0x00000000 - 0x003FFFFF (4MB)
    mov eax, 0x00000003         ; Page table entry, maps physical address 0x00000000 (P=1, RW=1, US=0)
    mov edi, 0x2000             ; Page table address (aligned to 4KB)
    mov ecx, 1024               ; Map 4MB of memory (1024 entries, each 4KB)
setup_page_table:
    stosd                       ; Write the page table entry
    add eax, 0x1000             ; Move to the next 4KB page
    loop setup_page_table

    ; Point the first entry of the page directory to the first page table (maps 0x00000000 - 0x003FFFFF)
    mov eax, 0x00002003         ; Page directory entry, points to the page table at 0x2000 (P=1, RW=1, US=0)
    mov [0x1000], eax

    ; Load the page directory address into CR3
    mov eax, 0x1000
    mov cr3, eax

    ret

setup_kernel_page_table:
    stosd                       ; Write the page table entry
    add eax, 0x1000             ; Move to the next 4KB page
    loop setup_kernel_page_table

    ; Point the first entry of the page directory to the first page table (maps 0x00000000)
    mov eax, 0x00000003         ; Page directory entry, points to the page table at 0x2000 (P=1, RW=1, US=0)
    mov [0x1000], eax

    ; Point the second entry of the page directory to the second page table (maps 0x00100000)
    mov eax, 0x00003003         ; Page directory entry, points to the page table at 0x3000 (P=1, RW=1, US=0)
    mov [0x1004], eax

    ; Load the page directory address into CR3
    mov eax, 0x1000
    mov cr3, eax

    ret


section .bss
    align 16
stack_bottom: resb 8192 
stack_top:

section .data
align 8
GDT:
    dq 0x0000000000000000       ; Null descriptor
    dq 0x00cf9a000000ffff       ; Code segment descriptor
    dq 0x00cf92000000ffff       ; Data segment descriptor

gdt_descriptor:
    dw gdt_descriptor_end - GDT - 1 ; GDT limit (size - 1)
    dd GDT                        ; GDT base address
gdt_descriptor_end:

CODE_SEG equ 0x08
DATA_SEG equ 0x10
