#include "vfs/vfs.h"
#include "string.h"
#include "stdio.h"
#include "console.h"
#include "keyboard.h"
#include "fb.h"
#include "dev/fbdev.h"
#include "sys/process.h"

typedef struct { char name[16]; int (*mount_fn)(const char *dev, const char *target); } fsreg_t;
typedef struct { char name[VFS_NAME_MAX]; uint16_t mode; uint16_t kind; void *ptr; } devnode_t;

#define ENOSYS 38
#define EINVAL 22
#define EACCES 13

#ifndef EDGE_SECURITY_DEBUG
#define EDGE_SECURITY_DEBUG 0
#endif

enum {
    DEV_KIND_NONE = 0,
    DEV_KIND_BLOCK,
    DEV_KIND_TTY0,
    DEV_KIND_NULL,
    DEV_KIND_ZERO,
    DEV_KIND_FB0,
    DEV_KIND_RANDOM,
    DEV_KIND_URANDOM,
    DEV_KIND_PTMX
};

static vfs_superblock_t g_mounts[VFS_MAX_MOUNTS];
static int g_mount_count;
static fsreg_t g_fsreg[4];
static int g_fsreg_count;
static char g_cwd[VFS_PATH_MAX] = "/";
static devnode_t g_devnodes[BLOCK_MAX_DEVICES + 16];
static int g_devnode_count;
static uint64_t g_rng_state = 0x9E3779B97F4A7C15ULL;
static uint32_t g_rng_entropy_bits;
static uint32_t g_rng_last_timer_ticks;
static uint64_t g_rng_last_kbd_irqs;
// Turn on FS debug for more verbose output during path resolution and operations.
//#define VFS_DEBUG(fmt, ...) printf("[vfs] " fmt "\n", ##__VA_ARGS__)
#define VFS_DEBUG(fmt, ...) ((void)0)

static char *vfs_current_cwd_ptr(void) {
    task_t *t = process_current_task();
    if (t && t->state != TASK_UNUSED) {
        if (!t->cwd[0]) strcpy(t->cwd, "/");
        return t->cwd;
    }
    return g_cwd;
}

extern int ext2_mount(const char *dev, const char *target);
extern int ext2_mount_block(block_device_t *bdev, const char *target);
extern int ext2_setattr(vfs_superblock_t *sb, const vfs_inode_t *inode, uint16_t mode, uint16_t uid, uint16_t gid, uint32_t mask);
extern int ext4_mount(const char *dev, const char *target);
extern int ext4_mount_block(block_device_t *bdev, const char *target);
extern int ext4_setattr(vfs_superblock_t *sb, const vfs_inode_t *inode, uint16_t mode, uint16_t uid, uint16_t gid, uint32_t mask);
extern int fat32_mount(const char *dev, const char *target);
extern int procfs_mount(const char *dev, const char *target);
extern volatile uint32_t g_timer_ticks;

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

static void rng_mix_u64(uint64_t v, uint32_t entropy_bits) {
    g_rng_state ^= mix64(v + 0x9E3779B97F4A7C15ULL + (g_rng_state << 7) + (g_rng_state >> 3));
    if (entropy_bits > 64) entropy_bits = 64;
    g_rng_entropy_bits += entropy_bits;
    if (g_rng_entropy_bits > 256) g_rng_entropy_bits = 256;
}

static void rng_collect_entropy(void) {
    uint32_t ticks = g_timer_ticks;
    uint64_t kbd_irqs = keyboard_entropy_irq_count();
    uint64_t tsc = rdtsc64();
    rng_mix_u64(tsc, 1);
    if (ticks != g_rng_last_timer_ticks) {
        rng_mix_u64(((uint64_t)ticks << 32) ^ tsc, 2);
        g_rng_last_timer_ticks = ticks;
    }
    if (kbd_irqs != g_rng_last_kbd_irqs) {
        rng_mix_u64(keyboard_entropy_last_tsc() ^ (kbd_irqs << 1), 4);
        g_rng_last_kbd_irqs = kbd_irqs;
    }
}

