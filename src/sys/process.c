#include "sys/process.h"
#include "sys/scheduler.h"
#include "sys/syscall.h"
#include "elf/elf_loader.h"
#include "fb.h"
#include "dev/fbdev.h"
#include "stdio.h"
#include "string.h"
#include "sys/user_exec.h"

#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITE   0x002ULL
#define PAGE_USER    0x004ULL
#define PAGE_PS      0x080ULL

#define USER_TEXT_BASE   0x0000000020000000ULL
#define USER_STACK_BASE  0x0000000020200000ULL
#define USER_HEAP_BASE   0x0000000020400000ULL
#define USER_HEAP_EXT_BASE (USER_HEAP_BASE + USER_HEAP_MAX_DELTA)
#define USER_HEAP_EXT_SIZE USER_HEAP_PY_EXTRA_DELTA
#define USER_MMAP_BASE   0x0000000030000000ULL
#define USER_BIGPIE_BASE  0x0000000030000000ULL
#define USER_BIGPIE_SIZE  (16ULL * 1024ULL * 1024ULL)
#define USER_FBDEV_BASE   EDGE_FBDEV_USER_BASE
#define USER_FBDEV_MAX_PAGES EDGE_FBDEV_USER_MAX_PAGES
#define USER_REGION_SIZE (2 * 1024 * 1024)
#define USER_HEAP_PDE_CNT (USER_HEAP_MAX_DELTA / USER_REGION_SIZE)
#define USER_HEAP_SIZE    (USER_HEAP_PDE_CNT * USER_REGION_SIZE)
#define USER_HEAP_EXT_PDE_CNT (USER_HEAP_EXT_SIZE / USER_REGION_SIZE)
#define USER_BIGPIE_PDE_CNT (USER_BIGPIE_SIZE / USER_REGION_SIZE)
#define USER_STACK_TOP   (USER_STACK_BASE + USER_REGION_SIZE)
#define USER_AS_MAX_TASKS 16
#define USER_LOW_BASE    0x0000000000400000ULL
#define USER_LOW_SIZE    (4 * 1024 * 1024)
#define USER_LOW_PDE_CNT (USER_LOW_SIZE / (2 * 1024 * 1024))

#define KSTACK_SIZE 16384
#define IA32_FS_BASE_MSR 0xC0000100u

#ifndef EDGE_SECURITY_DEBUG
#define EDGE_SECURITY_DEBUG 0
#endif

#ifndef EDGE_SPAWN_DEBUG
#define EDGE_SPAWN_DEBUG 0
#endif

#ifndef EDGE_SCHED_PROC_DEBUG
#define EDGE_SCHED_PROC_DEBUG 0
#endif

#define LINUX_SIGINT 2
#define LINUX_SIGCHLD 17
#define LINUX_SIGKILL 9
#define LINUX_SIGTERM 15
#define LINUX_SIG_DFL 0ULL
#define LINUX_SIG_IGN 1ULL

static task_t g_tasks[PROC_MAX_TASKS];
static int g_next_pid;

// Page tables
static uint64_t g_pml4[PROC_MAX_TASKS][512] __attribute__((aligned(4096)));
static uint64_t g_pdpt[PROC_MAX_TASKS][512] __attribute__((aligned(4096)));
static uint64_t g_pd[PROC_MAX_TASKS][4][512] __attribute__((aligned(4096)));
static uint8_t g_kstack[PROC_MAX_TASKS][16384] __attribute__((aligned(16)));
static uint8_t g_user_text[USER_AS_MAX_TASKS][USER_REGION_SIZE] __attribute__((aligned(USER_REGION_SIZE)));
static uint8_t g_user_stack[USER_AS_MAX_TASKS][USER_REGION_SIZE] __attribute__((aligned(USER_REGION_SIZE)));
static uint8_t g_user_heap[USER_AS_MAX_TASKS][USER_HEAP_SIZE] __attribute__((aligned(USER_REGION_SIZE)));
static uint8_t g_user_heap_ext[USER_AS_MAX_TASKS][USER_HEAP_EXT_SIZE] __attribute__((aligned(USER_REGION_SIZE)));
static uint8_t g_user_bigpie[USER_AS_MAX_TASKS][USER_BIGPIE_SIZE] __attribute__((aligned(USER_REGION_SIZE)));
static uint8_t g_user_low[USER_AS_MAX_TASKS][USER_LOW_SIZE] __attribute__((aligned(2 * 1024 * 1024)));
extern char _kernel_start;
extern char _kernel_end;

// --- Helper Functions ---

static inline void cr3_write(uint64_t v) { 
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory"); 
}

