#include "ext2/ext2.h"
#include "vfs/vfs.h"
#include "block/block.h"
#include "string.h"
#include "stdio.h"
#include "sys/process.h"

#pragma pack(push,1)
typedef struct {
    uint32_t inodes_count, blocks_count, r_blocks_count, free_blocks_count;
    uint32_t free_inodes_count, first_data_block, log_block_size, log_frag_size;
    uint32_t blocks_per_group, frags_per_group, inodes_per_group;
    uint32_t mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev_level;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
} ext2_super_t;

typedef struct {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks_count, free_inodes_count, used_dirs_count;
    uint16_t pad;
    uint8_t reserved[12];
} ext2_bgdesc_t;

typedef struct {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links_count;
    uint32_t blocks, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, dir_acl, faddr;
    uint8_t osd2[12];
} ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    uint8_t name[];
} ext2_dirent_t;
#pragma pack(pop)

typedef struct {
    block_device_t *bdev;
    ext2_super_t sb;
    ext2_bgdesc_t bg;
    uint32_t block_size;
} ext2_fs_t;

static ext2_fs_t g_ext2_mounts[8];
static int g_ext2_mount_count;
static uint8_t g_io[4096];
static uint8_t g_io2[4096];

#define EXT2_DEBUG(fmt, ...) printf("[ext2] " fmt "\n", ##__VA_ARGS__)

#define EXT4_INCOMPAT_FILETYPE      0x0002u
#define EXT4_INCOMPAT_EXTENTS       0x0040u
#define EXT4_INCOMPAT_64BIT         0x0080u
#define EXT4_INCOMPAT_MMP           0x0100u
#define EXT4_INCOMPAT_FLEX_BG       0x0200u
#define EXT4_INCOMPAT_EA_INODE      0x0400u
#define EXT4_INCOMPAT_DIRDATA       0x1000u
#define EXT4_INCOMPAT_CSUM_SEED     0x2000u
#define EXT4_INCOMPAT_LARGEDIR      0x4000u
#define EXT4_INCOMPAT_INLINE_DATA   0x8000u
#define EXT4_INCOMPAT_ENCRYPT       0x10000u
#define EXT4_INCOMPAT_CASEFOLD      0x20000u

#define EXT4_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT4_RO_COMPAT_LARGE_FILE   0x0002u
#define EXT4_RO_COMPAT_BTREE_DIR    0x0004u
#define EXT4_RO_COMPAT_HUGE_FILE    0x0008u
#define EXT4_RO_COMPAT_GDT_CSUM     0x0010u
#define EXT4_RO_COMPAT_DIR_NLINK    0x0020u
#define EXT4_RO_COMPAT_EXTRA_ISIZE  0x0040u
#define EXT4_RO_COMPAT_QUOTA        0x0100u
#define EXT4_RO_COMPAT_BIGALLOC     0x0200u
#define EXT4_RO_COMPAT_METADATA_CSUM 0x0400u
#define EXT4_RO_COMPAT_REPLICA      0x0800u
#define EXT4_RO_COMPAT_READONLY     0x1000u
#define EXT4_RO_COMPAT_PROJECT      0x2000u
#define EXT4_RO_COMPAT_VERITY       0x8000u
#define EXT4_RO_COMPAT_ORPHAN_PRESENT 0x10000u

static int ext4_feature_set_supported(const ext2_super_t *sb) {
    uint32_t unsupported_incompat;
    uint32_t unsupported_ro;
    if (!sb) return 0;

    unsupported_incompat = sb->feature_incompat & ~EXT4_INCOMPAT_FILETYPE;
    if (unsupported_incompat) {
        EXT2_DEBUG("ext4 unsupported incompat=0x%x", unsupported_incompat);
        return 0;
    }

    unsupported_ro = sb->feature_ro_compat &
        ~(EXT4_RO_COMPAT_SPARSE_SUPER | EXT4_RO_COMPAT_LARGE_FILE | EXT4_RO_COMPAT_BTREE_DIR);
    if (unsupported_ro) {
        EXT2_DEBUG("ext4 unsupported ro_compat=0x%x", unsupported_ro);
        return 0;
    }
    return 1;
}

