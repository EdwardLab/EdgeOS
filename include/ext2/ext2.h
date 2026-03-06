#ifndef EXT2_EXT2_H
#define EXT2_EXT2_H

#include "block/block.h"
#include "vfs/vfs.h"

int ext2_mount(const char *dev, const char *target);
int ext2_mount_block(block_device_t *bdev, const char *target);
int ext2_mount_as(const char *dev, const char *target, const char *fs_name, int ext4_mode);
int ext2_mount_block_as(block_device_t *bdev, const char *target, const char *fs_name, int ext4_mode);
int ext2_setattr(vfs_superblock_t *sb, const vfs_inode_t *inode, uint16_t mode, uint16_t uid, uint16_t gid, uint32_t mask);

#endif
