#ifndef VFS_VFS_H
#define VFS_VFS_H

#include <stdint.h>
#include "block/block.h"
#include "sys/process.h"

#define VFS_NAME_MAX 64
#define VFS_PATH_MAX 256
#define VFS_MAX_MOUNTS 8

#define VFS_INODE_DIR  0x4000
#define VFS_INODE_FILE 0x8000
#define VFS_INODE_CHR  0x2000
#define VFS_INODE_BLK  0x6000

typedef struct vfs_inode vfs_inode_t;
typedef struct vfs_superblock vfs_superblock_t;

typedef struct {
    int (*lookup)(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, vfs_inode_t *out);
    int (*read)(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, void *buf, uint32_t len);
    int (*write)(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, const void *buf, uint32_t len);
    int (*create)(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out);
    int (*mkdir)(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out);
    int (*unlink)(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name);
    int (*readdir)(vfs_superblock_t *sb, vfs_inode_t *dir, uint32_t idx, char *name_out, vfs_inode_t *inode_out);
    int (*statfs)(vfs_superblock_t *sb, uint32_t *total_kb, uint32_t *used_kb);
} filesystem_ops_t;

struct vfs_inode {
    uint32_t ino;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t size;
    uint32_t fs_private[4];
};

struct vfs_superblock {
    char fs_name[16];
    char dev_name[16];
    char mountpoint[VFS_PATH_MAX];
    vfs_inode_t root;
    filesystem_ops_t *ops;
    void *fs_private;
};

void vfs_init(void);
int vfs_register(const char *name, int (*mount_fn)(const char *dev, const char *target));
int vfs_mount(const char *dev, const char *target, const char *fsname);
int vfs_mount_blockdev(block_device_t *dev, const char *target, const char *fsname);
int vfs_add_superblock(vfs_superblock_t *sb);
int vfs_resolve(const char *path, vfs_inode_t *out_inode, vfs_superblock_t **out_sb, vfs_inode_t *out_parent, char *leaf);
int vfs_read_file(const char *path, char *out, uint32_t max);
int vfs_write_file(const char *path, const char *buf, uint32_t len);
int vfs_mkdir(const char *path);
int vfs_touch(const char *path);
int vfs_unlink(const char *path);
void vfs_list(const char *path, int longf);
const char *vfs_getcwd(void);
int vfs_chdir(const char *path);
int vfs_chroot(const char *path);
void vfs_list_mounts(void);
int vfs_statfs_path(const char *path, uint32_t *total_kb, uint32_t *used_kb);
int vfs_has_mounts(void);
int vfs_inode_get_block_device(const vfs_inode_t *inode, block_device_t **out);
int vfs_dev_ioctl(const char *path, uint32_t cmd, void *arg);
int vfs_dev_mmap(const char *path, uint64_t *addr_out, uint32_t *len_out);
int vfs_dev_pwrite(const char *path, const char *buf, uint32_t len, uint64_t off);
int vfs_mounts_snapshot(char *buf, uint32_t max);
int vfs_permission_check(const vfs_inode_t *inode, int access_mask, const task_t *task);
int vfs_chmod(const char *path, uint16_t mode);
int vfs_chown(const char *path, uint16_t uid, uint16_t gid);

#endif
