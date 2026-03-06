#include "vfs/vfs.h"
#include "net/lwip_stack.h"
#include "string.h"
#include "sys/boottime.h"
#include "sys/meminfo.h"
#include "sys/process.h"
#include "sys/scheduler.h"

#define PROC_NODE_ROOT 1
#define PROC_NODE_SELF_DIR 2
#define PROC_NODE_PID_DIR 3
#define PROC_NODE_STAT 4
#define PROC_NODE_STATUS 5
#define PROC_NODE_CMDLINE 6
#define PROC_NODE_MEMINFO 7
#define PROC_NODE_UPTIME 8
#define PROC_NODE_MOUNTS 9
#define PROC_NODE_STAT_GLOBAL 10
#define PROC_NODE_LOADAVG 11
#define PROC_NODE_NET_DIR 12
#define PROC_NODE_NET_DEV 13
#define PROC_NODE_NET_IF_INET6 14

static vfs_superblock_t g_proc_sb;

static uint32_t proc_ino_make(uint16_t node, uint32_t pid, uint32_t tag) {
    return (0xF0000000u | ((uint32_t)node << 20) | ((pid & 0xFFFu) << 8) | (tag & 0xFFu));
}

static void proc_fill_inode(vfs_inode_t *out, uint16_t node, uint32_t pid, uint16_t mode, uint32_t tag) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ino = proc_ino_make(node, pid, tag);
    out->mode = mode;
    out->size = 0;
    out->fs_private[0] = node;
    out->fs_private[1] = pid;
}

static int proc_nth_pid(uint32_t nth, uint32_t *pid_out) {
    uint32_t seen = 0;
    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        const task_t *t = process_task_by_index(i);
        if (!t || t->state == TASK_UNUSED || t->pid <= 0) continue;
        if (seen == nth) {
            *pid_out = (uint32_t)t->pid;
            return 0;
        }
        seen++;
    }
    return -1;
}

static int proc_parse_u32(const char *s, uint32_t *v) {
    uint32_t n = 0;
    if (!s || !s[0]) return -1;
    for (int i = 0; s[i]; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10u + (uint32_t)(s[i] - '0');
    }
    *v = n;
    return 0;
}

static int proc_task_state_char(task_state_t st) {
    if (st == TASK_RUNNING) return 'R';
    if (st == TASK_BLOCKED) return 'S';
    if (st == TASK_ZOMBIE) return 'Z';
    if (st == TASK_RUNNABLE) return 'R';
    return '?';
}

static int buf_append(char *buf, int max, int *off, const char *s) {
    int p = *off;
    if (p < 0 || p >= max) return -1;
    for (int i = 0; s[i]; ++i) {
        if (p + 1 >= max) return -1;
        buf[p++] = s[i];
    }
    buf[p] = 0;
    *off = p;
    return 0;
}

