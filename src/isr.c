#include "isr.h"
#include "idt.h"
#include "8259_pic.h"
#include "console.h"
#include "sys/process.h"
#include "sys/scheduler.h"
#include <stdint.h>

#define MAX_HANDLERS_PER_VECTOR 8

static ISR g_interrupt_handlers[NO_INTERRUPT_HANDLERS][MAX_HANDLERS_PER_VECTOR];
static uint8_t g_interrupt_handler_count[NO_INTERRUPT_HANDLERS];

#define PTE_PRESENT 0x001ULL
#define PTE_WRITE   0x002ULL
#define PTE_USER    0x004ULL
#define PTE_PS      0x080ULL
#define USER_MIN_ADDR 0x0000000000001000ULL
#define USER_MAX_ADDR 0x0000000040000000ULL

char *exception_messages[32] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint", "Overflow",
    "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available (No Math Coprocessor)",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection", "Page Fault", "Unknown Interrupt (intel reserved)",
    "x87 FPU Floating-Point Error (Math Fault)", "Alignment Check", "Machine Check",
    "SIMD Floating-Point Exception", "Virtualization Exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved"
};

void isr_register_interrupt_handler(int num, ISR handler) {
    if (num < 0 || num >= NO_INTERRUPT_HANDLERS || !handler) return;
    for (uint8_t i = 0; i < g_interrupt_handler_count[num]; ++i) {
        if (g_interrupt_handlers[num][i] == handler) return;
    }
    if (g_interrupt_handler_count[num] >= MAX_HANDLERS_PER_VECTOR) return;
    g_interrupt_handlers[num][g_interrupt_handler_count[num]++] = handler;
}

int isr_interrupt_has_handler(int num) {
    if (num < 0 || num >= NO_INTERRUPT_HANDLERS) return 0;
    return g_interrupt_handler_count[num] > 0 ? 1 : 0;
}

void isr_end_interrupt(int num) {
    pic8259_eoi(num);
}

void isr_irq_handler(REGISTERS *reg) {
    if (reg->int_no < NO_INTERRUPT_HANDLERS) {
        for (uint8_t i = 0; i < g_interrupt_handler_count[reg->int_no]; ++i) {
            ISR handler = g_interrupt_handlers[reg->int_no][i];
            if (handler) handler(reg);
        }
    }
    pic8259_eoi((int)reg->int_no);
}

static void print_registers(REGISTERS *reg) {
    printf("REGISTERS:\n");
    printf("err_code=%x\n", (uint32)reg->err_code);
    printf("rax=0x%x rbx=0x%x rcx=0x%x rdx=0x%x\n", (uint32)reg->rax, (uint32)reg->rbx, (uint32)reg->rcx, (uint32)reg->rdx);
    printf("rdi=0x%x rsi=0x%x rbp=0x%x rsp=0x%x\n", (uint32)reg->rdi, (uint32)reg->rsi, (uint32)reg->rbp, (uint32)reg->rsp);
    printf("rip=0x%x cs=0x%x ss=0x%x rflags=0x%x\n", (uint32)reg->rip, (uint32)reg->cs, (uint32)reg->ss, (uint32)reg->rflags);
}

static void print_task_context_brief(void) {
    task_t *t = process_current_task();
    if (!t) return;
    printf("TASK: pid=%d ppid=%d name=%s pgid=%d sid=%d ctty=%d ctty_id=%d\n",
           t->pid, t->ppid, t->name, t->pgid, t->sid, t->ctty_kind, t->ctty_id);
}

static int user_span_ok(uint64_t addr, uint64_t len) {
    if (addr < USER_MIN_ADDR) return 0;
    if (addr >= USER_MAX_ADDR) return 0;
    if (len > USER_MAX_ADDR) return 0;
    if (addr + len < addr) return 0;
    if (addr + len > USER_MAX_ADDR) return 0;
    return 1;
}

