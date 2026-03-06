#ifndef SYS_USER_EXEC_H
#define SYS_USER_EXEC_H

#include <stdint.h>

#define USER_CS 0x1B
#define USER_DS 0x23
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

typedef struct user_exec_image {
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t user_heap_base;
    uint64_t at_phdr;
    uint64_t at_phnum;
    uint64_t at_entry;
    uint64_t at_base;
} user_exec_image_t;

void user_exec_set_kernel_rsp0(uint64_t rsp0);
int user_exec_run(const user_exec_image_t *img, int argc, char **argv, int envc, char **envp);

#endif