static uint64_t rng_next_u64(void) {
    uint64_t x = g_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void rng_fill_bytes(uint8_t *out, uint32_t len, int blocking) {
    uint64_t pool = 0;
    uint32_t pool_n = 0;
    if (!out || len == 0) return;
    for (uint32_t i = 0; i < len; ++i) {
        for (;;) {
            rng_collect_entropy();
            if (!blocking || g_rng_entropy_bits >= 8) break;
            __asm__ __volatile__("sti; hlt");
        }
        if (pool_n == 0) {
            pool = rng_next_u64();
            pool_n = 8;
        }
        out[i] = (uint8_t)pool;
        pool >>= 8;
        pool_n--;
        if (blocking && g_rng_entropy_bits) g_rng_entropy_bits--;
    }
}

static int vfs_add_devnode(const char *name, uint16_t mode, uint16_t kind, void *ptr) {
    if (g_devnode_count >= (int)(sizeof(g_devnodes) / sizeof(g_devnodes[0]))) return -1;
    strcpy(g_devnodes[g_devnode_count].name, name);
    g_devnodes[g_devnode_count].mode = mode;
    g_devnodes[g_devnode_count].kind = kind;
    g_devnodes[g_devnode_count].ptr = ptr;
    g_devnode_count++;
    return 0;
}

static void vfs_inode_set_ptr(vfs_inode_t *ino, void *p) {
    uintptr_t v = (uintptr_t)p;
    ino->fs_private[0] = (uint32_t)(v & 0xFFFFFFFFu);
    ino->fs_private[1] = (uint32_t)((v >> 32) & 0xFFFFFFFFu);
}

static void *vfs_inode_get_ptr(const vfs_inode_t *ino) {
    uintptr_t v = (uintptr_t)ino->fs_private[0] | ((uintptr_t)ino->fs_private[1] << 32);
    return (void *)v;
}

static void vfs_build_devnodes(void) {
    g_devnode_count = 0;
    for (int i = 0; i < block_count(); ++i) {
        block_device_t *b = block_get(i);
        if (!b || !b->present) continue;
        if (vfs_add_devnode(b->name, VFS_INODE_BLK | 0660, DEV_KIND_BLOCK, b) < 0) break;
    }
    vfs_add_devnode("console", VFS_INODE_CHR | 0666, DEV_KIND_TTY0, 0);
    vfs_add_devnode("tty0", VFS_INODE_CHR | 0666, DEV_KIND_TTY0, 0);
    vfs_add_devnode("tty1", VFS_INODE_CHR | 0666, DEV_KIND_TTY0, 0);
    vfs_add_devnode("tty2", VFS_INODE_CHR | 0666, DEV_KIND_TTY0, 0);
    vfs_add_devnode("tty3", VFS_INODE_CHR | 0666, DEV_KIND_TTY0, 0);
    vfs_add_devnode("tty4", VFS_INODE_CHR | 0666, DEV_KIND_TTY0, 0);
    vfs_add_devnode("tty", VFS_INODE_CHR | 0666, DEV_KIND_TTY0, 0);
    vfs_add_devnode("null", VFS_INODE_CHR | 0666, DEV_KIND_NULL, 0);
    vfs_add_devnode("zero", VFS_INODE_CHR | 0666, DEV_KIND_ZERO, 0);
    vfs_add_devnode("fb0", VFS_INODE_CHR | 0660, DEV_KIND_FB0, 0);
    vfs_add_devnode("random", VFS_INODE_CHR | 0666, DEV_KIND_RANDOM, 0);
    vfs_add_devnode("urandom", VFS_INODE_CHR | 0666, DEV_KIND_URANDOM, 0);
    vfs_add_devnode("ptmx", VFS_INODE_CHR | 0666, DEV_KIND_PTMX, 0);
    rng_collect_entropy();
}

static int vfs_devnode_find(const char *name) {
    for (int i = 0; i < g_devnode_count; ++i) {
        if (strcmp(g_devnodes[i].name, (char *)name) == 0) return i;
    }
    return -1;
}

static int vfs_devnode_from_inode(const vfs_inode_t *ino) {
    if (!ino) return -1;
    if (ino->ino < 0xD0000000u) return -1;
    uint32_t idx = ino->ino - 0xD0000000u;
    if (idx >= (uint32_t)g_devnode_count) return -1;
    return (int)idx;
}

static int vfs_read_tty(char *out, uint32_t max) {
    if (max == 0) return 0;
    uint32_t i = 0;
    for (; i < max; ++i) {
        int ch = keyboard_getchar();
        if (ch < 0) break;
        out[i] = (char)ch;
        if ((char)ch == '\n') {
            ++i;
            break;
        }
    }
    return (int)i;
}

static int vfs_dev_read_chr(const devnode_t *dn, char *out, uint32_t max) {
    if (!dn) return -1;
    switch (dn->kind) {
        case DEV_KIND_TTY0:
            return vfs_read_tty(out, max);
        case DEV_KIND_NULL:
            return 0;
        case DEV_KIND_ZERO:
            if (max) memset(out, 0, max);
            return (int)max;
        case DEV_KIND_FB0:
            return -ENOSYS;
        case DEV_KIND_RANDOM:
            rng_fill_bytes((uint8_t *)out, max, 1);
            return (int)max;
        case DEV_KIND_URANDOM:
            rng_fill_bytes((uint8_t *)out, max, 0);
            return (int)max;
        case DEV_KIND_PTMX:
            return -ENOSYS;
        default:
            return -1;
    }
}

static int vfs_dev_write_chr(const devnode_t *dn, const char *buf, uint32_t len) {
    if (!dn) return -1;
    switch (dn->kind) {
        case DEV_KIND_TTY0:
            for (uint32_t i = 0; i < len; ++i) console_putchar(buf[i]);
            return (int)len;
        case DEV_KIND_NULL:
        case DEV_KIND_ZERO:
            return (int)len;
        case DEV_KIND_FB0: {
            uint32_t fb_bytes;
            uint32_t n;
            uint8_t *dst;
            if (!fb.addr || fb.pitch == 0 || fb.height == 0) return -1;
            fb_bytes = fb.pitch * fb.height;
            n = (len < fb_bytes) ? len : fb_bytes;
            dst = fb_get_draw_buffer();
            if (!dst) dst = fb.addr;
            if (n) memcpy(dst, buf, n);
            if (dst != fb.addr) fb_present();
            return (int)n;
        }
        case DEV_KIND_RANDOM:
        case DEV_KIND_URANDOM:
            for (uint32_t i = 0; i < len; ++i) rng_mix_u64((uint8_t)buf[i] + ((uint64_t)i << 8), 1);
            return (int)len;
        case DEV_KIND_PTMX:
            return -ENOSYS;
        default:
            return -1;
    }
}

static int parse_decimal_str(const char *s, int *out) {
    int v = 0;
    if (!s || !s[0] || !out) return -1;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v < 0 || v > 1000000) return -1;
    }
    *out = v;
    return 0;
}