static inline uint64_t cr3_read(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_fs_base(uint64_t v) {
    uint32_t lo = (uint32_t)(v & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(v >> 32);
    __asm__ __volatile__(
        "wrmsr"
        :
        : "c"(IA32_FS_BASE_MSR), "a"(lo), "d"(hi)
        : "memory");
}

extern void ret_from_fork(void);
static void process_enter_user_current(void);

static void task_child_unlink(task_t *child) {
    task_t *parent;
    if (!child) return;
    parent = child->parent;
    if (child->sibling_prev) child->sibling_prev->sibling_next = child->sibling_next;
    else if (parent) parent->first_child = child->sibling_next;
    if (child->sibling_next) child->sibling_next->sibling_prev = child->sibling_prev;
    child->sibling_prev = 0;
    child->sibling_next = 0;
    child->parent = 0;
}

static void task_child_link(task_t *parent, task_t *child) {
    if (!child) return;
    task_child_unlink(child);
    child->parent = parent;
    if (parent) {
        child->sibling_next = parent->first_child;
        if (parent->first_child) parent->first_child->sibling_prev = child;
        parent->first_child = child;
        child->ppid = parent->pid;
    } else {
        child->ppid = 0;
    }
}

static void process_notify_parent_sigchld(task_t *parent) {
    if (!parent) return;
    if (parent->state == TASK_UNUSED || parent->state == TASK_ZOMBIE) return;
    if (parent->sigchld_handler != LINUX_SIG_IGN) {
        parent->sigchld_pending = 1;
    }
    if (parent->state == TASK_BLOCKED) {
        uint32_t wake_cpu = (parent->assigned_cpu >= 0) ? (uint32_t)parent->assigned_cpu : scheduler_cpu_id();
        scheduler_task_make_runnable(parent, wake_cpu);
    }
}

// --- Task Management ---

static task_t *task_find_by_pid(int pid) {
    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        if (g_tasks[i].state != TASK_UNUSED && g_tasks[i].pid == pid) return &g_tasks[i];
    }
    return 0;
}

static int task_index(task_t *t) {
    if (!t) return -1;
    return (int)(t - &g_tasks[0]);
}

static task_t *task_alloc(void) {
    for (int i = 0; i < USER_AS_MAX_TASKS; ++i) {
        if (g_tasks[i].state == TASK_UNUSED) return &g_tasks[i];
    }
    return 0;
}

static void task_clear_user_regions(task_t *t) {
    int idx = task_index(t);
    uint64_t heap_bytes = USER_HEAP_DEFAULT_DELTA;
    uint64_t heap_ext_bytes = 0;
    if (idx < 0 || idx >= USER_AS_MAX_TASKS) return;
    if (t && t->user_heap_limit > USER_HEAP_BASE) {
        heap_bytes = t->user_heap_limit - USER_HEAP_BASE;
        if (heap_bytes > USER_HEAP_SIZE) {
            heap_ext_bytes = heap_bytes - USER_HEAP_SIZE;
            heap_bytes = USER_HEAP_SIZE;
            if (heap_ext_bytes > USER_HEAP_EXT_SIZE) heap_ext_bytes = USER_HEAP_EXT_SIZE;
        }
    }
    memset(&g_user_low[idx][0], 0, USER_LOW_SIZE);
    memset(&g_user_text[idx][0], 0, USER_REGION_SIZE);
    memset(&g_user_stack[idx][0], 0, USER_REGION_SIZE);
    memset(&g_user_heap[idx][0], 0, (uint32_t)heap_bytes);
    if (heap_ext_bytes) memset(&g_user_heap_ext[idx][0], 0, (uint32_t)heap_ext_bytes);
}

static void task_clear_user_vmas(task_t *t) {
    if (!t) return;
    t->user_vma_count = 0;
    memset(t->user_vmas, 0, sizeof(t->user_vmas));
}

static int task_name_from_path(const char *path, char *out, int out_sz) {
    if (!path || !out || out_sz <= 0) return -1;
    const char *name = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') name = p + 1;
    }
    int i = 0;
    while (name[i] && i < out_sz - 1) {
        out[i] = name[i];
        ++i;
    }
    out[i] = '\0';
    return 0;
}

