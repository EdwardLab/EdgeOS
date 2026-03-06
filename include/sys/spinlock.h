#ifndef SYS_SPINLOCK_H
#define SYS_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t v;
} spinlock_t;

static inline void spinlock_init(spinlock_t *l) {
    if (!l) return;
    l->v = 0;
}

static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t flags = 0;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&l->v, 1)) {
        while (l->v) {
            __asm__ __volatile__("pause");
        }
    }
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    __sync_lock_release(&l->v);
    if (flags & (1ULL << 9)) __asm__ __volatile__("sti");
}

#endif