static uint16_t rec_len_min(uint8_t name_len) { return (uint16_t)((8 + name_len + 3) & ~3); }

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
} ext2_dirent_hdr_t;

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int ext2_dirent_read(const uint8_t *blk, uint32_t block_size, uint32_t off, ext2_dirent_hdr_t *out) {
    if (!blk || !out || off + 8 > block_size) return -1;
    out->inode = rd32le(blk + off + 0);
    out->rec_len = rd16le(blk + off + 4);
    out->name_len = blk[off + 6];
    out->file_type = blk[off + 7];
    if (out->rec_len < 8 || off + out->rec_len > block_size) return -1;
    return 0;
}

static uint16_t vfs_mode_from_ext2(uint16_t ext2_mode) {
    uint16_t kind = ext2_mode & 0xF000;
    uint16_t perms = ext2_mode & 07777;
    if (kind == 0x4000) return (uint16_t)(VFS_INODE_DIR | perms);
    if (kind == 0x8000) return (uint16_t)(VFS_INODE_FILE | perms);
    return 0;
}

static int read_block(ext2_fs_t *fs, uint32_t block, void *out) {
    if (!fs || !fs->bdev || fs->bdev->sector_size == 0) return -1;
    uint32_t cnt = fs->block_size / fs->bdev->sector_size;
    if (cnt == 0) return -1;
    return block_read_sectors(fs->bdev, block * cnt, cnt, out);
}

static int write_block(ext2_fs_t *fs, uint32_t block, const void *in) {
    if (!fs || !fs->bdev || fs->bdev->sector_size == 0) return -1;
    uint32_t cnt = fs->block_size / fs->bdev->sector_size;
    if (cnt == 0) return -1;
    return block_write_sectors(fs->bdev, block * cnt, cnt, in);
}

static int sync_super_bg(ext2_fs_t *fs) {
    if (!fs || !fs->bdev) return -1;

    if (block_read_sectors(fs->bdev, 2, 2, g_io) < 0) return -1;
    memcpy(g_io, &fs->sb, sizeof(fs->sb));
    if (block_write_sectors(fs->bdev, 2, 2, g_io) < 0) return -1;

    if (read_block(fs, fs->sb.first_data_block + 1, g_io) < 0) return -1;
    memcpy(g_io, &fs->bg, sizeof(fs->bg));
    return write_block(fs, fs->sb.first_data_block + 1, g_io);
}

static int read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out) {
    if (!fs || !out || ino == 0 || ino > fs->sb.inodes_count) return -1;
    uint32_t idx = ino - 1;
    uint32_t off = idx * fs->sb.inode_size;
    uint32_t blk = fs->bg.inode_table + off / fs->block_size;
    uint32_t boff = off % fs->block_size;
    if (boff + sizeof(ext2_inode_t) > fs->block_size) return -1;
    if (read_block(fs, blk, g_io) < 0) return -1;
    memcpy(out, g_io + boff, sizeof(ext2_inode_t));
    return 0;
}

static int write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *in) {
    if (!fs || !in || ino == 0 || ino > fs->sb.inodes_count) return -1;
    uint32_t idx = ino - 1;
    uint32_t off = idx * fs->sb.inode_size;
    uint32_t blk = fs->bg.inode_table + off / fs->block_size;
    uint32_t boff = off % fs->block_size;
    if (boff + sizeof(ext2_inode_t) > fs->block_size) return -1;
    if (read_block(fs, blk, g_io) < 0) return -1;
    memcpy(g_io + boff, in, sizeof(ext2_inode_t));
    return write_block(fs, blk, g_io);
}

