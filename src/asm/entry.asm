section .multiboot2
align 8
header_start:
    dd 0xE85250D6
    dd 0
    dd header_end - header_start
    dd -(0xE85250D6 + 0 + (header_end - header_start))

    align 8
    dw 5
    dw 1
    dd 20
    dd 0
    dd 0
    dd 0

    align 8
    dw 0
    dw 0
    dd 8
align 8
header_end:


%define CR4_PAE            (1 << 5)
%define CR4_OSFXSR         (1 << 9)
%define CR4_OSXMMEXCPT     (1 << 10)
%define CR0_PG             (1 << 31)
%define CR0_MP             (1 << 1)
%define CR0_EM             (1 << 2)
%define CR0_TS             (1 << 3)
%define CR0_NE             (1 << 5)
%define IA32_EFER_MSR      0xC0000080
%define IA32_EFER_LME      (1 << 8)
%define PAGE_PRESENT_WRITE 0x003
%define PAGE_PS            0x080


section .bootstrap.text
bits 32
global _start
global pml4_table
global pdpt_table
global pd_table0
global pd_table1
global pd_table2
global pd_table3
extern kmain

_start:
    cli

    mov [mb_magic], eax
    mov [mb_info], ebx

    mov esp, bootstrap_stack_top

    lgdt [gdt64_ptr]

    ; ---------------------------
    ; Zero page tables
    ; ---------------------------
    mov edi, pml4_table
    mov ecx, 4096/4
    xor eax, eax
    rep stosd

    mov edi, pdpt_table
    mov ecx, 4096/4
    xor eax, eax
    rep stosd

    mov edi, pd_table0
    mov ecx, (4096*4)/4
    xor eax, eax
    rep stosd

    ; ---------------------------
    ; PML4[0] -> PDPT
    ; ---------------------------
    mov eax, pdpt_table
    or eax, PAGE_PRESENT_WRITE
    mov [pml4_table], eax
    mov dword [pml4_table+4], 0

    ; ---------------------------
    ; PDPT[0..3] -> PD0..PD3
    ; ---------------------------
    mov eax, pd_table0
    or eax, PAGE_PRESENT_WRITE
    mov [pdpt_table + 0], eax
    mov dword [pdpt_table + 4], 0

    mov eax, pd_table1
    or eax, PAGE_PRESENT_WRITE
    mov [pdpt_table + 8], eax
    mov dword [pdpt_table + 12], 0

    mov eax, pd_table2
    or eax, PAGE_PRESENT_WRITE
    mov [pdpt_table + 16], eax
    mov dword [pdpt_table + 20], 0

    mov eax, pd_table3
    or eax, PAGE_PRESENT_WRITE
    mov [pdpt_table + 24], eax
    mov dword [pdpt_table + 28], 0

    ; ---------------------------
    ; Map first 4GiB (2MiB pages)
    ; ---------------------------

    xor ecx, ecx
.map_pd0:
    mov eax, ecx
    shl eax, 21
    or eax, PAGE_PRESENT_WRITE | PAGE_PS
    mov [pd_table0 + ecx*8], eax
    mov dword [pd_table0 + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .map_pd0

    xor ecx, ecx
.map_pd1:
    mov eax, ecx
    shl eax, 21
    add eax, 0x40000000
    or eax, PAGE_PRESENT_WRITE | PAGE_PS
    mov [pd_table1 + ecx*8], eax
    mov dword [pd_table1 + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .map_pd1

    xor ecx, ecx
.map_pd2:
    mov eax, ecx
    shl eax, 21
    add eax, 0x80000000
    or eax, PAGE_PRESENT_WRITE | PAGE_PS
    mov [pd_table2 + ecx*8], eax
    mov dword [pd_table2 + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .map_pd2

    xor ecx, ecx
.map_pd3:
    mov eax, ecx
    shl eax, 21
    add eax, 0xC0000000
    or eax, PAGE_PRESENT_WRITE | PAGE_PS
    mov [pd_table3 + ecx*8], eax
    mov dword [pd_table3 + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .map_pd3

    ; ---------------------------
    ; Enable paging
    ; ---------------------------
    mov eax, pml4_table
    mov cr3, eax

    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    mov ecx, IA32_EFER_MSR
    rdmsr
    or eax, IA32_EFER_LME
    wrmsr

    mov eax, cr0
    or eax, CR0_PG
    mov cr0, eax

    jmp 0x08:long_mode_entry


section .text
bits 64
long_mode_entry:

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Enable x87/SSE for kernel and user space before entering C code.
    mov rax, cr0
    and rax, ~(CR0_EM | CR0_TS)
    or  rax, (CR0_MP | CR0_NE)
    mov cr0, rax

    mov rax, cr4
    or  rax, (CR4_OSFXSR | CR4_OSXMMEXCPT)
    mov cr4, rax

    fninit
    ldmxcsr [rel mxcsr_default]

    mov rsp, long_mode_stack_top
    and rsp, -16

    mov edi, dword [mb_magic]
    mov esi, dword [mb_info]

    call kmain

.hang:
    hlt
    jmp .hang


section .rodata
align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

align 16
mxcsr_default:
    dd 0x00001F80


section .bootstrap.bss
align 16
mb_magic: resd 1
mb_info:  resd 1

align 4096
pml4_table: resb 4096
align 4096
pdpt_table: resb 4096
align 4096
pd_table0:  resb 4096
align 4096
pd_table1:  resb 4096
align 4096
pd_table2:  resb 4096
align 4096
pd_table3:  resb 4096

align 16
bootstrap_stack: resb 4096
bootstrap_stack_top:

align 16
long_mode_stack: resb 16384
long_mode_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