static void task_build_address_space(task_t *t, int user_mode) {
    int idx = task_index(t);
    memset(g_pml4[idx], 0, sizeof(g_pml4[idx]));
    memset(g_pdpt[idx], 0, sizeof(g_pdpt[idx]));
    memset(g_pd[idx], 0, sizeof(g_pd[idx]));

    uint64_t upper_flags = PAGE_PRESENT | PAGE_WRITE;
    if (user_mode) upper_flags |= PAGE_USER;

    g_pml4[idx][0] = ((uint64_t)&g_pdpt[idx][0]) | upper_flags;

    for (int pdi = 0; pdi < 4; ++pdi) {
        g_pdpt[idx][pdi] = ((uint64_t)&g_pd[idx][pdi][0]) | upper_flags;
        for (int i = 0; i < 512; ++i) {
            uint64_t base = ((uint64_t)pdi << 30) + ((uint64_t)i << 21);
            uint64_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_PS;
            if (user_mode) {
                uint64_t end = base + (2ULL * 1024ULL * 1024ULL);
                uint64_t ks = (uint64_t)(uintptr_t)&_kernel_start;
                uint64_t ke = (uint64_t)(uintptr_t)&_kernel_end;
                if (end <= ks || base >= ke) flags |= PAGE_USER;
            }
            g_pd[idx][pdi][i] = base | flags;
        }
    }

    if (user_mode && idx < USER_AS_MAX_TASKS) {
        uint64_t uflags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_PS;
        for (uint32_t p = 0; p < USER_LOW_PDE_CNT; ++p) {
            uint64_t va = USER_LOW_BASE + ((uint64_t)p << 21);
            uint32_t pde = (uint32_t)((va >> 21) & 0x1FF);
            g_pd[idx][0][pde] = ((uint64_t)(uintptr_t)&g_user_low[idx][p << 21]) | uflags;
        }
        g_pd[idx][0][(USER_TEXT_BASE >> 21) & 0x1FF] = ((uint64_t)(uintptr_t)&g_user_text[idx][0]) | uflags;
        g_pd[idx][0][(USER_STACK_BASE >> 21) & 0x1FF] = ((uint64_t)(uintptr_t)&g_user_stack[idx][0]) | uflags;
        for (uint32_t p = 0; p < USER_HEAP_PDE_CNT; ++p) {
            uint32_t pde = (uint32_t)(((USER_HEAP_BASE >> 21) & 0x1FF) + p);
            if (pde >= 512) break;
            g_pd[idx][0][pde] = ((uint64_t)(uintptr_t)&g_user_heap[idx][p << 21]) | uflags;
        }
        for (uint32_t p = 0; p < USER_HEAP_EXT_PDE_CNT; ++p) {
            uint32_t pde = (uint32_t)(((USER_HEAP_EXT_BASE >> 21) & 0x1FF) + p);
            if (pde >= 512) break;
            g_pd[idx][0][pde] = ((uint64_t)(uintptr_t)&g_user_heap_ext[idx][p << 21]) | uflags;
        }
        for (uint32_t p = 0; p < USER_BIGPIE_PDE_CNT; ++p) {
            uint32_t pde = (uint32_t)(((USER_BIGPIE_BASE >> 21) & 0x1FF) + p);
            if (pde >= 512) break;
            g_pd[idx][0][pde] = ((uint64_t)(uintptr_t)&g_user_bigpie[idx][p << 21]) | uflags;
        }
        {
            uint64_t fb_phys = 0, fb_off = 0;
            uint32_t fb_pages = 0;
            if (fb_get_2m_phys_window(&fb_phys, &fb_pages, &fb_off)) {
                (void)fb_off;
                if (fb_pages > USER_FBDEV_MAX_PAGES) fb_pages = USER_FBDEV_MAX_PAGES;
                for (uint32_t p = 0; p < fb_pages; ++p) {
                    uint32_t pde = (uint32_t)(((USER_FBDEV_BASE >> 21) & 0x1FF) + p);
                    if (pde >= 512) break;
                    g_pd[idx][0][pde] = (fb_phys + ((uint64_t)p << 21)) | uflags;
                }
            }
        }
    }

    {
        uint64_t fb_phys = 0, fb_virt = 0;
        uint32_t fb_pages = 0;
        if (fb_get_2m_remap(&fb_phys, &fb_pages, &fb_virt)) {
            uint64_t kflags = PAGE_PRESENT | PAGE_WRITE | PAGE_PS;
            uint32_t pde_start = (uint32_t)((fb_virt - 0xC0000000ULL) >> 21);
            for (uint32_t i = 0; i < fb_pages && (pde_start + i) < 512; ++i) {
                g_pd[idx][3][pde_start + i] = (fb_phys + ((uint64_t)i << 21)) | kflags;
            }
        }
    }

    t->cr3 = (uint64_t)&g_pml4[idx][0];
}

void process_init(void) {
    memset(g_tasks, 0, sizeof(g_tasks));
    g_next_pid = 1;
    scheduler_init();
    scheduler_set_cpu_id(0);

    task_t *init = task_alloc();
    if (!init) return;

    init->pid = g_next_pid++;
    init->ppid = 0;
    init->state = TASK_UNUSED;
    init->exit_code = 0;
    strcpy(init->name, "init");
    init->kernel_stack_top = (uint64_t)(uintptr_t)&g_kstack[0][KSTACK_SIZE - 16];
    init->user_stack_top = USER_STACK_TOP;
    init->user_heap_base = USER_HEAP_BASE;
    init->user_brk = USER_HEAP_BASE;
    init->user_heap_limit = USER_HEAP_BASE + USER_HEAP_DEFAULT_DELTA;
    init->user_mmap_next = USER_MMAP_BASE;
    task_clear_user_vmas(init);
    init->fs_base = 0;
    init->uid = 0;
    init->gid = 0;
    init->euid = 0;
    init->egid = 0;
    init->umask = 022;
    init->pgid = init->pid;
    init->sid = init->pid;
    init->ctty_kind = PROCESS_CTTY_CONSOLE;
    init->ctty_id = -1;
    init->sigaltstack_flags = 2; /* SS_DISABLE */
    strcpy(init->cwd, "/");
    init->assigned_cpu = -1;
    init->cr3 = cr3_read();
    scheduler_set_boot_current(init);
}

