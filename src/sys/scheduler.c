#include "sys/scheduler.h"
#include "gdt.h"
#include "stdio.h"

extern void switch_to(cpu_context_t *prev, cpu_context_t *next);
extern void ret_from_fork(void);

#define IA32_FS_BASE_MSR 0xC0000100u

static scheduler_cpu_t g_sched_cpus[SCHED_MAX_CPUS] __attribute__((aligned(64)));
static task_t g_idle_tasks[SCHED_MAX_CPUS];
static uint8_t g_idle_stacks[SCHED_MAX_CPUS][4096] __attribute__((aligned(16)));

/* Logical CPU id must be set during BSP/AP bring-up; BSP defaults to 0. */
static volatile uint32_t g_local_cpu_id;
static int g_scheduler_ready;
static volatile uint64_t g_sched_total_ticks;
static volatile uint64_t g_sched_idle_ticks;

#ifndef EDGE_SCHED_DEBUG
#define EDGE_SCHED_DEBUG 0
#endif

static const char *sched_state_name(task_state_t st) {
    switch (st) {
        case TASK_UNUSED: return "UNUSED";
        case TASK_RUNNABLE: return "RUNNABLE";
        case TASK_RUNNING: return "RUNNING";
        case TASK_BLOCKED: return "BLOCKED";
        case TASK_ZOMBIE: return "ZOMBIE";
        default: return "???";
    }
}

static int sched_pid(task_t *t) {
    return t ? t->pid : -1;
}

static void sched_log_cpu(const char *op, scheduler_cpu_t *cpu, task_t *t) {
#if EDGE_SCHED_DEBUG
    if (!cpu) return;
    printf("[sched] %s cpu=%u pid=%d st=%s acpu=%d onrq=%d head=%d tail=%d cur=%d\n",
           op,
           cpu->logical_id,
           sched_pid(t),
           t ? sched_state_name(t->state) : "nil",
           t ? t->assigned_cpu : -1,
           t ? (int)t->on_runqueue : -1,
           sched_pid(cpu->rq_head),
           sched_pid(cpu->rq_tail),
           sched_pid(cpu->current));
#else
    (void)op; (void)cpu; (void)t;
#endif
}

static void sched_invariant_check(const char *where, scheduler_cpu_t *cpu) {
#if EDGE_SCHED_DEBUG
    if (!cpu) return;
    if (!cpu->current) {
        printf("[sched][ERR] %s cpu=%u current=NULL\n", where, cpu->logical_id);
    }
    if (cpu->rq_head == 0 && cpu->rq_tail != 0) {
        printf("[sched][ERR] %s cpu=%u head=NULL tail=%d\n", where, cpu->logical_id, sched_pid(cpu->rq_tail));
    }
    if (cpu->rq_head != 0 && cpu->rq_tail == 0) {
        printf("[sched][ERR] %s cpu=%u head=%d tail=NULL\n", where, cpu->logical_id, sched_pid(cpu->rq_head));
    }
    if (cpu->rq_head == 0 && cpu->current && !cpu->current->is_idle) {
        printf("[sched][ERR] %s cpu=%u rq-empty current-not-idle pid=%d st=%s\n",
               where, cpu->logical_id, cpu->current->pid, sched_state_name(cpu->current->state));
    }
    if (cpu->current && !cpu->current->is_idle && cpu->current->state != TASK_RUNNING) {
        printf("[sched][ERR] %s cpu=%u current pid=%d state=%s (expected RUNNING)\n",
               where, cpu->logical_id, cpu->current->pid, sched_state_name(cpu->current->state));
    }

    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        task_t *t = (task_t *)process_task_by_index(i);
        int rq_count = 0;
        int rq_owner = -1;
        int is_current_any = 0;
        if (!t || t->state == TASK_UNUSED) continue;

        for (uint32_t c = 0; c < SCHED_MAX_CPUS; ++c) {
            scheduler_cpu_t *sc = &g_sched_cpus[c];
            if (sc->current == t) is_current_any = 1;
            for (task_t *q = sc->rq_head; q; q = q->rq_next) {
                if (q == t) {
                    rq_count++;
                    rq_owner = (int)c;
                }
            }
        }

        if (t->on_runqueue && t->state != TASK_RUNNABLE) {
            printf("[sched][ERR] %s pid=%d onrq=1 state=%s\n",
                   where, t->pid, sched_state_name(t->state));
        }
        if ((t->state == TASK_RUNNABLE || t->state == TASK_RUNNING) &&
            !t->on_runqueue && !is_current_any) {
            printf("[sched][ERR] %s pid=%d state=%s onrq=0 and not current\n",
                   where, t->pid, sched_state_name(t->state));
        }
        if (rq_count > 1) {
            printf("[sched][ERR] %s pid=%d present on %d runqueues\n", where, t->pid, rq_count);
        }
        if (t->on_runqueue && rq_count == 0) {
            printf("[sched][ERR] %s pid=%d onrq=1 but not present in any runqueue\n", where, t->pid);
        }
        if (!t->on_runqueue && rq_count > 0) {
            printf("[sched][ERR] %s pid=%d onrq=0 but present in a runqueue\n", where, t->pid);
        }
        if (rq_count == 1 && t->assigned_cpu != rq_owner) {
            printf("[sched][ERR] %s pid=%d assigned_cpu=%d rq_owner=%d mismatch\n",
                   where, t->pid, t->assigned_cpu, rq_owner);
        }
    }
