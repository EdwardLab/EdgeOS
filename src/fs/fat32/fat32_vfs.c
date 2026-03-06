#include "vfs/vfs.h"
#include "fat32.h"
#include "string.h"

static int f_lookup(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, vfs_inode_t *out) {
    (void)sb;
    fat32_node_t *base = dir->ino ? (fat32_node_t *)(uintptr_t)dir->ino : fat32_get_root();
    fat32_node_t *n = fat32_open(name, base);
    if (!n) return -1;
    out->ino = (uint32_t)(uintptr_t)n;
    out->mode = (n->type == FAT32_TYPE_DIR ? VFS_INODE_DIR : VFS_INODE_FILE) | 0777;
    out->size = n->size;
    return 0;
}

static int f_read(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, void *buf, uint32_t len) {
    (void)sb; (void)off;
    fat32_node_t *n = (fat32_node_t *)(uintptr_t)inode->ino;
    return fat32_read(n, buf, (int)len);
}

static int f_write(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, const void *buf, uint32_t len) {
    (void)sb; (void)off;
    fat32_node_t *n = (fat32_node_t *)(uintptr_t)inode->ino;
    if (!n || n->type != FAT32_TYPE_FILE) return -1;
    if (len > FAT32_CONTENT_MAX) len = FAT32_CONTENT_MAX;
    memcpy(n->content, buf, len);
    n->content[len] = 0;
    n->size = len;
    return len;
}

static int f_create(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out) {
    (void)sb; (void)mode;
    fat32_node_t *d = (fat32_node_t *)(uintptr_t)dir->ino;
    if (fat32_touch(name, d) < 0) return -1;
    fat32_node_t *n = fat32_open(name, d);
    out->ino = (uint32_t)(uintptr_t)n; out->mode = VFS_INODE_FILE | 0777; out->size = 0;
    return 0;
}

static int f_mkdir(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out) {
    (void)sb; (void)mode;
    fat32_node_t *d = (fat32_node_t *)(uintptr_t)dir->ino;
    if (fat32_mkdir(name, d) < 0) return -1;
    fat32_node_t *n = fat32_open(name, d);
    out->ino = (uint32_t)(uintptr_t)n; out->mode = VFS_INODE_DIR | 0777; out->size = 0;
    return 0;
}

static int f_unlink(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name) { (void)sb; return fat32_rm(name, (fat32_node_t *)(uintptr_t)dir->ino); }
static int f_readdir(vfs_superblock_t *sb, vfs_inode_t *dir, uint32_t idx, char *name_out, vfs_inode_t *inode_out) { (void)sb; (void)dir; (void)idx; (void)name_out; (void)inode_out; return -1; }
static int f_statfs(vfs_superblock_t *sb, uint32_t *total_kb, uint32_t *used_kb) { (void)sb; *total_kb = 1024; *used_kb = 128; return 0; }

static filesystem_ops_t g_ops = {f_lookup, f_read, f_write, f_create, f_mkdir, f_unlink, f_readdir, f_statfs};

int fat32_mount(const char *dev, const char *target) {
    (void)dev;
    fat32_init();
    vfs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    strcpy(sb.fs_name, "fat32");
    strcpy(sb.dev_name, "memfs");
    strcpy(sb.mountpoint, target);
    sb.root.ino = (uint32_t)(uintptr_t)fat32_get_root();
    sb.root.mode = VFS_INODE_DIR | 0777;
    sb.ops = &g_ops;
    return vfs_add_superblock(&sb);
}