int process_fork(const edge_trap_frame_t *parent_tf) {
    task_t *parent = process_current_task();
    if (!parent || !parent_tf) return -1;

    task_t *child = task_alloc();
    if (!child) return -1;

    int child_idx = task_index(child);
    int parent_idx = task_index(parent);
    if (child_idx >= USER_AS_MAX_TASKS || parent_idx >= USER_AS_MAX_TASKS) return -1;

    // 1. Metadata
    child->pid = g_next_pid++;
    child->ppid = parent->pid;
    child->state = TASK_UNUSED;
    child->exit_code = 0;
    strcpy(child->name, parent->name);

    // 2. Address Space
    task_build_address_space(child, 1);

    /* Start from clean pages; fork then overlays parent image below. */
    task_clear_user_regions(child);

    // 3. Stack Setup
    child->kernel_stack_top = (uint64_t)(uintptr_t)&g_kstack[child_idx][KSTACK_SIZE - 16];
    child->user_stack_top = parent->user_stack_top;
    child->user_heap_base = parent->user_heap_base;
    child->user_brk = parent->user_brk;
    child->user_heap_limit = parent->user_heap_limit;
    child->user_mmap_next = parent->user_mmap_next;
    child->user_vma_count = parent->user_vma_count;
    memcpy(child->user_vmas, parent->user_vmas, sizeof(child->user_vmas));
    child->fs_base = parent->fs_base;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->umask = parent->umask;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->ctty_kind = parent->ctty_kind;
    child->ctty_id = parent->ctty_id;
    strcpy(child->cwd, parent->cwd[0] ? parent->cwd : "/");
    child->sigint_handler = parent->sigint_handler;
    child->sigalrm_handler = parent->sigalrm_handler;
    child->sigterm_handler = parent->sigterm_handler;
    child->sigchld_handler = parent->sigchld_handler;
    child->sigint_pending = 0;
    child->sigalrm_pending = 0;
    child->sigterm_pending = 0;
    child->sigaltstack_sp = parent->sigaltstack_sp;
    child->sigaltstack_size = parent->sigaltstack_size;
    child->sigaltstack_flags = parent->sigaltstack_flags;
    task_child_link(parent, child);

    // 4. Copy user memory
    memcpy(&g_user_low[child_idx][0], &g_user_low[parent_idx][0], USER_LOW_SIZE);
    memcpy(&g_user_text[child_idx][0], &g_user_text[parent_idx][0], USER_REGION_SIZE);
    memcpy(&g_user_stack[child_idx][0], &g_user_stack[parent_idx][0], USER_REGION_SIZE);
    {
        uint64_t heap_copy = USER_HEAP_DEFAULT_DELTA;
        uint64_t heap_ext_copy = 0;
        if (parent->user_heap_limit > USER_HEAP_BASE) {
            heap_copy = parent->user_heap_limit - USER_HEAP_BASE;
            if (heap_copy > USER_HEAP_SIZE) {
                heap_ext_copy = heap_copy - USER_HEAP_SIZE;
                heap_copy = USER_HEAP_SIZE;
                if (heap_ext_copy > USER_HEAP_EXT_SIZE) heap_ext_copy = USER_HEAP_EXT_SIZE;
            }
        }
        memcpy(&g_user_heap[child_idx][0], &g_user_heap[parent_idx][0], (uint32_t)heap_copy);
        if (heap_copy < USER_HEAP_SIZE) {
            memset(&g_user_heap[child_idx][(uint32_t)heap_copy], 0, (uint32_t)(USER_HEAP_SIZE - heap_copy));
        }
        if (heap_ext_copy) {
            memcpy(&g_user_heap_ext[child_idx][0], &g_user_heap_ext[parent_idx][0], (uint32_t)heap_ext_copy);
        }
        if (heap_ext_copy < USER_HEAP_EXT_SIZE) {
            memset(&g_user_heap_ext[child_idx][(uint32_t)heap_ext_copy], 0, (uint32_t)(USER_HEAP_EXT_SIZE - heap_ext_copy));
        }
    }
    /*
     * Trap-frame driven fork:
     * child resumes through normal syscall/interrupt return path, with
     * registers copied from parent except rax=0.
     */
    child->fork_tf = *parent_tf;
    child->fork_tf.rax = 0;
    /* Never propagate single-step into a fork child. */
    child->fork_tf.rflags &= ~(1ull << 8);

    memset(&child->context, 0, sizeof(child->context));
    child->context.r12 = (uint64_t)(uintptr_t)&child->fork_tf;
    child->context.rip = (uint64_t)ret_from_fork;
    child->context.rsp = child->kernel_stack_top;
    child->context.rbp = child->kernel_stack_top;
    scheduler_task_set_blocked(child);

    return child->pid;
}

int process_set_current(int pid) {
    task_t *cur = process_current_task();
    if (cur && cur->pid == pid) return 0;
    return -1;
}

int process_getpid(void) {
    task_t *t = process_current_task();
    return t ? t->pid : 0;
}
int process_getppid(void) {
    task_t *t = process_current_task();
    return t ? t->ppid : 0;
}

void process_exit_current(int code) {
    task_t *cur = process_current_task();
    task_t *parent;
    task_t *init;
    if (!cur) return;
    if (cur->pid == 1) {
        for (;;) __asm__ __volatile__("hlt");
    }
    cur->exit_code = code;
    process_adopt_orphans(cur->pid, 1);
    parent = cur->parent;
    init = task_find_by_pid(1);
    if (parent && parent->sigchld_handler == LINUX_SIG_IGN && init) {
        task_child_unlink(cur);
        task_child_link(init, cur);
        parent = init;
    }
    scheduler_task_set_zombie(cur);
#if EDGE_SCHED_PROC_DEBUG
    printf("[proc] exit pid=%d ppid=%d parent=%d st=%d acpu=%d onrq=%d\n",
           cur->pid, cur->ppid, parent ? parent->pid : -1, (int)cur->state, cur->assigned_cpu, (int)cur->on_runqueue);
#endif
    process_notify_parent_sigchld(parent);
}

int process_wait_any(int *status) {
    task_t *parent = process_current_task();
    if (!parent) return -1;

    for (;;) {
        int has_children = 0;
        for (task_t *t = parent->first_child; t; t = t->sibling_next) {
            has_children = 1;
            if (t->state == TASK_ZOMBIE) {
                int pid = t->pid;
                if (status) *status = (t->exit_code & 0xFF) << 8;
                syscall_release_process_fds(pid);
                task_child_unlink(t);
                scheduler_task_set_unused(t);
                memset(t, 0, sizeof(*t));
                return pid;
            }
        }
        if (!has_children) return -1;
#if EDGE_SCHED_PROC_DEBUG
        printf("[proc] wait_any block parent=%d st=%d acpu=%d onrq=%d\n",
               parent->pid, (int)parent->state, parent->assigned_cpu, (int)parent->on_runqueue);
#endif
        scheduler_task_set_blocked(parent);
        scheduler_yield();
    }
}