static int dev_input_is_mouse_stream(const char *name) {
    if (!name) return 0;
    return strcmp(name, "input/mice") == 0 || strcmp(name, "input/mouse0") == 0;
}

static int dev_input_is_event_stream(const char *name) {
    if (!name) return 0;
    return strcmp(name, "input/event0") == 0;
}

static uint16_t vfs_current_umask(void) {
    task_t *t = process_current_task();
    return t ? (uint16_t)(t->umask & 0777u) : 0;
}

static int vfs_try_resolve_devpath(const char *abs, vfs_inode_t *out_inode, vfs_superblock_t **out_sb, vfs_inode_t *out_parent) {
    if (!abs) return -1;
    if (strcmp(abs, "/dev") == 0) return -1;
    if (strncmp(abs, "/dev/", 5) != 0) return -1;
    const char *name = abs + 5;
    if (strcmp(name, "pts") == 0) {
        if (out_parent) {
            vfs_inode_t devdir;
            if (vfs_resolve("/dev", &devdir, 0, 0, 0) == 0) *out_parent = devdir;
            else memset(out_parent, 0, sizeof(*out_parent));
        }
        if (out_sb) *out_sb = 0;
        if (out_inode) {
            memset(out_inode, 0, sizeof(*out_inode));
            out_inode->ino = 0xD0FFF000u;
            out_inode->mode = VFS_INODE_DIR | 0755;
            out_inode->uid = 0;
            out_inode->gid = 0;
            out_inode->size = 0;
            vfs_inode_set_ptr(out_inode, 0);
        }
        return 0;
    }
    if (strcmp(name, "input") == 0) {
        if (out_parent) {
            vfs_inode_t devdir;
            if (vfs_resolve("/dev", &devdir, 0, 0, 0) == 0) *out_parent = devdir;
            else memset(out_parent, 0, sizeof(*out_parent));
        }
        if (out_sb) *out_sb = 0;
        if (out_inode) {
            memset(out_inode, 0, sizeof(*out_inode));
            out_inode->ino = 0xD0FFE000u;
            out_inode->mode = VFS_INODE_DIR | 0755;
            out_inode->uid = 0;
            out_inode->gid = 0;
            out_inode->size = 0;
            vfs_inode_set_ptr(out_inode, 0);
        }
        return 0;
    }
    if (dev_input_is_mouse_stream(name) || dev_input_is_event_stream(name)) {
        if (out_parent) {
            memset(out_parent, 0, sizeof(*out_parent));
            out_parent->ino = 0xD0FFE000u;
            out_parent->mode = VFS_INODE_DIR | 0755;
            out_parent->uid = 0;
            out_parent->gid = 0;
            out_parent->size = 0;
            vfs_inode_set_ptr(out_parent, 0);
        }
        if (out_sb) *out_sb = 0;
        if (out_inode) {
            memset(out_inode, 0, sizeof(*out_inode));
            out_inode->ino = dev_input_is_event_stream(name) ? 0xD0FFE101u : 0xD0FFE100u;
            out_inode->mode = VFS_INODE_CHR | 0666;
            out_inode->uid = 0;
            out_inode->gid = 0;
            out_inode->size = 0;
            vfs_inode_set_ptr(out_inode, 0);
        }
        return 0;
    }
    if (strncmp(name, "pts/", 4) == 0) {
        int idx = 0;
        if (parse_decimal_str(name + 4, &idx) < 0) return -1;
        if (out_parent) {
            memset(out_parent, 0, sizeof(*out_parent));
            out_parent->ino = 0xD0FFF000u;
            out_parent->mode = VFS_INODE_DIR | 0755;
            out_parent->uid = 0;
            out_parent->gid = 0;
            out_parent->size = 0;
            vfs_inode_set_ptr(out_parent, 0);
        }
        if (out_sb) *out_sb = 0;
        if (out_inode) {
            memset(out_inode, 0, sizeof(*out_inode));
            out_inode->ino = 0xD0FFF100u + (uint32_t)idx;
            out_inode->mode = VFS_INODE_CHR | 0620;
            out_inode->uid = 0;
            out_inode->gid = 0;
            out_inode->size = 0;
            vfs_inode_set_ptr(out_inode, 0);
        }
        return 0;
    }
    if (!name[0]) return -1;
    for (const char *p = name; *p; ++p) if (*p == '/') return -1;

    int idx = vfs_devnode_find(name);
    if (idx < 0) return -1;

    if (out_parent) {
        vfs_inode_t devdir;
        if (vfs_resolve("/dev", &devdir, 0, 0, 0) == 0) *out_parent = devdir;
        else memset(out_parent, 0, sizeof(*out_parent));
    }
    if (out_sb) *out_sb = 0;
    if (out_inode) {
        memset(out_inode, 0, sizeof(*out_inode));
        out_inode->ino = (uint32_t)(0xD0000000u + (uint32_t)idx);
        out_inode->mode = g_devnodes[idx].mode;
        out_inode->uid = 0;
        out_inode->gid = 0;
        out_inode->size = 0;
        vfs_inode_set_ptr(out_inode, g_devnodes[idx].ptr);
    }
    return 0;
}

