#ifndef EDGE_MUSL_UNISTD_H
#define EDGE_MUSL_UNISTD_H

#include <stddef.h>

ssize_t write(int fd, const void *buf, size_t count);
int getpid(void);
int wait(int *status);
int execve(const char *path, char *const argv[], char *const envp[]);
void _exit(int status);

#endif