int process_wait_pid(int pid, int *status, int options) {
    task_t *parent = process_current_task();
    if (!parent) return -1;

    for (;;) {
        int has_match = 0;
        for (task_t *t = parent->first_child; t; t = t->sibling_next) {
            if (pid > 0) {
                if (t->pid != pid) continue;
            } else if (pid == 0) {
                if (t->pgid != parent->pgid) continue;
            } else if (pid < -1) {
                if (t->pgid != -pid) continue;
            } /* pid == -1: any child */
            has_match = 1;
            if (t->state == TASK_ZOMBIE) {
                int reaped = t->pid;
                if (status) *status = (t->exit_code & 0xFF) << 8;
                syscall_release_process_fds(reaped);
                task_child_unlink(t);
                scheduler_task_set_unused(t);
                memset(t, 0, sizeof(*t));
                return reaped;
            }
        }
        if (!has_match) return -1;
        if (options & 1) return 0;
#if EDGE_SCHED_PROC_DEBUG
        printf("[proc] wait_pid block parent=%d st=%d acpu=%d onrq=%d\n",
               parent->pid, (int)parent->state, parent->assigned_cpu, (int)parent->on_runqueue);
#endif
        scheduler_task_set_blocked(parent);
        scheduler_yield();
    }
}

int process_adopt_orphans(int from_ppid, int to_ppid) {
    task_t *from = task_find_by_pid(from_ppid);
    task_t *to = task_find_by_pid(to_ppid);
    int moved = 0;

    if (from) {
        task_t *next;
        for (task_t *c = from->first_child; c; c = next) {
            next = c->sibling_next;
            task_child_link(to, c);
            moved++;
        }
        return moved;
    }

    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        task_t *t = &g_tasks[i];
        if (t->state == TASK_UNUSED || t->ppid != from_ppid) continue;
        task_child_link(to, t);
        moved++;
    }
    return moved;
}

int process_kill_pid(int pid, int code) {
    task_t *t = task_find_by_pid(pid);
    task_t *parent;
    task_t *init;
    if (!t) return -1;
    if (pid == 1) return -1;
    t->exit_code = code;
    process_adopt_orphans(t->pid, 1);
    parent = t->parent;
    init = task_find_by_pid(1);
    if (parent && parent->sigchld_handler == LINUX_SIG_IGN && init) {
        task_child_unlink(t);
        task_child_link(init, t);
        parent = init;
    }
    scheduler_task_set_zombie(t);
    process_notify_parent_sigchld(parent);
    return 0;
}

void process_list_print(void) {
    printf("PID PPID S NAME\n");
    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        task_t *t = &g_tasks[i];
        if (t->state == TASK_UNUSED) continue;
        char s = '?';
        if (t->state == TASK_RUNNING || t->state == TASK_RUNNABLE) s = 'R';
        else if (t->state == TASK_BLOCKED) s = 'S';
        else if (t->state == TASK_ZOMBIE) s = 'Z';
        printf("%d %d %c %s\n", t->pid, t->ppid, s, t->name);
    }
}

