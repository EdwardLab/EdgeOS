#ifndef EXT4_EXT4_H
#define EXT4_EXT4_H

#include "block/block.h"
#include "vfs/vfs.h"

int ext4_mount(const char *dev, const char *target);
int ext4_mount_block(block_device_t *bdev, const char *target);
int ext4_setattr(vfs_superblock_t *sb, const vfs_inode_t *inode, uint16_t mode, uint16_t uid, uint16_t gid, uint32_t mask);

#endif
