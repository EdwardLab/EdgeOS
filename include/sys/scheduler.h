#ifndef SYS_SCHEDULER_H
#define SYS_SCHEDULER_H

#include <stdint.h>
#include "sys/process.h"
#include "sys/spinlock.h"

#define SCHED_MAX_CPUS 8

typedef struct scheduler_cpu {
    task_t *current;
    task_t *idle;
    task_t *rq_head;
    task_t *rq_tail;
    spinlock_t rq_lock;
    uint32_t logical_id;
} __attribute__((aligned(64))) scheduler_cpu_t;

void scheduler_init(void);
void scheduler_set_cpu_id(uint32_t logical_id);
uint32_t scheduler_cpu_id(void);
task_t *scheduler_current_task(void);
scheduler_cpu_t *scheduler_cpu_local(void);
void scheduler_set_boot_current(task_t *t);
void scheduler_task_make_runnable(task_t *t, uint32_t cpu_id);
void scheduler_task_set_blocked(task_t *t);
void scheduler_task_set_zombie(task_t *t);
void scheduler_task_set_unused(task_t *t);
void scheduler_task_set_running(task_t *t);

/* Called by platform timer interrupt (PIC today, LAPIC per-CPU in SMP bring-up). */
void scheduler_tick(void);
void scheduler_yield(void);
void scheduler_kill_current_and_yield(int code);
uint64_t scheduler_total_ticks(void);
uint64_t scheduler_idle_ticks(void);

#endif