static int ext2_alloc_from_bitmap(ext2_fs_t *fs, uint32_t bitmap_block, uint32_t max, int is_inode) {
    if (!fs) return -1;
    if (read_block(fs, bitmap_block, g_io) < 0) return -1;
    for (uint32_t i = 0; i < max; ++i) {
        uint32_t byte = i / 8, bit = i % 8;
        if (!(g_io[byte] & (1u << bit))) {
            g_io[byte] |= (1u << bit);
            if (write_block(fs, bitmap_block, g_io) < 0) return -1;
            if (is_inode) {
                if (fs->sb.free_inodes_count) fs->sb.free_inodes_count--;
                if (fs->bg.free_inodes_count) fs->bg.free_inodes_count--;
            } else {
                if (fs->sb.free_blocks_count) fs->sb.free_blocks_count--;
                if (fs->bg.free_blocks_count) fs->bg.free_blocks_count--;
            }
            if (sync_super_bg(fs) < 0) return -1;
            return (int)(i + (is_inode ? 1 : 0));
        }
    }
    return -1;
}

static int ext2_lookup(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, vfs_inode_t *out) {
    ext2_fs_t *fs = (ext2_fs_t *)sb->fs_private;
    ext2_inode_t dino;
    if (!fs || !dir || !name || !out) return -1;
    if (read_inode(fs, dir->ino, &dino) < 0) return -1;

    for (int bi = 0; bi < 12; ++bi) {
        if (!dino.block[bi]) continue;
        if (read_block(fs, dino.block[bi], g_io) < 0) continue;

        uint32_t off = 0;
        while (off + 8 <= fs->block_size) {
            ext2_dirent_hdr_t de;
            if (ext2_dirent_read(g_io, fs->block_size, off, &de) < 0) break;

            if (de.inode && de.name_len > 0 && de.name_len < VFS_NAME_MAX && rec_len_min(de.name_len) <= de.rec_len) {
                char dn[VFS_NAME_MAX];
                memcpy(dn, g_io + off + 8, de.name_len);
                dn[de.name_len] = 0;

                if (strcmp(dn, (char *)name) == 0) {
                    uint32_t found_ino = de.inode;
                    
                    ext2_inode_t t;
                    if (read_inode(fs, found_ino, &t) < 0) return -1; 
                    
                    uint16_t mode = vfs_mode_from_ext2(t.mode);
                    if (!mode) return -1;
                    
                    out->ino = found_ino;
                    out->size = t.size;
                    out->mode = mode;
                    out->uid = t.uid;
                    out->gid = t.gid;
                    return 0;
                }
            }
            off += de.rec_len;
        }
    }
    return -1;
}

static int ext2_readdir(vfs_superblock_t *sb, vfs_inode_t *dir, uint32_t idx, char *name_out, vfs_inode_t *inode_out) {
    ext2_fs_t *fs = (ext2_fs_t *)sb->fs_private;
    ext2_inode_t dino;
    uint32_t seen = 0;
    if (!fs || !name_out || !inode_out || !dir) return -1;
    if (read_inode(fs, dir->ino, &dino) < 0) return -1;

    for (int bi = 0; bi < 12; ++bi) {
        if (!dino.block[bi]) continue;
        if (read_block(fs, dino.block[bi], g_io) < 0) continue;

        uint32_t off = 0;
        while (off + 8 <= fs->block_size) {
            ext2_dirent_hdr_t de;
            if (ext2_dirent_read(g_io, fs->block_size, off, &de) < 0) break;

            if (de.inode && de.name_len > 0 && de.name_len < VFS_NAME_MAX && rec_len_min(de.name_len) <= de.rec_len) {
                if (seen == idx) {
                    memcpy(name_out, g_io + off + 8, de.name_len);
                    name_out[de.name_len] = 0;

                    uint32_t found_ino = de.inode;

                    ext2_inode_t t;
                    if (read_inode(fs, found_ino, &t) < 0) return -1;

                    uint16_t mode = vfs_mode_from_ext2(t.mode);
                    if (!mode) return -1;

                    inode_out->ino = found_ino;
                    inode_out->mode = mode;
                    inode_out->size = t.size;
                    inode_out->uid = t.uid;
                    inode_out->gid = t.gid;
                    return 0;
                }
                seen++;
            }
            off += de.rec_len;
        }
    }
    return -1;
}

