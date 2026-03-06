#ifndef EDGEOS_COMPAT_H
#define EDGEOS_COMPAT_H

#include <stddef.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

int spawn(const char *path, char *const argv[]);
int sys_ls(const char *path, int longf);
int sys_mkdir(const char *path);
int sys_touch(const char *path);
int sys_unlink(const char *path);
int sys_cat(const char *path);
int sys_statfs(const char *path, unsigned *total_kb, unsigned *used_kb);
int sys_meminfo(unsigned long long *total_b, unsigned long long *used_b, unsigned long long *free_b);
int sys_mounts(void);
int sys_shutdown(void);
int sys_ps(void);
int sys_kill(int pid, int sig);
int sys_sleep(unsigned ms);
int sys_dmesg(void);
int sys_stat(const char *path);
int sys_mv(const char *src, const char *dst);
int sys_writefile(const char *path, const void *buf, size_t len);
int sys_mount(const char *src, const char *target, const char *fstype);
int sys_readfile(const char *path, void *buf, size_t max_len);

#endif