#else
    (void)where; (void)cpu;
#endif
}

static inline uint64_t cr3_read(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void cr3_write(uint64_t v) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory");
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

static void scheduler_idle_loop(void) {
    for (;;) {
        __asm__ __volatile__("sti; hlt");
    }
}

static inline scheduler_cpu_t *cpu_by_id(uint32_t id) {
    if (id >= SCHED_MAX_CPUS) id = 0;
    return &g_sched_cpus[id];
}

static task_t *rq_pop_head_locked(scheduler_cpu_t *cpu) {
    task_t *t = cpu->rq_head;
    if (!t) return 0;

    cpu->rq_head = t->rq_next;
    if (cpu->rq_head) cpu->rq_head->rq_prev = 0;
    else cpu->rq_tail = 0;

    t->rq_next = 0;
    t->rq_prev = 0;
    t->on_runqueue = 0;
    sched_log_cpu("rq_pop", cpu, t);
    return t;
}

static void rq_push_tail_locked(scheduler_cpu_t *cpu, task_t *t) {
    if (!cpu || !t) return;
    if (t->on_runqueue) {
        printf("[sched][ERR] rq_push existing onrq pid=%d state=%s acpu=%d\n",
               t->pid, sched_state_name(t->state), t->assigned_cpu);
        return;
    }
    t->rq_prev = cpu->rq_tail;
    t->rq_next = 0;
    if (cpu->rq_tail) cpu->rq_tail->rq_next = t;
    else cpu->rq_head = t;
    cpu->rq_tail = t;
    t->on_runqueue = 1;
    sched_log_cpu("rq_push", cpu, t);
}

static void rq_remove_locked(scheduler_cpu_t *cpu, task_t *t) {
    if (!cpu || !t || !t->on_runqueue) return;
    if (t->rq_prev) t->rq_prev->rq_next = t->rq_next;
    else cpu->rq_head = t->rq_next;

    if (t->rq_next) t->rq_next->rq_prev = t->rq_prev;
    else cpu->rq_tail = t->rq_prev;

    t->rq_prev = 0;
    t->rq_next = 0;
    t->on_runqueue = 0;
    sched_log_cpu("rq_remove", cpu, t);
}

static task_t *pick_next_runnable_locked(scheduler_cpu_t *cpu) {
    task_t *t = rq_pop_head_locked(cpu);
    while (t) {
        if (t->state == TASK_RUNNABLE) return t;
        t = rq_pop_head_locked(cpu);
    }
    return cpu->idle;
}

static void switch_task_context(task_t *prev, task_t *next) {
    if (!next) return;
    gdt_set_tss_rsp0(next->kernel_stack_top);
    cr3_write(next->cr3);
    write_fs_base(next->fs_base);

    /* switch_to stores current RIP/RSP into prev->context and jumps to next. */
    if (prev) {
        switch_to(&prev->context, &next->context);
        return;
    }

    __asm__ __volatile__(
        "mov %0, %%rsp\n"
        "push %1\n"
        "ret\n"
        :
        : "r"(next->context.rsp), "r"(next->context.rip)
        : "memory");
}

void scheduler_init(void) {
    if (g_scheduler_ready) return;

    for (uint32_t i = 0; i < SCHED_MAX_CPUS; ++i) {
        scheduler_cpu_t *cpu = &g_sched_cpus[i];
        task_t *idle = &g_idle_tasks[i];
        cpu->current = 0;
        cpu->rq_head = 0;
        cpu->rq_tail = 0;
        cpu->logical_id = i;
        spinlock_init(&cpu->rq_lock);

        idle->pid = 0;
        idle->ppid = 0;
        idle->exit_code = 0;
        idle->state = TASK_RUNNING;
        idle->on_runqueue = 0;
        idle->is_idle = 1;
        idle->assigned_cpu = (int)i;
        idle->rq_prev = 0;
        idle->rq_next = 0;
        idle->parent = 0;
        idle->first_child = 0;
        idle->sibling_prev = 0;
        idle->sibling_next = 0;
        idle->kernel_stack_top = (uint64_t)(uintptr_t)&g_idle_stacks[i][sizeof(g_idle_stacks[i]) - 16];
        idle->cr3 = cr3_read();
        idle->fs_base = 0;
        idle->context.rsp = idle->kernel_stack_top;
        idle->context.rbp = idle->kernel_stack_top;
        idle->context.rip = (uint64_t)(uintptr_t)scheduler_idle_loop;
        cpu->idle = idle;
        cpu->current = idle;
    }

    g_local_cpu_id = 0;
    g_scheduler_ready = 1;
}

void scheduler_set_cpu_id(uint32_t logical_id) {
    if (logical_id >= SCHED_MAX_CPUS) logical_id = 0;
    g_local_cpu_id = logical_id;
}

uint32_t scheduler_cpu_id(void) {
    uint32_t id = g_local_cpu_id;
    if (id >= SCHED_MAX_CPUS) id = 0;
    return id;
}

scheduler_cpu_t *scheduler_cpu_local(void) {
    if (!g_scheduler_ready) return 0;
    return cpu_by_id(scheduler_cpu_id());
}

task_t *scheduler_current_task(void) {
    scheduler_cpu_t *cpu = scheduler_cpu_local();
    task_t *cur;
    if (!cpu) return &g_idle_tasks[0];
    cur = cpu->current ? cpu->current : (cpu->idle ? cpu->idle : &g_idle_tasks[0]);
#if EDGE_SCHED_DEBUG
    printf("[sched] current cpu=%u pid=%d st=%s acpu=%d onrq=%d head=%d tail=%d cur=%d\n",
           cpu->logical_id, sched_pid(cur), sched_state_name(cur->state), cur->assigned_cpu,
           (int)cur->on_runqueue, sched_pid(cpu->rq_head), sched_pid(cpu->rq_tail), sched_pid(cpu->current));
#endif
    return cur;
}

void scheduler_set_boot_current(task_t *t) {
    scheduler_cpu_t *cpu = scheduler_cpu_local();
    if (!cpu || !t) return;

    uint64_t flags = spin_lock_irqsave(&cpu->rq_lock);
    if (t->on_runqueue) rq_remove_locked(cpu, t);
    t->assigned_cpu = (int)scheduler_cpu_id();
    t->state = TASK_RUNNING;
    cpu->current = t;
    spin_unlock_irqrestore(&cpu->rq_lock, flags);
}

void scheduler_task_make_runnable(task_t *t, uint32_t cpu_id) {
    scheduler_cpu_t *cpu;
    uint64_t flags;
    uint32_t old_cpu_id;

    if (!t || t->is_idle) return;
    if (cpu_id >= SCHED_MAX_CPUS) cpu_id = 0;
    old_cpu_id = (t->assigned_cpu >= 0) ? (uint32_t)t->assigned_cpu : cpu_id;

    if (t->on_runqueue && old_cpu_id != cpu_id) {
        scheduler_cpu_t *old_cpu = cpu_by_id(old_cpu_id);
        uint64_t old_flags = spin_lock_irqsave(&old_cpu->rq_lock);
        rq_remove_locked(old_cpu, t);
        spin_unlock_irqrestore(&old_cpu->rq_lock, old_flags);
    }

    cpu = cpu_by_id(cpu_id);
    flags = spin_lock_irqsave(&cpu->rq_lock);

    t->assigned_cpu = (int)cpu_id;
    if (t->on_runqueue) rq_remove_locked(cpu, t);
    t->state = TASK_RUNNABLE;
    rq_push_tail_locked(cpu, t);
    sched_log_cpu("make_runnable", cpu, t);
    sched_invariant_check("make_runnable", cpu);

    spin_unlock_irqrestore(&cpu->rq_lock, flags);
}

void scheduler_task_set_blocked(task_t *t) {
    scheduler_cpu_t *cpu;
    uint32_t cpu_id;
    uint64_t flags;

    if (!t || t->is_idle) return;
    cpu_id = (t->assigned_cpu >= 0) ? (uint32_t)t->assigned_cpu : scheduler_cpu_id();
    cpu = cpu_by_id(cpu_id);
    flags = spin_lock_irqsave(&cpu->rq_lock);

    if (t->on_runqueue) rq_remove_locked(cpu, t);
    t->assigned_cpu = (int)cpu_id;
    t->state = TASK_BLOCKED;
    sched_log_cpu("set_blocked", cpu, t);
    sched_invariant_check("set_blocked", cpu);

    spin_unlock_irqrestore(&cpu->rq_lock, flags);
}

void scheduler_task_set_zombie(task_t *t) {
    scheduler_cpu_t *cpu;
    uint32_t cpu_id;
    uint64_t flags;

    if (!t || t->is_idle) return;
    cpu_id = (t->assigned_cpu >= 0) ? (uint32_t)t->assigned_cpu : scheduler_cpu_id();
    cpu = cpu_by_id(cpu_id);
    flags = spin_lock_irqsave(&cpu->rq_lock);

    if (t->on_runqueue) rq_remove_locked(cpu, t);
    t->assigned_cpu = (int)cpu_id;
    t->state = TASK_ZOMBIE;
    sched_log_cpu("set_zombie", cpu, t);
    sched_invariant_check("set_zombie", cpu);

    spin_unlock_irqrestore(&cpu->rq_lock, flags);
}

void scheduler_task_set_unused(task_t *t) {
    scheduler_cpu_t *cpu;
    uint32_t cpu_id;
    uint64_t flags;

    if (!t || t->is_idle) return;
    cpu_id = (t->assigned_cpu >= 0) ? (uint32_t)t->assigned_cpu : scheduler_cpu_id();
    cpu = cpu_by_id(cpu_id);
    flags = spin_lock_irqsave(&cpu->rq_lock);

    if (t->on_runqueue) rq_remove_locked(cpu, t);
    t->assigned_cpu = -1;
    t->state = TASK_UNUSED;

    spin_unlock_irqrestore(&cpu->rq_lock, flags);
}

void scheduler_task_set_running(task_t *t) {
    scheduler_cpu_t *cpu;
    uint32_t cpu_id;
    uint64_t flags;

    if (!t || t->is_idle) return;
    cpu_id = (t->assigned_cpu >= 0) ? (uint32_t)t->assigned_cpu : scheduler_cpu_id();
    cpu = cpu_by_id(cpu_id);
    flags = spin_lock_irqsave(&cpu->rq_lock);

    if (t->on_runqueue) rq_remove_locked(cpu, t);
    t->assigned_cpu = (int)cpu_id;
    t->state = TASK_RUNNING;
    sched_log_cpu("set_running", cpu, t);
    sched_invariant_check("set_running", cpu);

    spin_unlock_irqrestore(&cpu->rq_lock, flags);
}

static void schedule_common(int tick_mode) {
    scheduler_cpu_t *cpu = scheduler_cpu_local();
    task_t *prev;
    task_t *next;
    uint64_t flags;

    if (!g_scheduler_ready || !cpu) return;

    flags = spin_lock_irqsave(&cpu->rq_lock);
    prev = cpu->current ? cpu->current : cpu->idle;
    sched_log_cpu(tick_mode ? "yield_tick" : "yield", cpu, prev);

    if (!prev) {
        spin_unlock_irqrestore(&cpu->rq_lock, flags);
        return;
    }

    if (!prev->is_idle) {
        if (tick_mode) {
            if (prev->state == TASK_RUNNING || prev->state == TASK_RUNNABLE) {
                prev->state = TASK_RUNNABLE;
                rq_push_tail_locked(cpu, prev);
            }
        } else if (prev->state == TASK_RUNNING) {
            prev->state = TASK_RUNNABLE;
            rq_push_tail_locked(cpu, prev);
        }
    }

    next = pick_next_runnable_locked(cpu);
    if (next && !next->is_idle) {
        next->assigned_cpu = (int)scheduler_cpu_id();
        next->state = TASK_RUNNING;
        next->need_resched = 0;
    }
    cpu->current = next ? next : cpu->idle;
    if (!cpu->current) {
        printf("[sched][ERR] schedule_common cpu=%u selected NULL current\n", cpu->logical_id);
        cpu->current = cpu->idle;
    }
    sched_log_cpu("yield_pick", cpu, cpu->current);
    sched_invariant_check("yield_pick", cpu);

    spin_unlock_irqrestore(&cpu->rq_lock, flags);

    if (cpu->current != prev) {
        switch_task_context(prev, cpu->current);
    }
}

void scheduler_tick(void) {
    scheduler_cpu_t *cpu = scheduler_cpu_local();
    task_t *cur;
    if (!g_scheduler_ready || !cpu) return;
    cur = cpu->current ? cpu->current : cpu->idle;
    if (!cur) return;
    g_sched_total_ticks++;
    if (cur->is_idle) g_sched_idle_ticks++;
    if (!cur->is_idle && cur->state == TASK_RUNNING) {
        cur->need_resched = 1;
    }
}

void scheduler_yield(void) {
    schedule_common(0);
}

void scheduler_kill_current_and_yield(int code) {
    process_exit_current(code);
    scheduler_yield();
    for (;;) {
        __asm__ __volatile__("sti; hlt");
    }
}

uint64_t scheduler_total_ticks(void) {
    return g_sched_total_ticks;
}

uint64_t scheduler_idle_ticks(void) {
    return g_sched_idle_ticks;
}