static void dump_user_bytes(uint64_t addr, int count, const char *tag) {
    if (!user_span_ok(addr, (uint64_t)count)) return;
    printf("%s: va=0x%x bytes=", tag, (uint32)addr);
    for (int i = 0; i < count; ++i) {
        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(addr + (uint64_t)i);
        printf("%x", (uint32)(*p));
        if (i + 1 < count) printf(" ");
    }
    printf("\n");
}

static void dump_user_qwords(uint64_t addr, int count, const char *tag) {
    if (!user_span_ok(addr, (uint64_t)count * 8ULL)) return;
    printf("%s: va=0x%x", tag, (uint32)addr);
    for (int i = 0; i < count; ++i) {
        const volatile uint64_t *p = (const volatile uint64_t *)(uintptr_t)(addr + (uint64_t)i * 8ULL);
        printf(" q%d=0x%x", i, (uint32)(*p));
    }
    printf("\n");
}

static uint64 read_cr2(void) {
    uint64 v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}

static uint64 read_cr3(void) {
    uint64 v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void dump_pte_for_va(uint64 va) {
    uint64 cr3 = read_cr3() & ~0xFFFULL;
    uint64 *pml4 = (uint64 *)(uintptr_t)cr3;
    uint64 pml4e = pml4[(va >> 39) & 0x1FF];
    if (!(pml4e & PTE_PRESENT)) {
        printf("PT: va=0x%x pml4e=not-present\n", (uint32)va);
        return;
    }

    uint64 *pdpt = (uint64 *)(uintptr_t)(pml4e & ~0xFFFULL);
    uint64 pdpte = pdpt[(va >> 30) & 0x1FF];
    if (!(pdpte & PTE_PRESENT)) {
        printf("PT: va=0x%x pdpte=not-present pml4e_flags=0x%x\n", (uint32)va, (uint32)(pml4e & 0xFFFULL));
        return;
    }
    if (pdpte & PTE_PS) {
        printf("PT: va=0x%x 1G flags=0x%x P=%d U=%d W=%d\n",
               (uint32)va, (uint32)(pdpte & 0xFFFULL),
               (int)((pdpte & PTE_PRESENT) != 0), (int)((pdpte & PTE_USER) != 0), (int)((pdpte & PTE_WRITE) != 0));
        return;
    }

    uint64 *pd = (uint64 *)(uintptr_t)(pdpte & ~0xFFFULL);
    uint64 pde = pd[(va >> 21) & 0x1FF];
    if (!(pde & PTE_PRESENT)) {
        printf("PT: va=0x%x pde=not-present pdpte_flags=0x%x\n", (uint32)va, (uint32)(pdpte & 0xFFFULL));
        return;
    }
    if (pde & PTE_PS) {
        printf("PT: va=0x%x 2M flags=0x%x P=%d U=%d W=%d\n",
               (uint32)va, (uint32)(pde & 0xFFFULL),
               (int)((pde & PTE_PRESENT) != 0), (int)((pde & PTE_USER) != 0), (int)((pde & PTE_WRITE) != 0));
        return;
    }

    uint64 *pt = (uint64 *)(uintptr_t)(pde & ~0xFFFULL);
    uint64 pte = pt[(va >> 12) & 0x1FF];
    if (!(pte & PTE_PRESENT)) {
        printf("PT: va=0x%x pte=not-present pde_flags=0x%x\n", (uint32)va, (uint32)(pde & 0xFFFULL));
        return;
    }

    printf("PT: va=0x%x 4K flags=0x%x P=%d U=%d W=%d\n",
           (uint32)va, (uint32)(pte & 0xFFFULL),
           (int)((pte & PTE_PRESENT) != 0), (int)((pte & PTE_USER) != 0), (int)((pte & PTE_WRITE) != 0));
}

void isr_exception_handler(REGISTERS *reg) {
    if (reg->int_no == 6) {
        if ((reg->cs & 0x3) == 0x3) {
            const uint8_t *pc = (const uint8_t *)(uintptr_t)reg->rip;
            /* x86 CET IBT ENDBR instructions are safe no-ops on CPUs without CET.
             * Some user binaries are built with -fcf-protection and would otherwise
             * fault with #UD on entry to many functions. */
            if (pc && pc[0] == 0xF3 && pc[1] == 0x0F && pc[2] == 0x1E &&
                (pc[3] == 0xFA || pc[3] == 0xFB)) {
                reg->rip += 4;
                return;
            }
            if (pc && pc[0] == 0x0F && pc[1] == 0x05) {
                ISR syscall_h = g_interrupt_handler_count[128] ? g_interrupt_handlers[128][0] : 0;
                reg->rcx = reg->rip + 2;
                reg->r11 = reg->rflags;
                reg->rip += 2;
                if (syscall_h) {
                    syscall_h(reg);
                    return;
                }
            }
        }
    }

    if (reg->int_no == 14) {
        uint64 cr2 = read_cr2();
        uint64 ec = reg->err_code;
        uint64 p = ec & 1;
        uint64 wr = (ec >> 1) & 1;
        uint64 us = (ec >> 2) & 1;

        printf("PAGE FAULT: cr2=0x%x err=0x%x P=%d W/R=%d U/S=%d\n",
               (uint32)cr2, (uint32)ec, (int)p, (int)wr, (int)us);
        dump_pte_for_va(cr2);
        if (reg->rip) dump_pte_for_va(reg->rip);
        print_registers(reg);

        if ((reg->cs & 0x3) == 0x3 || us) {
            printf("[process] killing pid=%d due to user page fault\n", process_getpid());
            scheduler_kill_current_and_yield(-14);
            return;
        }
    }

    if (reg->int_no == 13) {
        printf("GPF: rip=0x%x cs=0x%x err=0x%x\n", (uint32)reg->rip, (uint32)reg->cs, (uint32)reg->err_code);
        dump_pte_for_va(reg->rip);
        if ((reg->cs & 0x3) == 0x3) {
            print_task_context_brief();
            if (reg->rip >= USER_MIN_ADDR + 8 && reg->rip + 16 < USER_MAX_ADDR) {
                dump_user_bytes(reg->rip - 8, 24, "CODE");
            } else {
                dump_user_bytes(reg->rip, 16, "CODE");
            }
            dump_user_qwords(reg->rsp, 6, "STACK");
            print_registers(reg);
            printf("[process] killing pid=%d due to user GPF\n", process_getpid());
            scheduler_kill_current_and_yield(-13);
            return;
        }
    }

    if (reg->int_no < 32) {
        if ((reg->cs & 0x3) == 0x3) {
            printf("EXCEPTION: %s\n", exception_messages[reg->int_no]);
            if (reg->int_no == 6) {
                print_task_context_brief();
                if (reg->rip >= USER_MIN_ADDR + 8 && reg->rip + 16 < USER_MAX_ADDR) {
                    dump_user_bytes(reg->rip - 8, 24, "UD-CODE");
                } else {
                    dump_user_bytes(reg->rip, 16, "UD-CODE");
                }
            }
            print_registers(reg);
            printf("[process] killing pid=%d due to user exception %d\n", process_getpid(), (int)reg->int_no);
            scheduler_kill_current_and_yield(-(int)reg->int_no);
            return;
        }
        printf("EXCEPTION: %s\n", exception_messages[reg->int_no]);
        print_registers(reg);
        for (;;) ;
    }
    if (reg->int_no < NO_INTERRUPT_HANDLERS) {
        for (uint8_t i = 0; i < g_interrupt_handler_count[reg->int_no]; ++i) {
            ISR handler = g_interrupt_handlers[reg->int_no][i];
            if (handler) handler(reg);
        }
    }
}
