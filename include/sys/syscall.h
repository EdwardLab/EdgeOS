#ifndef SYS_SYSCALL_H
#define SYS_SYSCALL_H

#include <stdint.h>

#define EDGE_SYS_read    0
#define EDGE_SYS_write   1
#define EDGE_SYS_open    2
#define EDGE_SYS_close   3
#define EDGE_SYS_getpid  39
#define EDGE_SYS_fork    57
#define EDGE_SYS_execve  59
#define EDGE_SYS_exit    60
#define EDGE_SYS_wait    61
#define EDGE_SYS_brk     12
#define EDGE_SYS_spawn   400
#define EDGE_SYS_getcwd  401
#define EDGE_SYS_chdir   402
#define EDGE_SYS_ls      403
#define EDGE_SYS_mkdir   404
#define EDGE_SYS_touch   405
#define EDGE_SYS_unlink  406
#define EDGE_SYS_cat     407
#define EDGE_SYS_statfs  408
#define EDGE_SYS_meminfo 409
#define EDGE_SYS_mounts  410
#define EDGE_SYS_shutdown 411
#define EDGE_SYS_ps      412
#define EDGE_SYS_kill    413
#define EDGE_SYS_sleep   414
#define EDGE_SYS_dmesg   415
#define EDGE_SYS_stat    416
#define EDGE_SYS_mv      417
#define EDGE_SYS_writefile 418
#define EDGE_SYS_mount   419
#define EDGE_SYS_readfile 420

void syscall_init(void);
void syscall_release_process_fds(int pid);
void syscall_tty_irq_poll(void);

#endif