void vfs_init(void) {
    memset(g_mounts, 0, sizeof(g_mounts));
    g_mount_count = 0;
    g_fsreg_count = 0;
    g_devnode_count = 0;
    strcpy(g_cwd, "/");
    vfs_register("ext2", ext2_mount);
    vfs_register("ext4", ext4_mount);
    vfs_register("fat32", fat32_mount);
    vfs_register("proc", procfs_mount);
    vfs_build_devnodes();
}

int vfs_register(const char *name, int (*mount_fn)(const char *dev, const char *target)) {
    if (g_fsreg_count >= 4) return -1;
    strcpy(g_fsreg[g_fsreg_count].name, name);
    g_fsreg[g_fsreg_count].mount_fn = mount_fn;
    g_fsreg_count++;
    return 0;
}

static vfs_superblock_t *vfs_find_mount(const char *path) {
    int best = -1;
    int best_len = -1;
    for (int i = 0; i < g_mount_count; ++i) {
        int ml = (int)strlen(g_mounts[i].mountpoint);
        if (ml > best_len && strncmp(path, g_mounts[i].mountpoint, ml) == 0 && (path[ml] == 0 || path[ml] == '/' || ml == 1)) {
            best = i; best_len = ml;
        }
    }
    return best >= 0 ? &g_mounts[best] : 0;
}

int vfs_mount(const char *dev, const char *target, const char *fsname) {
    (void)dev; (void)target; (void)fsname;
    for (int i = 0; i < g_fsreg_count; ++i) {
        if (strcmp(g_fsreg[i].name, (char *)fsname) == 0) return g_fsreg[i].mount_fn(dev, target);
    }
    return -1;
}

int vfs_mount_blockdev(block_device_t *dev, const char *target, const char *fsname) {
    if (!dev || !target || !fsname) return -1;
    if (strcmp((char *)fsname, "ext4") == 0) return ext4_mount_block(dev, target);
    if (strcmp((char *)fsname, "ext2") == 0) return ext2_mount_block(dev, target);
    return -1;
}

int vfs_add_superblock(vfs_superblock_t *sb) {
    if (g_mount_count >= VFS_MAX_MOUNTS) return -1;
    g_mounts[g_mount_count++] = *sb;
    return 0;
}

static void normalize_path(const char *in, char *out) {
    char tmp[VFS_PATH_MAX];
    const char *cwd = vfs_current_cwd_ptr();
    if (!in || !in[0]) {
        strcpy(tmp, cwd);
    } else if (in[0] == '/') {
        strcpy(tmp, in);
    } else {
        strcpy(tmp, cwd);
        if (strcmp(tmp, "/") != 0) strcat(tmp, "/");
        strcat(tmp, in);
    }

    out[0] = '/';
    out[1] = 0;
    int oi = 1;
    int i = 0;
    while (tmp[i]) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        int start = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - start;
        if (len == 1 && tmp[start] == '.') continue;
        if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (oi > 1) {
                oi--;
                while (oi > 0 && out[oi - 1] != '/') oi--;
                out[oi] = 0;
            }
            continue;
        }
        if (oi > 1 && oi < VFS_PATH_MAX - 1) out[oi++] = '/';
        for (int k = 0; k < len && oi < VFS_PATH_MAX - 1; ++k) out[oi++] = tmp[start + k];
        out[oi] = 0;
    }
    if (oi == 0) { out[0] = '/'; out[1] = 0; }
}

static int path_split_last(const char *path, char *parent, char *leaf) {
    int len = (int)strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    int cut = len - 1;
    while (cut > 0 && path[cut] != '/') cut--;
    int n = len - cut - 1;
    if (n <= 0 || n >= VFS_NAME_MAX) return -1;
    memcpy(leaf, path + cut + 1, n); leaf[n] = 0;
    if (cut == 0) strcpy(parent, "/");
    else { memcpy(parent, path, cut); parent[cut] = 0; }
    return 0;
}

