#include <unistd.h>

extern long __edge_syscall1(long n, long a1);
extern long __edge_syscall3(long n, long a1, long a2, long a3);

#define SYS_write   1
#define SYS_getpid  39
#define SYS_execve  59
#define SYS_exit    60
#define SYS_wait    61

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)__edge_syscall3(SYS_write, fd, (long)buf, count);
}

int getpid(void) {
    return (int)__edge_syscall1(SYS_getpid, 0);
}

int wait(int *status) {
    return (int)__edge_syscall1(SYS_wait, (long)status);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)__edge_syscall3(SYS_execve, (long)path, (long)argv, (long)envp);
}

void _exit(int status) {
    (void)__edge_syscall3(SYS_exit, status, 0, 0);
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
