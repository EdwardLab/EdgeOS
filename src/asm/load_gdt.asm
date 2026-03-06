section .text
bits 64

global load_gdt
load_gdt:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    push qword 0x08
    lea rax, [rel gdt_flush_ret]
    push rax
    retfq

gdt_flush_ret:
    ret

global load_tss
load_tss:
    mov ax, di
    ltr ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