int vfs_resolve(const char *path, vfs_inode_t *out_inode, vfs_superblock_t **out_sb, vfs_inode_t *out_parent, char *leaf) {
    char abs[VFS_PATH_MAX];
    normalize_path(path, abs);
    if (vfs_try_resolve_devpath(abs, out_inode, out_sb, out_parent) == 0) return 0;
    VFS_DEBUG("resolve input='%s' abs='%s'", path ? path : "", abs);
    vfs_superblock_t *sb = vfs_find_mount(abs);
    if (!sb) {
        VFS_DEBUG("resolve failed: no mount for '%s'", abs);
        return -1;
    }

    char rel[VFS_PATH_MAX];
    int ml = (int)strlen(sb->mountpoint);
    if (ml == 1) strcpy(rel, abs + 1);
    else strcpy(rel, abs + ml + (abs[ml] == '/' ? 1 : 0));
    VFS_DEBUG("resolve mount='%s' rel='%s'", sb->mountpoint, rel);

    vfs_inode_t cur = sb->root;
    if (out_parent) *out_parent = cur;
    if (!rel[0]) {
        if (out_inode) *out_inode = cur;
        if (out_sb) *out_sb = sb;
        VFS_DEBUG("resolve success inode=%u (root)", cur.ino);
        return 0;
    }

    char part[VFS_NAME_MAX];
    const char *p = rel;
    while (*p) {
        int pi = 0;
        while (*p && *p != '/') { if (pi < VFS_NAME_MAX - 1) part[pi++] = *p; p++; }
        part[pi] = 0;
        if (*p == '/') p++;
        if (!part[0] || strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) continue;
        if (out_parent) *out_parent = cur;
        VFS_DEBUG("resolve step part='%s' parent_ino=%u", part, cur.ino);
        if (sb->ops->lookup(sb, &cur, part, &cur) < 0) {
            if (leaf) strcpy(leaf, part);
            if (out_sb) *out_sb = sb;
            VFS_DEBUG("resolve miss part='%s' under parent_ino=%u", part, out_parent ? out_parent->ino : 0);
            return -1;
        }
        VFS_DEBUG("resolve hit part='%s' inode=%u", part, cur.ino);
    }
    if (out_inode) *out_inode = cur;
    if (out_sb) *out_sb = sb;
    VFS_DEBUG("resolve success inode=%u", cur.ino);
    return 0;
}

int vfs_read_file(const char *path, char *out, uint32_t max) {
    vfs_inode_t ino; vfs_superblock_t *sb;
    if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) return -1;
    if (vfs_permission_check(&ino, 4, process_current_task()) < 0) return -1;
    if (path && (strcmp(path, "/dev/input/mice") == 0 || strcmp(path, "/dev/input/mouse0") == 0)) {
        return keyboard_mouse_read(out, max, 0);
    }
    if (path && strcmp(path, "/dev/input/event0") == 0) {
        return keyboard_mouse_event_read(out, max, 0);
    }
    if ((ino.mode & 0xF000) == VFS_INODE_CHR) {
        int idx = vfs_devnode_from_inode(&ino);
        if (idx >= 0) return vfs_dev_read_chr(&g_devnodes[idx], out, max);
        return vfs_read_tty(out, max);
    }
    if ((ino.mode & 0xF000) == VFS_INODE_BLK) {
        block_device_t *b = (block_device_t *)vfs_inode_get_ptr(&ino);
        static uint8_t secbuf[65536];
        if (!b || !b->ops.read_sectors || b->sector_size == 0) return -1;
        if (max == 0) return 0;
        uint32_t sectors = (max + b->sector_size - 1) / b->sector_size;
        if (sectors * b->sector_size > sizeof(secbuf)) sectors = sizeof(secbuf) / b->sector_size;
        if (sectors == 0) sectors = 1;
        if (block_read_sectors(b, 0, sectors, secbuf) < 0) return -1;
        uint32_t n = sectors * b->sector_size;
        if (n > max) n = max;
        memcpy(out, secbuf, n);
        return (int)n;
    }
    if (!sb || !sb->ops->read) return -1;
    return sb->ops->read(sb, &ino, 0, out, max);
}

int vfs_write_file(const char *path, const char *buf, uint32_t len) {
    vfs_inode_t ino, parent; vfs_superblock_t *sb; char leaf[VFS_NAME_MAX]; char parent_path[VFS_PATH_MAX];
    char abs[VFS_PATH_MAX];
    normalize_path(path, abs);
    if (vfs_resolve(abs, &ino, &sb, 0, 0) == 0) {
        if (vfs_permission_check(&ino, 2, process_current_task()) < 0) return -1;
        if ((ino.mode & 0xF000) == VFS_INODE_CHR) {
            int idx = vfs_devnode_from_inode(&ino);
            if (idx >= 0) return vfs_dev_write_chr(&g_devnodes[idx], buf, len);
            for (uint32_t i = 0; i < len; ++i) console_putchar(buf[i]);
            return (int)len;
        }
        if ((ino.mode & 0xF000) == VFS_INODE_BLK) {
            block_device_t *b = (block_device_t *)vfs_inode_get_ptr(&ino);
            static uint8_t secbuf[65536];
            if (!b || !b->ops.write_sectors || b->sector_size == 0) return -1;
            if (len == 0) return 0;
            uint32_t sectors = (len + b->sector_size - 1) / b->sector_size;
            if (sectors * b->sector_size > sizeof(secbuf)) sectors = sizeof(secbuf) / b->sector_size;
            if (sectors == 0) sectors = 1;
            memset(secbuf, 0, sectors * b->sector_size);
            memcpy(secbuf, buf, len < sectors * b->sector_size ? len : sectors * b->sector_size);
            return block_write_sectors(b, 0, sectors, secbuf) < 0 ? -1 : (int)len;
        }
        if (!sb->ops->write) return -1;
        return sb->ops->write(sb, &ino, 0, buf, len);
    }
    if (path_split_last(abs, parent_path, leaf) < 0) return -1;
    if (vfs_resolve(parent_path, &parent, &sb, 0, 0) < 0 || !sb->ops->create) return -1;
    if (vfs_permission_check(&parent, 3, process_current_task()) < 0) return -1;
    VFS_DEBUG("create path='%s' parent='%s' parent_ino=%u leaf='%s'", abs, parent_path, parent.ino, leaf);
    if (sb->ops->create(sb, &parent, leaf, VFS_INODE_FILE | (0644 & (uint16_t)~vfs_current_umask()), &ino) < 0) return -1;
    return sb->ops->write ? sb->ops->write(sb, &ino, 0, buf, len) : -1;
}

