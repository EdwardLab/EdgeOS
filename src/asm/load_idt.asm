section .text
bits 64
global load_idt

load_idt:
    lidt [rdi]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