static int ext2_read(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, void *buf, uint32_t len) {
    ext2_fs_t *fs = (ext2_fs_t *)sb->fs_private;
    ext2_inode_t in;
    if (!fs || !inode || !buf) return -1;
    if (read_inode(fs, inode->ino, &in) < 0) return -1;
    if (off >= in.size) return 0;
    if (off + len > in.size) len = in.size - off;

    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = off + done;
        uint32_t bi = pos / fs->block_size;
        uint32_t bo = pos % fs->block_size;
        uint32_t data_block = 0;

        if (bi < 12) {
            data_block = in.block[bi];
        } else {
            uint32_t ind_idx = bi - 12;
            uint32_t ents = fs->block_size / sizeof(uint32_t);
            if (ind_idx < ents) {
                if (!in.block[12]) break;
                if (read_block(fs, in.block[12], g_io) < 0) break;
                data_block = ((uint32_t *)g_io)[ind_idx];
            } else {
                uint32_t dind_idx = ind_idx - ents;
                uint32_t outer = dind_idx / ents;
                uint32_t inner = dind_idx % ents;
                uint32_t ind_block;
                if (!in.block[13] || outer >= ents) break;
                if (read_block(fs, in.block[13], g_io) < 0) break;
                ind_block = ((uint32_t *)g_io)[outer];
                if (!ind_block) break;
                if (read_block(fs, ind_block, g_io2) < 0) break;
                data_block = ((uint32_t *)g_io2)[inner];
            }
        }

        if (!data_block) break;
        if (read_block(fs, data_block, g_io) < 0) break;
        uint32_t n = fs->block_size - bo;
        if (n > len - done) n = len - done;
        memcpy(out + done, g_io + bo, n);
        done += n;
    }
    return (int)done;
}

static int ext2_write(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, const void *buf, uint32_t len) {
    ext2_fs_t *fs = (ext2_fs_t *)sb->fs_private;
    ext2_inode_t in;
    if (!fs || !inode || !buf) return -1;
    if (read_inode(fs, inode->ino, &in) < 0) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t pos = off + done;
        uint32_t bi = pos / fs->block_size;
        uint32_t bo = pos % fs->block_size;
        if (bi >= 12) break;

        if (!in.block[bi]) {
            int nb = ext2_alloc_from_bitmap(fs, fs->bg.block_bitmap, fs->sb.blocks_per_group, 0);
            if (nb < 0) break;
            in.block[bi] = (uint32_t)nb;
            memset(g_io, 0, fs->block_size);
            if (write_block(fs, in.block[bi], g_io) < 0) break;
        }

        if (read_block(fs, in.block[bi], g_io) < 0) break;

        uint32_t n = fs->block_size - bo;
        if (n > len - done) n = len - done;

        memcpy(g_io + bo, src + done, n);
        if (write_block(fs, in.block[bi], g_io) < 0) break;

        done += n;
    }

    if (off + done > in.size) in.size = off + done;
    in.blocks = (in.size + 511) / 512;
    if (write_inode(fs, inode->ino, &in) < 0) return -1;
    inode->size = in.size;
    return (int)done;
}