int process_spawn_exec(const char *path, int argc, char **argv) {
    task_t *parent = process_current_task();
    task_t *child = task_alloc();
    if (!child) {
        if (EDGE_SPAWN_DEBUG) {
            printf("[spawn] failed: no task slot for %s\n", path ? path : "(null)");
        }
        return -1;
    }
    /* Reserve the slot immediately to avoid reuse during spawn setup. */
    child->state = TASK_BLOCKED;

    memset(child, 0, sizeof(*child));
    child->pid = g_next_pid++;
    child->ppid = parent ? parent->pid : 0;
    child->state = TASK_BLOCKED;
    child->exit_code = 0;
    child->kernel_stack_top = (uint64_t)(uintptr_t)&g_kstack[task_index(child)][KSTACK_SIZE - 16];
    child->user_stack_top = USER_STACK_TOP;
    child->user_heap_base = USER_HEAP_BASE;
    child->user_brk = USER_HEAP_BASE;
    child->user_heap_limit = USER_HEAP_BASE + USER_HEAP_DEFAULT_DELTA;
    child->user_mmap_next = USER_MMAP_BASE;
    task_clear_user_vmas(child);
    child->fs_base = 0;
    if (parent) {
        child->uid = parent->uid;
        child->gid = parent->gid;
        child->euid = parent->euid;
        child->egid = parent->egid;
        child->umask = parent->umask;
        child->pgid = parent->pgid;
        child->sid = parent->sid;
        child->ctty_kind = parent->ctty_kind;
        child->ctty_id = parent->ctty_id;
        strcpy(child->cwd, parent->cwd[0] ? parent->cwd : "/");
        child->sigint_handler = parent->sigint_handler;
        child->sigalrm_handler = parent->sigalrm_handler;
        child->sigterm_handler = parent->sigterm_handler;
        child->sigchld_handler = parent->sigchld_handler;
        child->sigaltstack_sp = parent->sigaltstack_sp;
        child->sigaltstack_size = parent->sigaltstack_size;
        child->sigaltstack_flags = parent->sigaltstack_flags;
        child->assigned_cpu = -1;
    } else {
        child->uid = 0;
        child->gid = 0;
        child->euid = 0;
        child->egid = 0;
        child->umask = 022;
        child->pgid = child->pid;
        child->sid = child->pid;
        child->ctty_kind = PROCESS_CTTY_CONSOLE;
        child->ctty_id = -1;
        strcpy(child->cwd, "/");
        child->sigint_handler = LINUX_SIG_DFL;
        child->sigalrm_handler = LINUX_SIG_DFL;
        child->sigterm_handler = LINUX_SIG_DFL;
        child->sigchld_handler = LINUX_SIG_DFL;
        child->sigaltstack_flags = 2; /* SS_DISABLE */
        child->assigned_cpu = -1;
    }
    child->sigint_pending = 0;
    child->sigalrm_pending = 0;
    child->sigterm_pending = 0;
    task_child_link(parent, child);
#if EDGE_SCHED_PROC_DEBUG
    printf("[proc] spawn link parent=%d child=%d child_ppid=%d parent_first_child=%d\n",
           parent ? parent->pid : -1, child->pid, child->ppid,
           parent && parent->first_child ? parent->first_child->pid : -1);
#endif
    task_name_from_path(path, child->name, TASK_NAME_MAX);
    child->start_argc = 0;
    child->start_argv = 0;
    child->start_pending = 0;
    child->start_envc = 0;
    child->start_envp = 0;
    child->start_at_phdr = 0;
    child->start_at_phnum = 0;
    child->start_at_entry = 0;
    child->start_at_base = 0;

    if (task_index(child) >= USER_AS_MAX_TASKS) {
        if (EDGE_SPAWN_DEBUG) {
            printf("[spawn] failed: task index out of range idx=%d path=%s\n", task_index(child), path ? path : "(null)");
        }
        task_child_unlink(child);
        scheduler_task_set_unused(child);
        memset(child, 0, sizeof(*child));
        return -1;
    }

    task_clear_user_regions(child);
    memset(&g_user_heap_ext[task_index(child)][0], 0, USER_HEAP_EXT_SIZE);
    memset(&g_user_bigpie[task_index(child)][0], 0, USER_BIGPIE_SIZE);
    task_build_address_space(child, 1);

    if (argc < 0) argc = 0;
    if (argc > 32) argc = 32;
    for (int i = 0; i < argc; ++i) {
        if (!argv || !argv[i]) break;
        int j = 0;
        const char *src = argv[i];
        while (j < 127 && src[j]) {
            child->start_argbuf[i][j] = src[j];
            ++j;
        }
        child->start_argbuf[i][j] = '\0';
        child->start_argv_local[i] = child->start_argbuf[i];
        child->start_argc++;
    }
    child->start_argv_local[child->start_argc] = 0;

    uint64_t old_cr3 = cr3_read();
    uint64_t rflags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    cr3_write(child->cr3);

    edge_elf_image_t elf_img;
    if (elf_loader_exec(path, &elf_img) < 0) {
        if (EDGE_SPAWN_DEBUG) {
            int probe = elf_loader_probe(path);
            printf("[spawn] elf load failed path=%s probe=%d parent_pid=%d child_pid=%d\n",
                   path ? path : "(null)", probe, parent ? parent->pid : 0, child->pid);
        }
        task_child_unlink(child);
        scheduler_task_set_unused(child);
        memset(child, 0, sizeof(*child));
        cr3_write(old_cr3);
        if (rflags & (1ULL << 9)) __asm__ __volatile__("sti");
        return -1;
    }

    child->start_entry = elf_img.entry_rip;
    child->start_argv = (uint64_t)(uintptr_t)&child->start_argv_local[0];
    child->start_envp = (uint64_t)(uintptr_t)&child->start_envp_local[0];
    child->start_at_phdr = elf_img.at_phdr;
    child->start_at_phnum = elf_img.at_phnum;
    child->start_at_entry = elf_img.at_entry;
    child->start_at_base = elf_img.at_base;
    child->start_pending = 1;
    memset(&child->context, 0, sizeof(child->context));
    child->context.rsp = child->kernel_stack_top;
    child->context.rbp = child->kernel_stack_top;
    child->context.rip = (uint64_t)process_enter_user_current;
#if EDGE_SCHED_PROC_DEBUG
    printf("[proc] spawn child=%d -> BLOCKED\n", child->pid);
#endif
    scheduler_task_set_blocked(child);
#if EDGE_SCHED_PROC_DEBUG
    printf("[proc] spawn child=%d -> RUNNABLE cpu=%d\n", child->pid, process_pick_target_cpu());
#endif
    scheduler_task_make_runnable(child, (uint32_t)process_pick_target_cpu());

    cr3_write(old_cr3);
    if (rflags & (1ULL << 9)) __asm__ __volatile__("sti");
    return child->pid;
}

static void process_enter_user_current(void) {
    task_t *cur = process_current_task();
    if (!cur || !cur->start_pending) {
        process_exit_current(-1);
        for (;;) __asm__ __volatile__("sti; hlt");
    }

    user_exec_image_t img;
    img.entry = cur->start_entry;
    img.user_stack_top = cur->user_stack_top;
    img.user_heap_base = cur->user_heap_base;
    img.at_phdr = cur->start_at_phdr;
    img.at_phnum = cur->start_at_phnum;
    img.at_entry = cur->start_at_entry;
    img.at_base = cur->start_at_base;
    cur->start_pending = 0;
    user_exec_run(&img, cur->start_argc, (char **)(uintptr_t)cur->start_argv,
                  cur->start_envc, (char **)(uintptr_t)cur->start_envp);
}

