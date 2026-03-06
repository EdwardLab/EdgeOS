#include "sys/user_exec.h"

#include "string.h"
#include "sys/process.h"

extern void gdt_set_tss_rsp0(uint64_t rsp0);

__attribute__((noreturn)) static void user_iretq_enter(uint64_t rip, uint64_t rsp, uint64_t argc, uint64_t argv) {
    __asm__ __volatile__(
        "xor %%rcx, %%rcx\n"
        "xor %%rdx, %%rdx\n"
        "xor %%r8, %%r8\n"
        "xor %%r9, %%r9\n"
        "xor %%r10, %%r10\n"
        "xor %%r11, %%r11\n"
        "mov %[argc], %%rdi\n"
        "mov %[argv], %%rsi\n"
        "mov $0x202, %%rax\n"
        "pushq %[uds]\n"
        "pushq %[ursp]\n"
        "pushq %%rax\n"
        "pushq %[ucs]\n"
        "pushq %[urip]\n"
        "iretq\n"
        :
        : [urip]"r"(rip), [ursp]"r"(rsp), [ucs]"i"(USER_CS), [uds]"i"(USER_DS),
          [argc]"r"(argc), [argv]"r"(argv)
        : "rax", "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10", "r11", "memory");
    for (;;) __asm__ __volatile__("hlt");
}

void user_exec_set_kernel_rsp0(uint64_t rsp0) {
    gdt_set_tss_rsp0(rsp0);
}

static int user_push_u64(uintptr_t *sp, uint64_t v) {
    if (!sp || *sp < 8) return -1;
    *sp -= sizeof(uint64_t);
    *(uint64_t *)(*sp) = v;
    return 0;
}

int user_exec_run(const user_exec_image_t *img, int argc, char **argv, int envc, char **envp) {
    if (!img) return -1;

    task_t *cur = process_current_task();
    if (!cur) return -1;

    if (argc < 0) argc = 0;
    if (argc > 32) argc = 32;

    uintptr_t sp = (uintptr_t)img->user_stack_top;
    uint64_t user_argv_ptrs[33];
    uint64_t user_envp_ptrs[33];
    int real_argc = 0;
    int real_envc = 0;

    for (int i = 0; i < argc; ++i) {
        if (!argv || !argv[i]) break;
        real_argc++;
    }
    if (envc < 0) envc = 0;
    if (envc > 32) envc = 32;
    for (int i = 0; i < envc; ++i) {
        if (!envp || !envp[i]) break;
        real_envc++;
    }

    for (int i = real_argc - 1; i >= 0; --i) {
        int n = strlen(argv[i]) + 1;
        sp -= (uintptr_t)n;
        memcpy((void *)sp, argv[i], (uint32_t)n);
        user_argv_ptrs[i] = (uint64_t)sp;
    }
    user_argv_ptrs[real_argc] = 0;
    for (int i = real_envc - 1; i >= 0; --i) {
        int n = strlen(envp[i]) + 1;
        sp -= (uintptr_t)n;
        memcpy((void *)sp, envp[i], (uint32_t)n);
        user_envp_ptrs[i] = (uint64_t)sp;
    }
    user_envp_ptrs[real_envc] = 0;

    {
        /* x86_64 SysV process entry: keep %rsp 16-byte aligned at _start.
         * Stack push count below is (25 + argc + envc) qwords. Choose the
         * pre-push alignment so final %rsp stays 16-byte aligned. */
        int qwords = 25 + real_argc + real_envc;
        sp &= ~(uintptr_t)0xFULL;
        if ((qwords & 1) != 0) {
            if (sp < sizeof(uint64_t)) return -1;
            sp -= sizeof(uint64_t);
        }
    }
    /* Build Linux-style initial stack:
     * [argc][argv...][NULL][envp...][NULL][auxv...][AT_NULL]
     */
    if (user_push_u64(&sp, 0) < 0) return -1; /* AT_NULL a_val */
    if (user_push_u64(&sp, 0) < 0) return -1; /* AT_NULL a_type */
    if (user_push_u64(&sp, cur->egid) < 0) return -1;
    if (user_push_u64(&sp, 14) < 0) return -1; /* AT_EGID */
    if (user_push_u64(&sp, cur->gid) < 0) return -1;
    if (user_push_u64(&sp, 13) < 0) return -1; /* AT_GID */
    if (user_push_u64(&sp, cur->euid) < 0) return -1;
    if (user_push_u64(&sp, 12) < 0) return -1; /* AT_EUID */
    if (user_push_u64(&sp, cur->uid) < 0) return -1;
    if (user_push_u64(&sp, 11) < 0) return -1; /* AT_UID */
    if (user_push_u64(&sp, 4096) < 0) return -1;
    if (user_push_u64(&sp, 6) < 0) return -1;  /* AT_PAGESZ */
    if (user_push_u64(&sp, img->at_base) < 0) return -1;
    if (user_push_u64(&sp, 7) < 0) return -1;  /* AT_BASE */
    if (user_push_u64(&sp, img->at_entry) < 0) return -1;
    if (user_push_u64(&sp, 9) < 0) return -1;  /* AT_ENTRY */
    if (user_push_u64(&sp, img->at_phnum) < 0) return -1;
    if (user_push_u64(&sp, 5) < 0) return -1;  /* AT_PHNUM */
    if (user_push_u64(&sp, 56) < 0) return -1;
    if (user_push_u64(&sp, 4) < 0) return -1;  /* AT_PHENT */
    if (user_push_u64(&sp, img->at_phdr) < 0) return -1;
    if (user_push_u64(&sp, 3) < 0) return -1;  /* AT_PHDR */
    if (user_push_u64(&sp, 0) < 0) return -1;  /* envp terminator */
    for (int i = real_envc - 1; i >= 0; --i) {
        if (user_push_u64(&sp, user_envp_ptrs[i]) < 0) return -1;
    }
    for (int i = real_argc; i >= 0; --i) {
        if (user_push_u64(&sp, user_argv_ptrs[i]) < 0) return -1;
    }
    if (user_push_u64(&sp, (uint64_t)real_argc) < 0) return -1;

    user_exec_set_kernel_rsp0(cur->kernel_stack_top);
    user_iretq_enter(img->entry, (uint64_t)sp, (uint64_t)real_argc, (uint64_t)(sp + sizeof(uint64_t)));
    return -1;
}