static int buf_append_u32(char *buf, int max, int *off, uint32_t v) {
    char t[16];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    } else {
        while (v && n < (int)sizeof(t)) {
            t[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        char c[2];
        c[0] = t[i];
        c[1] = 0;
        if (buf_append(buf, max, off, c) < 0) return -1;
    }
    return 0;
}

static int buf_append_u64_dec(char *buf, int max, int *off, uint64_t v) {
    char t[32];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    } else {
        while (v && n < (int)sizeof(t)) {
            t[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        char c[2];
        c[0] = t[i];
        c[1] = 0;
        if (buf_append(buf, max, off, c) < 0) return -1;
    }
    return 0;
}

static int buf_append_hex_u64(char *buf, int max, int *off, uint64_t v, int digits) {
    static const char hx[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; --i) {
        char c[2];
        c[0] = hx[(v >> (i * 4)) & 0xFu];
        c[1] = 0;
        if (buf_append(buf, max, off, c) < 0) return -1;
    }
    return 0;
}

static int proc_lookup(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, vfs_inode_t *out) {
    (void)sb;
    uint32_t node = dir ? dir->fs_private[0] : PROC_NODE_ROOT;
    uint32_t pid = dir ? dir->fs_private[1] : 0;

    if (!dir || !name || !out) return -1;
    if (strcmp(name, ".") == 0) { *out = *dir; return 0; }

    if (node == PROC_NODE_ROOT) {
        if (strcmp(name, "..") == 0) { *out = *dir; return 0; }
        if (strcmp(name, "self") == 0) {
            proc_fill_inode(out, PROC_NODE_SELF_DIR, 0, VFS_INODE_DIR | 0555, 1);
            return 0;
        }
        if (strcmp(name, "meminfo") == 0) {
            proc_fill_inode(out, PROC_NODE_MEMINFO, 0, VFS_INODE_FILE | 0444, 2);
            return 0;
        }
        if (strcmp(name, "uptime") == 0) {
            proc_fill_inode(out, PROC_NODE_UPTIME, 0, VFS_INODE_FILE | 0444, 3);
            return 0;
        }
        if (strcmp(name, "mounts") == 0) {
            proc_fill_inode(out, PROC_NODE_MOUNTS, 0, VFS_INODE_FILE | 0444, 4);
            return 0;
        }
        if (strcmp(name, "stat") == 0) {
            proc_fill_inode(out, PROC_NODE_STAT_GLOBAL, 0, VFS_INODE_FILE | 0444, 5);
            return 0;
        }
        if (strcmp(name, "loadavg") == 0) {
            proc_fill_inode(out, PROC_NODE_LOADAVG, 0, VFS_INODE_FILE | 0444, 6);
            return 0;
        }
        if (strcmp(name, "net") == 0) {
            proc_fill_inode(out, PROC_NODE_NET_DIR, 0, VFS_INODE_DIR | 0555, 9);
            return 0;
        }
        if (proc_parse_u32(name, &pid) == 0) {
            const task_t *t = process_get_task((int)pid);
            if (!t || t->state == TASK_UNUSED) return -1;
            proc_fill_inode(out, PROC_NODE_PID_DIR, pid, VFS_INODE_DIR | 0555, 7);
            return 0;
        }
        return -1;
    }

    if (node == PROC_NODE_NET_DIR) {
        if (strcmp(name, "..") == 0) {
            proc_fill_inode(out, PROC_NODE_ROOT, 0, VFS_INODE_DIR | 0555, 0);
            return 0;
        }
        if (strcmp(name, "dev") == 0) {
            proc_fill_inode(out, PROC_NODE_NET_DEV, 0, VFS_INODE_FILE | 0444, 10);
            return 0;
        }
        if (strcmp(name, "if_inet6") == 0) {
            proc_fill_inode(out, PROC_NODE_NET_IF_INET6, 0, VFS_INODE_FILE | 0444, 11);
            return 0;
        }
        return -1;
    }

    if (node == PROC_NODE_SELF_DIR) {
        pid = (uint32_t)process_getpid();
    }
    if (node == PROC_NODE_PID_DIR || node == PROC_NODE_SELF_DIR) {
        const task_t *t = process_get_task((int)pid);
        if (!t || t->state == TASK_UNUSED) return -1;
        if (strcmp(name, "..") == 0) {
            proc_fill_inode(out, PROC_NODE_ROOT, 0, VFS_INODE_DIR | 0555, 0);
            return 0;
        }
        if (strcmp(name, "stat") == 0) {
            proc_fill_inode(out, PROC_NODE_STAT, pid, VFS_INODE_FILE | 0444, 6);
            return 0;
        }
        if (strcmp(name, "status") == 0) {
            proc_fill_inode(out, PROC_NODE_STATUS, pid, VFS_INODE_FILE | 0444, 7);
            return 0;
        }
        if (strcmp(name, "cmdline") == 0) {
            proc_fill_inode(out, PROC_NODE_CMDLINE, pid, VFS_INODE_FILE | 0444, 8);
            return 0;
        }
        return -1;
    }
    return -1;
}

static int proc_readdir(vfs_superblock_t *sb, vfs_inode_t *dir, uint32_t idx, char *name_out, vfs_inode_t *inode_out) {
    (void)sb;
    uint32_t node = dir ? dir->fs_private[0] : PROC_NODE_ROOT;
    uint32_t pid = dir ? dir->fs_private[1] : 0;
    if (!dir || !name_out || !inode_out) return -1;

    if (node == PROC_NODE_ROOT) {
        if (idx == 0) { strcpy(name_out, "."); *inode_out = *dir; return 0; }
        if (idx == 1) { strcpy(name_out, ".."); *inode_out = *dir; return 0; }
        if (idx == 2) { strcpy(name_out, "self"); proc_fill_inode(inode_out, PROC_NODE_SELF_DIR, 0, VFS_INODE_DIR | 0555, 1); return 0; }
        if (idx == 3) { strcpy(name_out, "meminfo"); proc_fill_inode(inode_out, PROC_NODE_MEMINFO, 0, VFS_INODE_FILE | 0444, 2); return 0; }
        if (idx == 4) { strcpy(name_out, "uptime"); proc_fill_inode(inode_out, PROC_NODE_UPTIME, 0, VFS_INODE_FILE | 0444, 3); return 0; }
        if (idx == 5) { strcpy(name_out, "mounts"); proc_fill_inode(inode_out, PROC_NODE_MOUNTS, 0, VFS_INODE_FILE | 0444, 4); return 0; }
        if (idx == 6) { strcpy(name_out, "stat"); proc_fill_inode(inode_out, PROC_NODE_STAT_GLOBAL, 0, VFS_INODE_FILE | 0444, 5); return 0; }
        if (idx == 7) { strcpy(name_out, "loadavg"); proc_fill_inode(inode_out, PROC_NODE_LOADAVG, 0, VFS_INODE_FILE | 0444, 6); return 0; }
        if (idx == 8) { strcpy(name_out, "net"); proc_fill_inode(inode_out, PROC_NODE_NET_DIR, 0, VFS_INODE_DIR | 0555, 9); return 0; }
        uint32_t pid_n = 0;
        if (proc_nth_pid(idx - 9, &pid_n) < 0) return -1;
        int off = 0;
        name_out[0] = 0;
        if (buf_append_u32(name_out, VFS_NAME_MAX, &off, pid_n) < 0) return -1;
        proc_fill_inode(inode_out, PROC_NODE_PID_DIR, pid_n, VFS_INODE_DIR | 0555, 7);
        return 0;
    }

    if (node == PROC_NODE_NET_DIR) {
        if (idx == 0) { strcpy(name_out, "."); *inode_out = *dir; return 0; }
        if (idx == 1) { strcpy(name_out, ".."); proc_fill_inode(inode_out, PROC_NODE_ROOT, 0, VFS_INODE_DIR | 0555, 0); return 0; }
        if (idx == 2) { strcpy(name_out, "dev"); proc_fill_inode(inode_out, PROC_NODE_NET_DEV, 0, VFS_INODE_FILE | 0444, 10); return 0; }
        if (idx == 3) { strcpy(name_out, "if_inet6"); proc_fill_inode(inode_out, PROC_NODE_NET_IF_INET6, 0, VFS_INODE_FILE | 0444, 11); return 0; }
        return -1;
    }

    if (node == PROC_NODE_SELF_DIR) pid = (uint32_t)process_getpid();
    if (node == PROC_NODE_PID_DIR || node == PROC_NODE_SELF_DIR) {
        const task_t *t = process_get_task((int)pid);
        if (!t || t->state == TASK_UNUSED) return -1;
        if (idx == 0) { strcpy(name_out, "."); *inode_out = *dir; return 0; }
        if (idx == 1) { strcpy(name_out, ".."); proc_fill_inode(inode_out, PROC_NODE_ROOT, 0, VFS_INODE_DIR | 0555, 0); return 0; }
        if (idx == 2) { strcpy(name_out, "stat"); proc_fill_inode(inode_out, PROC_NODE_STAT, pid, VFS_INODE_FILE | 0444, 6); return 0; }
        if (idx == 3) { strcpy(name_out, "status"); proc_fill_inode(inode_out, PROC_NODE_STATUS, pid, VFS_INODE_FILE | 0444, 7); return 0; }
        if (idx == 4) { strcpy(name_out, "cmdline"); proc_fill_inode(inode_out, PROC_NODE_CMDLINE, pid, VFS_INODE_FILE | 0444, 8); return 0; }
        return -1;
    }
    return -1;
}

static int proc_render_text(vfs_inode_t *inode, char *tmp, uint32_t tmp_sz, uint32_t *n_out) {
    uint32_t node = inode->fs_private[0];
    uint32_t pid = inode->fs_private[1];
    int off = 0;
    tmp[0] = 0;

    if (node == PROC_NODE_MEMINFO) {
        uint64_t total_kb = meminfo_total_bytes() / 1024ull;
        uint64_t free_kb = meminfo_free_bytes() / 1024ull;
        uint64_t used_kb = meminfo_used_bytes() / 1024ull;
        if (buf_append(tmp, (int)tmp_sz, &off, "MemTotal:       ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, total_kb) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " kB\nMemFree:        ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, free_kb) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " kB\nMemUsed:        ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, used_kb) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " kB\n") < 0) return -1;
    } else if (node == PROC_NODE_UPTIME) {
        uint64_t us = boottime_now_us();
        uint64_t sec = us / 1000000ull;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, sec) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, ".00 0.00\n") < 0) return -1;
    } else if (node == PROC_NODE_MOUNTS) {
        int n = vfs_mounts_snapshot(tmp, tmp_sz);
        if (n < 0) return -1;
        off = n;
    } else if (node == PROC_NODE_STAT_GLOBAL) {
        uint32_t running = 0;
        uint32_t total = 0;
        uint64_t btime = (boottime_realtime_us() - boottime_monotonic_us()) / 1000000ull;
        uint64_t total_j = scheduler_total_ticks();
        uint64_t idle_j = scheduler_idle_ticks();
        uint64_t user_j = 0;
        uint64_t system_j = 0;
        for (int i = 0; i < PROC_MAX_TASKS; ++i) {
            const task_t *t = process_task_by_index(i);
            if (!t || t->state == TASK_UNUSED) continue;
            total++;
            if (t->state == TASK_RUNNING || t->state == TASK_RUNNABLE) running++;
        }
        if (idle_j > total_j) idle_j = total_j;
        if (total_j > idle_j) {
            /* Keep it simple: attribute all non-idle time to system. */
            system_j = total_j - idle_j;
        }
        if (buf_append(tmp, (int)tmp_sz, &off, "cpu  ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, user_j) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " 0 ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, system_j) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, idle_j) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " 0 0 0 0 0 0\n") < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, "intr 0\nctxt 0\n") < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, "btime ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, btime) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, "\nprocesses ") < 0) return -1;
        if (buf_append_u32(tmp, (int)tmp_sz, &off, total) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, "\nprocs_running ") < 0) return -1;
        if (buf_append_u32(tmp, (int)tmp_sz, &off, running ? running : 1) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, "\nprocs_blocked 0\n") < 0) return -1;
    } else if (node == PROC_NODE_LOADAVG) {
        uint32_t running = 0;
        uint32_t total = 0;
        for (int i = 0; i < PROC_MAX_TASKS; ++i) {
            const task_t *t = process_task_by_index(i);
            if (!t || t->state == TASK_UNUSED) continue;
            total++;
            if (t->state == TASK_RUNNING || t->state == TASK_RUNNABLE) running++;
        }
        if (buf_append_u32(tmp, (int)tmp_sz, &off, running) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, ".00 ") < 0) return -1;
        if (buf_append_u32(tmp, (int)tmp_sz, &off, running) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, ".00 ") < 0) return -1;
        if (buf_append_u32(tmp, (int)tmp_sz, &off, running) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, ".00 ") < 0) return -1;
        if (buf_append_u32(tmp, (int)tmp_sz, &off, running ? running : 1) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, "/") < 0) return -1;
        if (buf_append_u32(tmp, (int)tmp_sz, &off, total ? total : 1) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " 1\n") < 0) return -1;
    } else if (node == PROC_NODE_NET_DEV) {
        uint64_t rx_packets = 0;
        uint64_t rx_bytes = 0;
        uint64_t tx_packets = 0;
        uint64_t tx_bytes = 0;
        lwip_stack_get_link_stats(&rx_packets, &rx_bytes, &tx_packets, &tx_bytes);
        if (buf_append(tmp, (int)tmp_sz, &off, "Inter-|   Receive                                                |  Transmit\n") < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n") < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, "  lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n") < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, "eth0: ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, rx_bytes) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, rx_packets) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " 0 0 0 0 0 0 ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, tx_bytes) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
        if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, tx_packets) < 0) return -1;
        if (buf_append(tmp, (int)tmp_sz, &off, " 0 0 0 0 0 0\n") < 0) return -1;
    } else if (node == PROC_NODE_NET_IF_INET6) {
        uint8_t addr[16];
        uint8_t plen = 0;
        uint8_t scope = 0;
        uint8_t flags = 0;
        if (buf_append(tmp, (int)tmp_sz, &off, "00000000000000000000000000000001 01 80 10 80       lo\n") < 0) return -1;
        for (int ord = 0; ord < 16; ++ord) {
            if (lwip_stack_get_ipv6_addr_at(ord, addr, &plen, &scope, &flags) < 0) break;
            for (int i = 0; i < 16; ++i) {
                if (buf_append_hex_u64(tmp, (int)tmp_sz, &off, addr[i], 2) < 0) return -1;
            }
            if (buf_append(tmp, (int)tmp_sz, &off, " 00000002 ") < 0) return -1;
            if (buf_append_hex_u64(tmp, (int)tmp_sz, &off, plen, 2) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
            if (buf_append_hex_u64(tmp, (int)tmp_sz, &off, scope, 2) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
            if (buf_append_hex_u64(tmp, (int)tmp_sz, &off, flags, 2) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "       eth0\n") < 0) return -1;
        }
    } else if (node == PROC_NODE_STAT || node == PROC_NODE_STATUS || node == PROC_NODE_CMDLINE) {
        const task_t *t = process_get_task((int)pid);
        if (!t || t->state == TASK_UNUSED) return -1;
        if (node == PROC_NODE_CMDLINE) {
            int n = (int)strlen(t->name);
            if ((uint32_t)(n + 1) > tmp_sz) n = (int)tmp_sz - 1;
            memcpy(tmp, t->name, (uint32_t)n);
            tmp[n] = 0;
            off = n + 1;
        } else if (node == PROC_NODE_STATUS) {
            if (buf_append(tmp, (int)tmp_sz, &off, "Name:\t") < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, t->name) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\nState:\t") < 0) return -1;
            {
                char s[2];
                s[0] = (char)proc_task_state_char(t->state);
                s[1] = 0;
                if (buf_append(tmp, (int)tmp_sz, &off, s) < 0) return -1;
            }
            if (buf_append(tmp, (int)tmp_sz, &off, "\nPid:\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, (uint32_t)t->pid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\nPPid:\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, (uint32_t)t->ppid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\nUid:\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->uid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->euid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->euid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->euid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\nGid:\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->gid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->egid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->egid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->egid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\nGroups:\t") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, t->gid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, "\n") < 0) return -1;
        } else {
            uint64_t vsize = 6ull * 1024ull * 1024ull;
            uint64_t rss_pages = vsize / 4096ull;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, (uint32_t)t->pid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, " (") < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, t->name) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, ") ") < 0) return -1;
            {
                char s[2];
                s[0] = (char)proc_task_state_char(t->state);
                s[1] = 0;
                if (buf_append(tmp, (int)tmp_sz, &off, s) < 0) return -1;
            }
            if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, (uint32_t)t->ppid) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, (uint32_t)(t->pgid > 0 ? t->pgid : t->pid)) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
            if (buf_append_u32(tmp, (int)tmp_sz, &off, (uint32_t)(t->sid > 0 ? t->sid : t->pid)) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, " 0 -1 0 0 0 0 0 0 0 0 0 0 20 0 1 0 1 ") < 0) return -1;
            if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, vsize) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, " ") < 0) return -1;
            if (buf_append_u64_dec(tmp, (int)tmp_sz, &off, rss_pages) < 0) return -1;
            if (buf_append(tmp, (int)tmp_sz, &off, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n") < 0) return -1;
        }
    } else {
        return -1;
    }

    *n_out = (uint32_t)off;
    return 0;
}

