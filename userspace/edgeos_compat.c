#include "edgeos_compat.h"

#include <sys/syscall.h>
#include <unistd.h>

#define EDGE_SYS_spawn 400
#define EDGE_SYS_ls 403
#define EDGE_SYS_mkdir 404
#define EDGE_SYS_touch 405
#define EDGE_SYS_unlink 406
#define EDGE_SYS_cat 407
#define EDGE_SYS_statfs 408
#define EDGE_SYS_meminfo 409
#define EDGE_SYS_mounts 410
#define EDGE_SYS_shutdown 411
#define EDGE_SYS_ps 412
#define EDGE_SYS_kill 413
#define EDGE_SYS_sleep 414
#define EDGE_SYS_dmesg 415
#define EDGE_SYS_stat 416
#define EDGE_SYS_mv 417
#define EDGE_SYS_writefile 418
#define EDGE_SYS_mount 419
#define EDGE_SYS_readfile 420

int spawn(const char *path, char *const argv[]) {
    return (int)syscall(EDGE_SYS_spawn, path, argv, 0);
}

int sys_ls(const char *path, int longf) {
    return (int)syscall(EDGE_SYS_ls, path, longf);
}

int sys_mkdir(const char *path) {
    return (int)syscall(EDGE_SYS_mkdir, path);
}

int sys_touch(const char *path) {
    return (int)syscall(EDGE_SYS_touch, path);
}

int sys_unlink(const char *path) {
    return (int)syscall(EDGE_SYS_unlink, path);
}

int sys_cat(const char *path) {
    return (int)syscall(EDGE_SYS_cat, path);
}

int sys_statfs(const char *path, unsigned *total_kb, unsigned *used_kb) {
    return (int)syscall(EDGE_SYS_statfs, path, total_kb, used_kb);
}

int sys_meminfo(unsigned long long *total_b, unsigned long long *used_b, unsigned long long *free_b) {
    return (int)syscall(EDGE_SYS_meminfo, total_b, used_b, free_b);
}

int sys_mounts(void) {
    return (int)syscall(EDGE_SYS_mounts);
}

int sys_shutdown(void) {
    return (int)syscall(EDGE_SYS_shutdown);
}

int sys_ps(void) {
    return (int)syscall(EDGE_SYS_ps);
}

int sys_kill(int pid, int sig) {
    return (int)syscall(EDGE_SYS_kill, pid, sig);
}

int sys_sleep(unsigned ms) {
    return (int)syscall(EDGE_SYS_sleep, ms);
}

int sys_dmesg(void) {
    return (int)syscall(EDGE_SYS_dmesg);
}

int sys_stat(const char *path) {
    return (int)syscall(EDGE_SYS_stat, path);
}

int sys_mv(const char *src, const char *dst) {
    return (int)syscall(EDGE_SYS_mv, src, dst);
}

int sys_writefile(const char *path, const void *buf, size_t len) {
    return (int)syscall(EDGE_SYS_writefile, path, buf, len);
}

int sys_mount(const char *src, const char *target, const char *fstype) {
    return (int)syscall(EDGE_SYS_mount, src, target, fstype);
}

int sys_readfile(const char *path, void *buf, size_t max_len) {
    return (int)syscall(EDGE_SYS_readfile, path, buf, max_len);
}