int vfs_mkdir(const char *path) {
    vfs_inode_t parent, out; vfs_superblock_t *sb; char leaf[VFS_NAME_MAX]; char parent_path[VFS_PATH_MAX];
    char abs[VFS_PATH_MAX];
    normalize_path(path, abs);
    if (path_split_last(abs, parent_path, leaf) < 0) return -1;
    if (vfs_resolve(parent_path, &parent, &sb, 0, 0) < 0 || !sb->ops->mkdir) return -1;
    if (vfs_permission_check(&parent, 3, process_current_task()) < 0) return -1;
    VFS_DEBUG("mkdir path='%s' parent='%s' parent_ino=%u leaf='%s'", abs, parent_path, parent.ino, leaf);
    return sb->ops->mkdir(sb, &parent, leaf, VFS_INODE_DIR | (0755 & (uint16_t)~vfs_current_umask()), &out);
}

int vfs_touch(const char *path) { return vfs_write_file(path, "", 0) >= 0 ? 0 : -1; }
int vfs_unlink(const char *path) {
    vfs_inode_t parent, victim; vfs_superblock_t *sb; char leaf[VFS_NAME_MAX]; char parent_path[VFS_PATH_MAX];
    char abs[VFS_PATH_MAX];
    normalize_path(path, abs);
    if (path_split_last(abs, parent_path, leaf) < 0) return -1;
    if (vfs_resolve(parent_path, &parent, &sb, 0, 0) < 0 || !sb->ops->unlink) return -1;
    if (vfs_permission_check(&parent, 3, process_current_task()) < 0) return -1;
    if (sb->ops->lookup && sb->ops->lookup(sb, &parent, leaf, &victim) == 0) {
        const task_t *t = process_current_task();
        if ((parent.mode & 01000) && t && t->euid != 0 && t->euid != parent.uid && t->euid != victim.uid) return -1;
    }
    return sb->ops->unlink(sb, &parent, leaf);
}

void vfs_list(const char *path, int longf) {
    vfs_inode_t dir, ino; vfs_superblock_t *sb;
    char abs[VFS_PATH_MAX];
    int is_dev_dir;
    int is_dev_input_dir;
    normalize_path(path ? path : g_cwd, abs);
    is_dev_dir = (strcmp(abs, "/dev") == 0);
    is_dev_input_dir = (strcmp(abs, "/dev/input") == 0);
    if (vfs_resolve(abs, &dir, &sb, 0, 0) < 0) return;
    char name[VFS_NAME_MAX];
    if (sb && sb->ops && sb->ops->readdir) {
        for (uint32_t i = 0;; ++i) {
            name[0] = 0;
            name[VFS_NAME_MAX - 1] = 0;
            if (sb->ops->readdir(sb, &dir, i, name, &ino) < 0) break;
            name[VFS_NAME_MAX - 1] = 0;
            if (longf) {
                char t = '-';
                uint16_t mt = (ino.mode & 0xF000);
                if (mt == VFS_INODE_DIR) t = 'd';
                else if (mt == VFS_INODE_BLK) t = 'b';
                else if (mt == VFS_INODE_CHR) t = 'c';
                printf("%c%03o %5u %8u %s\n", t, ino.mode & 0777, ino.ino, ino.size, name);
            }
            else printf("%s  ", name);
        }
    }
    if (is_dev_dir) {
        if (longf) printf("d%03o %5u %8u input\n", 0755, 0, 0);
        else printf("input  ");
        for (int i = 0; i < g_devnode_count; ++i) {
            if (longf) {
                char t = ((g_devnodes[i].mode & 0xF000) == VFS_INODE_BLK) ? 'b' : 'c';
                printf("%c%03o %5u %8u %s\n", t, g_devnodes[i].mode & 0777, 0, 0, g_devnodes[i].name);
            } else {
                printf("%s  ", g_devnodes[i].name);
            }
        }
    }
    if (is_dev_input_dir) {
        if (longf) {
            printf("c%03o %5u %8u mice\n", 0666, 0, 0);
            printf("c%03o %5u %8u mouse0\n", 0666, 0, 0);
            printf("c%03o %5u %8u event0\n", 0666, 0, 0);
        } else {
            printf("mice  mouse0  event0  ");
        }
    }
    if (!longf) printf("\n");
}

const char *vfs_getcwd(void) { return vfs_current_cwd_ptr(); }
int vfs_chdir(const char *path) {
    vfs_inode_t ino;
    char abs[VFS_PATH_MAX];
    normalize_path(path, abs);
    if (vfs_resolve(abs, &ino, 0, 0, 0) < 0 || !(ino.mode & VFS_INODE_DIR)) return -1;
    if (vfs_permission_check(&ino, 1, process_current_task()) < 0) return -1;
    strcpy(vfs_current_cwd_ptr(), abs);
    return 0;
}

