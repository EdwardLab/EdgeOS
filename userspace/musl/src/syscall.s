.text
.global __edge_syscall1
.global __edge_syscall3

__edge_syscall1:
    mov %rdi, %rax
    mov %rsi, %rdi
    int $0x80
    ret

__edge_syscall3:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    int $0x80
    ret