const task_t *process_get_task(int pid) { return task_find_by_pid(pid); }
const task_t *process_task_by_index(int index) {
    if (index < 0 || index >= PROC_MAX_TASKS) return 0;
    return &g_tasks[index];
}
task_t *process_current_task(void) { return scheduler_current_task(); }

int process_prepare_exec_current(void) {
    task_t *cur = process_current_task();
    if (!cur) return -1;
    if (task_index(cur) >= USER_AS_MAX_TASKS) return -1;

    task_clear_user_regions(cur);
    memset(&g_user_heap_ext[task_index(cur)][0], 0, USER_HEAP_EXT_SIZE);
    memset(&g_user_bigpie[task_index(cur)][0], 0, USER_BIGPIE_SIZE);
    cur->user_stack_top = USER_STACK_TOP;
    cur->user_heap_base = USER_HEAP_BASE;
    cur->user_brk = USER_HEAP_BASE;
    cur->user_heap_limit = USER_HEAP_BASE + USER_HEAP_DEFAULT_DELTA;
    cur->user_mmap_next = USER_MMAP_BASE;
    task_clear_user_vmas(cur);
    cur->fs_base = 0;
    if (cur->sigint_handler != LINUX_SIG_IGN) cur->sigint_handler = LINUX_SIG_DFL;
    if (cur->sigalrm_handler != LINUX_SIG_IGN) cur->sigalrm_handler = LINUX_SIG_DFL;
    if (cur->sigterm_handler != LINUX_SIG_IGN) cur->sigterm_handler = LINUX_SIG_DFL;
    if (cur->sigchld_handler != LINUX_SIG_IGN) cur->sigchld_handler = LINUX_SIG_DFL;
    cur->sigint_pending = 0;
    cur->sigalrm_pending = 0;
    cur->sigterm_pending = 0;
    cur->sigchld_pending = 0;
    cur->sig_stub_installed = 0;
    cur->sigaltstack_sp = 0;
    cur->sigaltstack_size = 0;
    cur->sigaltstack_flags = 2; /* SS_DISABLE */
    write_fs_base(0);
    return 0;
}

int process_set_fs_base(uint64_t base) {
    task_t *cur = process_current_task();
    if (!cur) return -1;
    cur->fs_base = base;
    write_fs_base(base);
    return 0;
}

uint64_t process_get_fs_base(void) {
    task_t *cur = process_current_task();
    if (!cur) return 0;
    return cur->fs_base;
}

uint32_t process_getuid(void) {
    task_t *cur = process_current_task();
    return cur ? cur->uid : 0;
}

uint32_t process_getgid(void) {
    task_t *cur = process_current_task();
    return cur ? cur->gid : 0;
}

uint32_t process_geteuid(void) {
    task_t *cur = process_current_task();
    return cur ? cur->euid : 0;
}

uint32_t process_getegid(void) {
    task_t *cur = process_current_task();
    return cur ? cur->egid : 0;
}

uint32_t process_setuid(uint32_t uid) {
    task_t *cur = process_current_task();
    if (!cur) return (uint32_t)-1;
    if (cur->euid == 0) {
        if (EDGE_SECURITY_DEBUG) printf("[sec] setuid pid=%d %u->%u\n", cur->pid, cur->euid, uid);
        cur->uid = uid;
        cur->euid = uid;
        return 0;
    }
    if (uid == cur->uid || uid == cur->euid) {
        if (EDGE_SECURITY_DEBUG) printf("[sec] setuid pid=%d %u->%u\n", cur->pid, cur->euid, uid);
        cur->euid = uid;
        return 0;
    }
    if (EDGE_SECURITY_DEBUG) printf("[sec] setuid denied pid=%d uid=%u euid=%u req=%u\n", cur->pid, cur->uid, cur->euid, uid);
    return (uint32_t)-1;
}

uint32_t process_setgid(uint32_t gid) {
    task_t *cur = process_current_task();
    if (!cur) return (uint32_t)-1;
    if (cur->euid == 0) {
        if (EDGE_SECURITY_DEBUG) printf("[sec] setgid pid=%d %u->%u\n", cur->pid, cur->egid, gid);
        cur->gid = gid;
        cur->egid = gid;
        return 0;
    }
    if (gid == cur->gid || gid == cur->egid) {
        if (EDGE_SECURITY_DEBUG) printf("[sec] setgid pid=%d %u->%u\n", cur->pid, cur->egid, gid);
        cur->egid = gid;
        return 0;
    }
    if (EDGE_SECURITY_DEBUG) printf("[sec] setgid denied pid=%d gid=%u egid=%u req=%u\n", cur->pid, cur->gid, cur->egid, gid);
    return (uint32_t)-1;
}

int process_setreuid(uint32_t ruid, uint32_t euid) {
    task_t *cur = process_current_task();
    if (!cur) return -1;
    if (cur->euid != 0) {
        if ((ruid != (uint32_t)-1 && ruid != cur->uid && ruid != cur->euid) ||
            (euid != (uint32_t)-1 && euid != cur->uid && euid != cur->euid)) return -1;
    }
    if (ruid != (uint32_t)-1) cur->uid = ruid;
    if (euid != (uint32_t)-1) cur->euid = euid;
    return 0;
}

