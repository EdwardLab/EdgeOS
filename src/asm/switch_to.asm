[BITS 64]

section .text
global switch_to
global fork_ret
global ret_from_fork
extern isr_return_from_frame

switch_to:
    mov [rdi + 0x00], r15
    mov [rdi + 0x08], r14
    mov [rdi + 0x10], r13
    mov [rdi + 0x18], r12
    mov [rdi + 0x20], rbx
    mov [rdi + 0x28], rbp

    pop rax
    mov [rdi + 0x30], rax
    mov [rdi + 0x38], rsp

    mov rsp, [rsi + 0x38]
    mov rax, [rsi + 0x30]
    push rax

    mov r15, [rsi + 0x00]
    mov r14, [rsi + 0x08]
    mov r13, [rsi + 0x10]
    mov r12, [rsi + 0x18]
    mov rbx, [rsi + 0x20]
    mov rbp, [rsi + 0x28]

    ret

global fork_ret
fork_ret:
    xor rax, rax
    ret

; r12 = pointer to edge_trap_frame_t
; Return to user mode through normal iretq path with copied syscall frame.
ret_from_fork:
    mov rsp, r12
    jmp isr_return_from_frame
section .note.GNU-stack noalloc noexec nowrite progbits