static int ext2_insert_dirent(ext2_fs_t *fs, ext2_inode_t *dino, uint32_t dir_ino, uint32_t child_ino, const char *name, int is_dir) {
    if (!fs || !dino || !name || !name[0]) return -1;

    uint16_t nlen = (uint16_t)strlen(name);
    if (nlen == 0 || nlen >= VFS_NAME_MAX) return -1;
    uint16_t need = rec_len_min((uint8_t)nlen);

    for (int bi = 0; bi < 12; ++bi) {
        if (!dino->block[bi]) continue;
        if (read_block(fs, dino->block[bi], g_io) < 0) continue;

        uint32_t off = 0;
        while (off + 8 <= fs->block_size) {
            ext2_dirent_hdr_t de;
            if (ext2_dirent_read(g_io, fs->block_size, off, &de) < 0) break;

            if (de.inode == 0 && de.rec_len >= need) {
                uint16_t slot = de.rec_len;
                memset(g_io + off, 0, slot);
                wr32le(g_io + off + 0, child_ino);
                wr16le(g_io + off + 4, slot);
                g_io[off + 6] = (uint8_t)nlen;
                g_io[off + 7] = is_dir ? 2 : 1;
                memcpy(g_io + off + 8, name, nlen);

                if (write_block(fs, dino->block[bi], g_io) < 0) return -1;
                if (write_inode(fs, dir_ino, dino) < 0) return -1;
                return 0;
            }

            off += de.rec_len;
        }
    }

    for (int bi = 0; bi < 12; ++bi) {
        if (dino->block[bi]) continue;

        int nb = ext2_alloc_from_bitmap(fs, fs->bg.block_bitmap, fs->sb.blocks_per_group, 0);
        if (nb < 0) return -1;

        dino->block[bi] = (uint32_t)nb;
        dino->size += fs->block_size;
        dino->blocks = (dino->size + 511) / 512;

        memset(g_io, 0, fs->block_size);

        wr32le(g_io + 0, child_ino);
        wr16le(g_io + 4, (uint16_t)fs->block_size);
        g_io[6] = (uint8_t)nlen;
        g_io[7] = is_dir ? 2 : 1;
        memcpy(g_io + 8, name, nlen);

        if (write_block(fs, dino->block[bi], g_io) < 0) return -1;
        if (write_inode(fs, dir_ino, dino) < 0) return -1;
        return 0;
    }

    return -1;
}

static int ext2_create_like(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out, int is_dir) {
    ext2_fs_t *fs = (ext2_fs_t *)sb->fs_private;
    ext2_inode_t dino;
    if (!fs || !dir || !name || !out || !name[0] || strlen(name) >= VFS_NAME_MAX) return -1;
    if (read_inode(fs, dir->ino, &dino) < 0) return -1;

    vfs_inode_t exists;
    if (ext2_lookup(sb, dir, name, &exists) == 0) return -1;

    int nino = ext2_alloc_from_bitmap(fs, fs->bg.inode_bitmap, fs->sb.inodes_per_group, 1);
    if (nino < 0) return -1;

    ext2_inode_t ni;
    memset(&ni, 0, sizeof(ni));

    uint16_t ext_kind = is_dir ? 0x4000 : 0x8000;
    uint16_t perms = mode & 0777;
    ni.mode = (uint16_t)(ext_kind | perms);
    ni.uid = (uint16_t)process_geteuid();
    ni.gid = (uint16_t)process_getegid();
    ni.links_count = is_dir ? 2 : 1;

    if (is_dir) {
        int nb = ext2_alloc_from_bitmap(fs, fs->bg.block_bitmap, fs->sb.blocks_per_group, 0);
        if (nb < 0) return -1;

        ni.block[0] = (uint32_t)nb;
        ni.size = fs->block_size;
        ni.blocks = fs->block_size / 512;

        memset(g_io, 0, fs->block_size);

        uint16_t dot_rec_len = rec_len_min(1);
        wr32le(g_io + 0, (uint32_t)nino);
        wr16le(g_io + 4, dot_rec_len);
        g_io[6] = 1;
        g_io[7] = 2;
        g_io[8] = '.';

        wr32le(g_io + dot_rec_len + 0, dir->ino);
        wr16le(g_io + dot_rec_len + 4, (uint16_t)(fs->block_size - dot_rec_len));
        g_io[dot_rec_len + 6] = 2;
        g_io[dot_rec_len + 7] = 2;
        g_io[dot_rec_len + 8] = '.';
        g_io[dot_rec_len + 9] = '.';

        if (write_block(fs, ni.block[0], g_io) < 0) return -1;
        dino.links_count++;
    }

    if (write_inode(fs, (uint32_t)nino, &ni) < 0) return -1;

    if (ext2_insert_dirent(fs, &dino, dir->ino, (uint32_t)nino, name, is_dir) < 0) return -1;

    if (write_inode(fs, dir->ino, &dino) < 0) return -1;

    out->ino = (uint32_t)nino;
    out->mode = mode;
    out->size = ni.size;
    out->uid = ni.uid;
    out->gid = ni.gid;
    return 0;
}