int process_setregid(uint32_t rgid, uint32_t egid) {
    task_t *cur = process_current_task();
    if (!cur) return -1;
    if (cur->euid != 0) {
        if ((rgid != (uint32_t)-1 && rgid != cur->gid && rgid != cur->egid) ||
            (egid != (uint32_t)-1 && egid != cur->gid && egid != cur->egid)) return -1;
    }
    if (rgid != (uint32_t)-1) cur->gid = rgid;
    if (egid != (uint32_t)-1) cur->egid = egid;
    return 0;
}

uint32_t process_set_umask(uint32_t new_mask) {
    task_t *cur = process_current_task();
    uint32_t old = 022;
    if (!cur) return old;
    old = cur->umask;
    cur->umask = new_mask & 0777u;
    return old;
}

int process_setpgid(int pid, int pgid) {
    task_t *cur = process_current_task();
    task_t *t;
    if (!cur) return -1;
    if (pid == 0) pid = cur->pid;
    t = task_find_by_pid(pid);
    if (!t) return -1;
    if (t->sid != cur->sid) return -1;
    if (pgid == 0) pgid = t->pid;
    if (pgid < 0) return -1;
    if (pgid != t->pid) {
        task_t *leader = task_find_by_pid(pgid);
        if (!leader || leader->sid != cur->sid) return -1;
    }
    t->pgid = pgid;
    return 0;
}

int process_getpgid(int pid) {
    task_t *cur = process_current_task();
    task_t *t;
    if (!cur) return -1;
    if (pid == 0) pid = cur->pid;
    t = task_find_by_pid(pid);
    if (!t) return -1;
    return t->pgid;
}

int process_getsid(int pid) {
    task_t *cur = process_current_task();
    task_t *t;
    if (!cur) return -1;
    if (pid == 0) pid = cur->pid;
    t = task_find_by_pid(pid);
    if (!t) return -1;
    return t->sid;
}

int process_setsid(void) {
    task_t *cur = process_current_task();
    if (!cur) return -1;
    if (cur->pgid == cur->pid) return -1;
    cur->sid = cur->pid;
    cur->pgid = cur->pid;
    cur->ctty_kind = PROCESS_CTTY_NONE;
    cur->ctty_id = -1;
    return cur->sid;
}

int process_kill_pgid(int pgid, int code) {
    int killed = 0;
    if (pgid <= 0) return -1;
    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        task_t *t = &g_tasks[i];
        task_t *parent;
        if (t->state == TASK_UNUSED) continue;
        if (t->pid == 1) continue;
        if (t->pgid != pgid) continue;
        t->exit_code = code;
        process_adopt_orphans(t->pid, 1);
        scheduler_task_set_zombie(t);
        parent = t->parent;
        if (parent && parent->state == TASK_BLOCKED) {
            uint32_t wake_cpu = (parent->assigned_cpu >= 0) ? (uint32_t)parent->assigned_cpu : scheduler_cpu_id();
            scheduler_task_make_runnable(parent, wake_cpu);
        }
        killed++;
    }
    return killed > 0 ? 0 : -1;
}

static int process_signal_one(task_t *t, int sig) {
    uint64_t handler = LINUX_SIG_DFL;
    if (!t) return -1;
    if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) return -1;
    if (sig == LINUX_SIGKILL) {
        if (t->pid == 1) return -1;
        return process_kill_pid(t->pid, 128 + sig);
    }
    if (sig == LINUX_SIGINT) handler = t->sigint_handler;
    else if (sig == LINUX_SIGTERM) handler = t->sigterm_handler;
    else return -1;

    if (handler == LINUX_SIG_IGN) return 0;
    if (handler == LINUX_SIG_DFL) {
        if (t->pid == 1) return -1;
        return process_kill_pid(t->pid, 128 + sig);
    }

    if (sig == LINUX_SIGINT) t->sigint_pending = 1;
    else if (sig == LINUX_SIGTERM) t->sigterm_pending = 1;

    if (t->state == TASK_BLOCKED) {
        uint32_t wake_cpu = (t->assigned_cpu >= 0) ? (uint32_t)t->assigned_cpu : scheduler_cpu_id();
        scheduler_task_make_runnable(t, wake_cpu);
    }
    return 0;
}

int process_send_signal(int pid, int sig) {
    task_t *t;
    if (pid <= 0) return -1;
    t = task_find_by_pid(pid);
    if (!t) return -1;
    return process_signal_one(t, sig);
}

int process_send_signal_pgid(int pgid, int sig) {
    int delivered = 0;
    if (pgid <= 0) return -1;
    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        task_t *t = &g_tasks[i];
        if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) continue;
        if (t->pgid != pgid) continue;
        if (process_signal_one(t, sig) == 0) delivered++;
    }
    return delivered > 0 ? 0 : -1;
}

int process_set_state(int pid, task_state_t state) {
    task_t *t = task_find_by_pid(pid);
    if (!t) return -1;
    switch (state) {
        case TASK_RUNNABLE:
        case TASK_RUNNING: {
            uint32_t cpu = (t->assigned_cpu >= 0) ? (uint32_t)t->assigned_cpu : (uint32_t)process_pick_target_cpu();
            scheduler_task_make_runnable(t, cpu);
            break;
        }
        case TASK_BLOCKED:
            scheduler_task_set_blocked(t);
            break;
        case TASK_ZOMBIE:
            scheduler_task_set_zombie(t);
            break;
        case TASK_UNUSED:
            scheduler_task_set_unused(t);
            break;
        default:
            return -1;
    }
    return 0;
}

int process_pick_target_cpu(void) {
    return (int)scheduler_cpu_id();
}