int vfs_chroot(const char *path) {
    vfs_inode_t ino;
    task_t *t = process_current_task();
    char abs[VFS_PATH_MAX];
    if (!t || !path) return -1;
    normalize_path(path, abs);
    if (vfs_resolve(abs, &ino, 0, 0, 0) < 0 || !(ino.mode & VFS_INODE_DIR)) return -1;
    if (vfs_permission_check(&ino, 1, t) < 0) return -1;
    strcpy(t->root, abs);
    if (!t->cwd[0]) strcpy(t->cwd, "/");
    return 0;
}

void vfs_list_mounts(void) {
    for (int i = 0; i < g_mount_count; ++i) printf("%s on %s type %s\n", g_mounts[i].dev_name, g_mounts[i].mountpoint, g_mounts[i].fs_name);
}

int vfs_statfs_path(const char *path, uint32_t *total_kb, uint32_t *used_kb) {
    vfs_superblock_t *sb = vfs_find_mount(path && path[0] ? path : vfs_current_cwd_ptr());
    if (!sb || !sb->ops->statfs) return -1;
    return sb->ops->statfs(sb, total_kb, used_kb);
}

int vfs_has_mounts(void) {
    return g_mount_count > 0;
}

int vfs_inode_get_block_device(const vfs_inode_t *inode, block_device_t **out) {
    if (!inode || !out) return -1;
    if ((inode->mode & 0xF000) != VFS_INODE_BLK) return -1;
    *out = (block_device_t *)vfs_inode_get_ptr(inode);
    return *out ? 0 : -1;
}

int vfs_dev_ioctl(const char *path, uint32_t cmd, void *arg) {
    vfs_inode_t ino;
    int idx;
    if (!path) return -EINVAL;
    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return -EINVAL;
    if ((ino.mode & 0xF000) != VFS_INODE_CHR) return -EINVAL;
    idx = vfs_devnode_from_inode(&ino);
    if (idx < 0) return -EINVAL;

    if (g_devnodes[idx].kind == DEV_KIND_FB0) {
        if (!arg) return -EINVAL;
        if (cmd == FB_IOCTL_GET_INFO_LEGACY) {
            ((struct fb_info *)arg)->width = fb.width;
            ((struct fb_info *)arg)->height = fb.height;
            ((struct fb_info *)arg)->pitch = fb.pitch;
            ((struct fb_info *)arg)->bpp = fb.bpp;
            return 0;
        }
        if (cmd == LINUX_FBIOGET_FSCREENINFO) {
            struct edge_fb_fix_screeninfo fix;
            memset(&fix, 0, sizeof(fix));
            strcpy(fix.id, "EdgeOS fb0");
            fix.smem_start = (uint64_t)(uintptr_t)fb.addr;
            fix.smem_len = fb.pitch * fb.height;
            fix.type = 0;   /* FB_TYPE_PACKED_PIXELS */
            fix.visual = 2; /* FB_VISUAL_TRUECOLOR */
            fix.line_length = fb.pitch;
            return memcpy(arg, &fix, sizeof(fix)), 0;
        }
        if (cmd == LINUX_FBIOGET_VSCREENINFO) {
            struct edge_fb_var_screeninfo var;
            memset(&var, 0, sizeof(var));
            var.xres = fb.width;
            var.yres = fb.height;
            var.xres_virtual = fb.width;
            var.yres_virtual = fb.height;
            var.bits_per_pixel = fb.bpp;
            var.red.offset = fb.r_pos;
            var.green.offset = fb.g_pos;
            var.blue.offset = fb.b_pos;
            var.red.length = 8;
            var.green.length = 8;
            var.blue.length = 8;
            var.transp.offset = 24;
            var.transp.length = (fb.bpp == 32) ? 8u : 0u;
            var.width = 0xFFFFFFFFu;
            var.height = 0xFFFFFFFFu;
            return memcpy(arg, &var, sizeof(var)), 0;
        }
        return -ENOSYS;
    }
    return -ENOSYS;
}

int vfs_dev_mmap(const char *path, uint64_t *addr_out, uint32_t *len_out) {
    vfs_inode_t ino;
    int idx;
    if (!path || !addr_out) return -EINVAL;
    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return -EINVAL;
    if ((ino.mode & 0xF000) != VFS_INODE_CHR) return -EINVAL;
    idx = vfs_devnode_from_inode(&ino);
    if (idx < 0) return -EINVAL;
    if (g_devnodes[idx].kind != DEV_KIND_FB0) return -ENOSYS;
    if (!fb.addr || fb.pitch == 0 || fb.height == 0) return -1;
    {
        uint64_t phys_base = 0, off = 0;
        uint32_t pages = 0;
        if (fb_get_2m_phys_window(&phys_base, &pages, &off)) {
            (void)phys_base;
            if (pages > 0 && pages <= EDGE_FBDEV_USER_MAX_PAGES) {
                *addr_out = EDGE_FBDEV_USER_BASE + off;
                if (len_out) *len_out = fb.pitch * fb.height;
                return 0;
            }
        }
    }
    *addr_out = (uint64_t)(uintptr_t)fb.addr;
    if (len_out) *len_out = fb.pitch * fb.height;
    return 0;
}