static int ext2_create(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out) {
    return ext2_create_like(sb, dir, name, mode, out, 0);
}

static int ext2_mkdir(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out) {
    return ext2_create_like(sb, dir, name, mode, out, 1);
}

static int ext2_unlink(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name) {
    ext2_fs_t *fs = (ext2_fs_t *)sb->fs_private;
    ext2_inode_t dino;
    if (!fs || !dir || !name) return -1;
    if (read_inode(fs, dir->ino, &dino) < 0) return -1;

    for (int bi = 0; bi < 12; ++bi) {
        if (!dino.block[bi]) continue;
        if (read_block(fs, dino.block[bi], g_io) < 0) continue;

        uint32_t off = 0;
        while (off + 8 <= fs->block_size) {
            ext2_dirent_hdr_t de;
            if (ext2_dirent_read(g_io, fs->block_size, off, &de) < 0) break;

            if (de.inode && de.name_len > 0 && de.name_len < VFS_NAME_MAX) {
                char dn[VFS_NAME_MAX];
                memcpy(dn, g_io + off + 8, de.name_len);
                dn[de.name_len] = 0;

                if (strcmp(dn, (char *)name) == 0) {
                    wr32le(g_io + off + 0, 0);
                    if (write_block(fs, dino.block[bi], g_io) < 0) return -1;
                    return 0;
                }
            }

            off += de.rec_len;
        }
    }
    return -1;
}

static int ext2_statfs(vfs_superblock_t *sb, uint32_t *total_kb, uint32_t *used_kb) {
    ext2_fs_t *fs = (ext2_fs_t *)sb->fs_private;
    if (!fs || !total_kb || !used_kb) return -1;
    uint64_t total_bytes = (uint64_t)fs->sb.blocks_count * (uint64_t)fs->block_size;
    uint64_t free_bytes = (uint64_t)fs->sb.free_blocks_count * (uint64_t)fs->block_size;
    uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    *total_kb = (uint32_t)(total_bytes / 1024ull);
    *used_kb = (uint32_t)(used_bytes / 1024ull);
    return 0;
}

static filesystem_ops_t g_ext2_ops = {
    ext2_lookup, ext2_read, ext2_write, ext2_create, ext2_mkdir, ext2_unlink, ext2_readdir, ext2_statfs
};