static int proc_read(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, void *buf, uint32_t len) {
    (void)sb;
    static char tmp[2048];
    uint32_t n = 0;
    if (!inode || !buf) return -1;
    if (proc_render_text(inode, tmp, sizeof(tmp), &n) < 0) return -1;
    if (off >= n) return 0;
    if (off + len > n) len = n - off;
    memcpy(buf, tmp + off, len);
    return (int)len;
}

static int proc_write(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, const void *buf, uint32_t len) {
    (void)sb;
    (void)inode;
    (void)off;
    (void)buf;
    (void)len;
    return -1;
}

static int proc_create(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out) {
    (void)sb; (void)dir; (void)name; (void)mode; (void)out;
    return -1;
}

static int proc_mkdir(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out) {
    (void)sb; (void)dir; (void)name; (void)mode; (void)out;
    return -1;
}

static int proc_unlink(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name) {
    (void)sb; (void)dir; (void)name;
    return -1;
}

static int proc_statfs(vfs_superblock_t *sb, uint32_t *total_kb, uint32_t *used_kb) {
    (void)sb;
    if (!total_kb || !used_kb) return -1;
    *total_kb = 0;
    *used_kb = 0;
    return 0;
}

static filesystem_ops_t g_procfs_ops = {
    proc_lookup,
    proc_read,
    proc_write,
    proc_create,
    proc_mkdir,
    proc_unlink,
    proc_readdir,
    proc_statfs
};

int procfs_mount(const char *dev, const char *target) {
    (void)dev;
    if (!target) return -1;
    memset(&g_proc_sb, 0, sizeof(g_proc_sb));
    strcpy(g_proc_sb.fs_name, "proc");
    strcpy(g_proc_sb.dev_name, "proc");
    strcpy(g_proc_sb.mountpoint, target);
    proc_fill_inode(&g_proc_sb.root, PROC_NODE_ROOT, 0, VFS_INODE_DIR | 0555, 0);
    g_proc_sb.ops = &g_procfs_ops;
    g_proc_sb.fs_private = 0;
    return vfs_add_superblock(&g_proc_sb);
}