int vfs_dev_pwrite(const char *path, const char *buf, uint32_t len, uint64_t off) {
    vfs_inode_t ino;
    int idx;
    if (!path || (!buf && len)) return -EINVAL;
    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return -EINVAL;
    if ((ino.mode & 0xF000) != VFS_INODE_CHR) return -EINVAL;
    idx = vfs_devnode_from_inode(&ino);
    if (idx < 0) return -EINVAL;
    if (g_devnodes[idx].kind == DEV_KIND_FB0) {
        uint32_t fb_bytes;
        uint32_t n;
        uint8_t *dst;
        if (!fb.addr || fb.pitch == 0 || fb.height == 0) return -1;
        fb_bytes = fb.pitch * fb.height;
        if (off >= fb_bytes) return 0;
        n = len;
        if (off + n < off || off + n > fb_bytes) n = (uint32_t)(fb_bytes - off);
        dst = fb_get_draw_buffer();
        if (!dst) dst = fb.addr;
        if (n) memcpy(dst + (uint32_t)off, buf, n);
        if (dst != fb.addr) fb_present();
        return (int)n;
    }
    /* Fallback to legacy device write semantics for other char devices. */
    return vfs_write_file(path, buf, len);
}

int vfs_mounts_snapshot(char *buf, uint32_t max) {
    uint32_t off = 0;
    if (!buf || max == 0) return -1;
    buf[0] = 0;
    #define APPEND_STR(_s) do { \
        const char *s_ = (_s); \
        while (*s_ && off + 1 < max) buf[off++] = *s_++; \
        if (off < max) buf[off] = 0; \
    } while (0)
    for (int i = 0; i < g_mount_count; ++i) {
        const char *dev = g_mounts[i].dev_name[0] ? g_mounts[i].dev_name : "none";
        const char *mp = g_mounts[i].mountpoint[0] ? g_mounts[i].mountpoint : "/";
        const char *fs = g_mounts[i].fs_name[0] ? g_mounts[i].fs_name : "unknown";
        APPEND_STR(dev);
        APPEND_STR(" ");
        APPEND_STR(mp);
        APPEND_STR(" ");
        APPEND_STR(fs);
        APPEND_STR(" rw 0 0\n");
        if (off + 1 >= max) break;
    }
    #undef APPEND_STR
    if (off >= max) off = max - 1;
    buf[off] = 0;
    return (int)off;
}

int vfs_permission_check(const vfs_inode_t *inode, int access_mask, const task_t *task) {
    if (!inode) return -EINVAL;
    if (!task) return 0;

    uint16_t mode = inode->mode & 07777u;
    if (task->euid == 0) {
        if (access_mask & 1) {
            if (!(mode & 0111u)) {
                if (EDGE_SECURITY_DEBUG) printf("[sec] perm denied root exec inode=%u mode=%o\n", inode->ino, mode);
                return -EACCES;
            }
        }
        return 0;
    }

    uint16_t bits;
    if (task->euid == inode->uid) bits = (uint16_t)((mode >> 6) & 7u);
    else if (task->egid == inode->gid) bits = (uint16_t)((mode >> 3) & 7u);
    else bits = (uint16_t)(mode & 7u);

    if ((access_mask & 4) && !(bits & 4u)) { if (EDGE_SECURITY_DEBUG) printf("[sec] perm denied R pid=%d ino=%u\n", task->pid, inode->ino); return -EACCES; }
    if ((access_mask & 2) && !(bits & 2u)) { if (EDGE_SECURITY_DEBUG) printf("[sec] perm denied W pid=%d ino=%u\n", task->pid, inode->ino); return -EACCES; }
    if ((access_mask & 1) && !(bits & 1u)) { if (EDGE_SECURITY_DEBUG) printf("[sec] perm denied X pid=%d ino=%u\n", task->pid, inode->ino); return -EACCES; }
    return 0;
}

int vfs_chmod(const char *path, uint16_t mode) {
    vfs_inode_t ino;
    vfs_superblock_t *sb = 0;
    if (!path) return -EINVAL;
    if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) return -1;
    if (sb && strcmp(sb->fs_name, "ext2") == 0) {
        return ext2_setattr(sb, &ino, mode, 0, 0, 1u);
    }
    if (sb && strcmp(sb->fs_name, "ext4") == 0) {
        return ext4_setattr(sb, &ino, mode, 0, 0, 1u);
    }
    return 0;
}

int vfs_chown(const char *path, uint16_t uid, uint16_t gid) {
    vfs_inode_t ino;
    vfs_superblock_t *sb = 0;
    if (!path) return -EINVAL;
    if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) return -1;
    if (sb && strcmp(sb->fs_name, "ext2") == 0) {
        return ext2_setattr(sb, &ino, 0, uid, gid, 2u | 4u);
    }
    if (sb && strcmp(sb->fs_name, "ext4") == 0) {
        return ext4_setattr(sb, &ino, 0, uid, gid, 2u | 4u);
    }
    return 0;
}