static int ext2_mount_common(block_device_t *b, const char *dev, const char *target, const char *fs_name, int ext4_mode) {
    ext2_fs_t *fs;
    if (!b || !target) return -1;
    if (g_ext2_mount_count >= (int)(sizeof(g_ext2_mounts) / sizeof(g_ext2_mounts[0]))) return -1;
    fs = &g_ext2_mounts[g_ext2_mount_count++];
    memset(fs, 0, sizeof(*fs));
    fs->bdev = b;

    if (block_read_sectors(b, 2, 2, g_io) < 0) {
        printf("[ext2] failed reading superblock from %s\n", dev);
        return -1;
    }

    memcpy(&fs->sb, g_io, sizeof(ext2_super_t));
    if (fs->sb.magic != 0xEF53) {
        printf("[ext2] invalid superblock magic on %s\n", dev);
        return -1;
    }
    if (ext4_mode && !ext4_feature_set_supported(&fs->sb)) {
        printf("[ext4] unsupported features on %s\n", dev);
        return -1;
    }

    fs->block_size = 1024u << fs->sb.log_block_size;
    if (fs->block_size == 0 || fs->block_size > sizeof(g_io)) return -1;

    if (read_block(fs, fs->sb.first_data_block + 1, g_io) < 0) return -1;
    memcpy(&fs->bg, g_io, sizeof(ext2_bgdesc_t));

    ext2_inode_t root;
    if (read_inode(fs, 2, &root) < 0) {
        printf("[ext2] failed to read root inode\n");
        return -1;
    }

    uint16_t root_mode = vfs_mode_from_ext2(root.mode);
    if (!root_mode || (root_mode & 0xF000u) != VFS_INODE_DIR) return -1;

    vfs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    if (!fs_name || !fs_name[0]) fs_name = "ext2";
    strcpy(sb.fs_name, fs_name);
    if (dev[0] == '/') strcpy(sb.dev_name, dev); else { strcpy(sb.dev_name, "/dev/"); strcat(sb.dev_name, dev); }
    strcpy(sb.mountpoint, target);

    sb.root.ino = 2;
    sb.root.mode = root_mode;
    sb.root.size = root.size;
    sb.root.uid = root.uid;
    sb.root.gid = root.gid;

    sb.ops = &g_ext2_ops;
    sb.fs_private = fs;

    if (strcmp(target, "/") == 0) {
        vfs_inode_t sbin, init;
        if (ext2_lookup(&sb, &sb.root, "sbin", &sbin) < 0 ||
            (sbin.mode & 0xF000u) != VFS_INODE_DIR ||
            ext2_lookup(&sb, &sbin, "init", &init) < 0 ||
            (init.mode & 0xF000u) != VFS_INODE_FILE) {
            printf("[ext2] rejecting %s as root: /sbin/init missing\n", sb.dev_name);
            return -1;
        }
    }

    if (vfs_add_superblock(&sb) < 0) return -1;

    printf("[%s] mounted %s on %s\n", fs_name, sb.dev_name, target);
    return 0;
}

int ext2_mount(const char *dev, const char *target) {
    if (!dev || !target) return -1;
    block_device_t *b = block_find(dev[0] == '/' ? dev + 5 : dev);
    if (!b) {
        printf("[ext2] block device not found: %s\n", dev);
        return -1;
    }
    return ext2_mount_common(b, dev, target, "ext2", 0);
}

int ext2_mount_block(block_device_t *bdev, const char *target) {
    if (!bdev || !target) return -1;
    return ext2_mount_common(bdev, bdev->name, target, "ext2", 0);
}

int ext2_mount_as(const char *dev, const char *target, const char *fs_name, int ext4_mode) {
    if (!dev || !target) return -1;
    block_device_t *b = block_find(dev[0] == '/' ? dev + 5 : dev);
    if (!b) return -1;
    return ext2_mount_common(b, dev, target, fs_name, ext4_mode);
}

int ext2_mount_block_as(block_device_t *bdev, const char *target, const char *fs_name, int ext4_mode) {
    if (!bdev || !target) return -1;
    return ext2_mount_common(bdev, bdev->name, target, fs_name, ext4_mode);
}

int ext2_setattr(vfs_superblock_t *sb, const vfs_inode_t *inode, uint16_t mode, uint16_t uid, uint16_t gid, uint32_t mask) {
    ext2_fs_t *fs;
    ext2_inode_t in;
    if (!sb || !inode) return -1;
    fs = (ext2_fs_t *)sb->fs_private;
    if (!fs) return -1;
    if (read_inode(fs, inode->ino, &in) < 0) return -1;
    if (mask & 1u) {
        uint16_t kind = in.mode & 0xF000u;
        in.mode = (uint16_t)(kind | (mode & 07777u));
    }
    if (mask & 2u) in.uid = uid;
    if (mask & 4u) in.gid = gid;
    return write_inode(fs, inode->ino, &in);
}
