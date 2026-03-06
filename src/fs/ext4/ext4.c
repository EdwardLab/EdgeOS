#include "ext4/ext4.h"

#include "block/block.h"
#include "stdio.h"
#include "string.h"
#include "sys/process.h"
#include "vfs/vfs.h"

#pragma pack(push,1)
typedef struct {
    uint32_t inodes_count, blocks_count_lo, r_blocks_count_lo, free_blocks_count_lo;
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
} ext4_super_t;

typedef struct {
    uint32_t block_bitmap_lo, inode_bitmap_lo, inode_table_lo;
    uint16_t free_blocks_count_lo, free_inodes_count_lo, used_dirs_count_lo;
    uint16_t flags;
    uint32_t exclude_bitmap_lo;
    uint16_t block_bitmap_csum_lo;
    uint16_t inode_bitmap_csum_lo;
    uint16_t itable_unused_lo;
    uint16_t checksum;
} ext4_bgdesc_t;

typedef struct {
    uint16_t mode, uid;
    uint32_t size_lo, atime, ctime, mtime, dtime;
    uint16_t gid, links_count;
    uint32_t blocks_lo, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl_lo, size_high, obso_faddr;
    uint8_t osd2[12];
} ext4_inode_t;

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} ext4_extent_header_t;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_extent_t;

typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ext4_extent_idx_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    uint8_t name[];
} ext4_dirent_t;
#pragma pack(pop)

typedef struct {
    block_device_t *bdev;
    ext4_super_t sb;
    ext4_bgdesc_t bg;
    uint32_t block_size;
    uint32_t desc_size;
    uint8_t meta_dirty;
    uint32_t next_free_block_hint;
    uint32_t next_free_inode_hint;
    uint32_t bitmap_cache_block;
    uint8_t bitmap_cache_valid;
    uint8_t bitmap_cache_dirty;
    uint8_t bitmap_cache[4096];
} ext4_fs_t;

static ext4_fs_t g_ext4_mounts[8];
static int g_ext4_mount_count;
static uint8_t g_io[4096];
static uint8_t g_blk[4096];

#define EXT4_EXTENTS_FL 0x00080000u
#define EXT4_EXT_MAGIC  0xF30Au

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
#ifndef EXT4_DEBUG
#define EXT4_DEBUG 0
#endif
#define EXT4_DBG(...) do { if (EXT4_DEBUG) printf(__VA_ARGS__); } while (0)

static uint16_t rec_len_min(uint8_t name_len) { return (uint16_t)((8 + name_len + 3) & ~3); }

static uint32_t inode_size_get(const ext4_inode_t *in) {
    return in ? in->size_lo : 0;
}

static uint16_t vfs_mode_from_ext(uint16_t mode) {
    uint16_t kind = mode & 0xF000u;
    uint16_t perms = mode & 07777u;
    if (kind == 0x4000u) return (uint16_t)(VFS_INODE_DIR | perms);
    if (kind == 0x8000u) return (uint16_t)(VFS_INODE_FILE | perms);
    return 0;
}

static int read_block(ext4_fs_t *fs, uint32_t block, void *out) {
    uint32_t cnt;
    if (!fs || !fs->bdev || fs->bdev->sector_size == 0) return -1;
    cnt = fs->block_size / fs->bdev->sector_size;
    if (cnt == 0) return -1;
    return block_read_sectors(fs->bdev, block * cnt, cnt, out);
}

static int write_block(ext4_fs_t *fs, uint32_t block, const void *in) {
    uint32_t cnt;
    if (!fs || !fs->bdev || fs->bdev->sector_size == 0) return -1;
    cnt = fs->block_size / fs->bdev->sector_size;
    if (cnt == 0) return -1;
    return block_write_sectors(fs->bdev, block * cnt, cnt, in);
}

static int sync_super_bg(ext4_fs_t *fs) {
    if (!fs || !fs->bdev) return -1;
    if (block_read_sectors(fs->bdev, 2, 2, g_io) < 0) return -1;
    memcpy(g_io, &fs->sb, sizeof(fs->sb));
    if (block_write_sectors(fs->bdev, 2, 2, g_io) < 0) return -1;
    if (read_block(fs, fs->sb.first_data_block + 1, g_io) < 0) return -1;
    memcpy(g_io, &fs->bg, sizeof(fs->bg));
    return write_block(fs, fs->sb.first_data_block + 1, g_io);
}

static int bitmap_cache_flush(ext4_fs_t *fs) {
    if (!fs) return -1;
    if (!fs->bitmap_cache_valid || !fs->bitmap_cache_dirty) return 0;
    if (write_block(fs, fs->bitmap_cache_block, fs->bitmap_cache) < 0) return -1;
    fs->bitmap_cache_dirty = 0;
    return 0;
}

static int bitmap_cache_load(ext4_fs_t *fs, uint32_t bitmap_block) {
    if (!fs) return -1;
    if (fs->block_size > sizeof(fs->bitmap_cache)) return -1;
    if (fs->bitmap_cache_valid && fs->bitmap_cache_block == bitmap_block) return 0;
    if (bitmap_cache_flush(fs) < 0) return -1;
    if (read_block(fs, bitmap_block, fs->bitmap_cache) < 0) return -1;
    fs->bitmap_cache_valid = 1;
    fs->bitmap_cache_dirty = 0;
    fs->bitmap_cache_block = bitmap_block;
    return 0;
}

static int sync_super_bg_if_dirty(ext4_fs_t *fs) {
    if (!fs) return -1;
    if (bitmap_cache_flush(fs) < 0) return -1;
    if (!fs->meta_dirty) return 0;
    if (sync_super_bg(fs) < 0) return -1;
    fs->meta_dirty = 0;
    return 0;
}

static int read_bgdesc(ext4_fs_t *fs, uint32_t group, ext4_bgdesc_t *out) {
    uint32_t per_block;
    uint32_t gd_block;
    uint32_t gd_index;
    uint32_t off;
    if (!fs || !out) return -1;
    per_block = fs->block_size / fs->desc_size;
    if (per_block == 0) return -1;
    gd_block = fs->sb.first_data_block + 1 + (group / per_block);
    gd_index = group % per_block;
    if (read_block(fs, gd_block, g_io) < 0) return -1;
    off = gd_index * fs->desc_size;
    if (off + sizeof(ext4_bgdesc_t) > fs->block_size) return -1;
    memcpy(out, g_io + off, sizeof(ext4_bgdesc_t));
    return 0;
}

static int read_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *out) {
    ext4_bgdesc_t bg;
    uint32_t idx, group, idx_in_group, off, blk, boff;
    if (!fs || !out || ino == 0 || ino > fs->sb.inodes_count) return -1;
    idx = ino - 1;
    group = idx / fs->sb.inodes_per_group;
    idx_in_group = idx % fs->sb.inodes_per_group;
    if (read_bgdesc(fs, group, &bg) < 0) return -1;
    off = idx_in_group * fs->sb.inode_size;
    blk = bg.inode_table_lo + off / fs->block_size;
    boff = off % fs->block_size;
    if (boff + sizeof(ext4_inode_t) > fs->block_size) return -1;
    if (read_block(fs, blk, g_io) < 0) return -1;
    memcpy(out, g_io + boff, sizeof(ext4_inode_t));
    return 0;
}

static int write_inode(ext4_fs_t *fs, uint32_t ino, const ext4_inode_t *in) {
    ext4_bgdesc_t bg;
    uint32_t idx, group, idx_in_group, off, blk, boff;
    if (!fs || !in || ino == 0 || ino > fs->sb.inodes_count) return -1;
    idx = ino - 1;
    group = idx / fs->sb.inodes_per_group;
    idx_in_group = idx % fs->sb.inodes_per_group;
    if (read_bgdesc(fs, group, &bg) < 0) return -1;
    off = idx_in_group * fs->sb.inode_size;
    blk = bg.inode_table_lo + off / fs->block_size;
    boff = off % fs->block_size;
    if (boff + sizeof(ext4_inode_t) > fs->block_size) return -1;
    if (read_block(fs, blk, g_io) < 0) return -1;
    memcpy(g_io + boff, in, sizeof(ext4_inode_t));
    return write_block(fs, blk, g_io);
}

static int alloc_from_bitmap(ext4_fs_t *fs, uint32_t bitmap_block, uint32_t max, int is_inode) {
    uint32_t n;
    uint32_t start;
    uint32_t *hint;
    uint8_t *bm;
    if (!fs) return -1;
    if (bitmap_cache_load(fs, bitmap_block) < 0) return -1;
    bm = fs->bitmap_cache;
    hint = is_inode ? &fs->next_free_inode_hint : &fs->next_free_block_hint;
    start = (*hint < max) ? *hint : 0u;
    for (n = 0; n < max; ++n) {
        uint32_t i = start + n;
        if (i >= max) i -= max;
        uint32_t byte = i / 8, bit = i % 8;
        if (!(bm[byte] & (1u << bit))) {
            bm[byte] |= (1u << bit);
            fs->bitmap_cache_dirty = 1;
            *hint = i + 1;
            if (*hint >= max) *hint = 0;
            if (is_inode) {
                if (fs->sb.free_inodes_count) fs->sb.free_inodes_count--;
                if (fs->bg.free_inodes_count_lo) fs->bg.free_inodes_count_lo--;
            } else {
                if (fs->sb.free_blocks_count_lo) fs->sb.free_blocks_count_lo--;
                if (fs->bg.free_blocks_count_lo) fs->bg.free_blocks_count_lo--;
            }
            fs->meta_dirty = 1;
            return (int)(i + (is_inode ? 1 : 0));
        }
    }
    return -1;
}

static int extent_header_valid(const ext4_extent_header_t *h) {
    return h && h->eh_magic == EXT4_EXT_MAGIC && h->eh_entries <= h->eh_max;
}

static uint32_t extent_start_phys(const ext4_extent_t *e) {
    uint64_t v = ((uint64_t)e->ee_start_hi << 32) | (uint64_t)e->ee_start_lo;
    return (uint32_t)v;
}

static uint32_t idx_leaf_phys(const ext4_extent_idx_t *e) {
    uint64_t v = ((uint64_t)e->ei_leaf_hi << 32) | (uint64_t)e->ei_leaf_lo;
    return (uint32_t)v;
}

static int extent_find_phys_from_node(ext4_fs_t *fs, const uint8_t *node, uint32_t lblock, uint32_t *phys_out) {
    const ext4_extent_header_t *h = (const ext4_extent_header_t *)node;
    uint16_t i;
    if (!fs || !node || !phys_out || !extent_header_valid(h)) return -1;

    if (h->eh_depth == 0) {
        const ext4_extent_t *ex = (const ext4_extent_t *)(node + sizeof(ext4_extent_header_t));
        for (i = 0; i < h->eh_entries; ++i) {
            uint32_t lb = ex[i].ee_block;
            uint32_t len = (uint32_t)(ex[i].ee_len & 0x7FFFu);
            uint32_t st = extent_start_phys(&ex[i]);
            if (len == 0) continue;
            if (lblock >= lb && lblock < lb + len) {
                *phys_out = st + (lblock - lb);
                return 0;
            }
        }
        return -1;
    }

    {
        const ext4_extent_idx_t *ix = (const ext4_extent_idx_t *)(node + sizeof(ext4_extent_header_t));
        int pick = -1;
        for (i = 0; i < h->eh_entries; ++i) {
            if (ix[i].ei_block <= lblock) pick = (int)i;
            else break;
        }
        if (pick < 0) pick = 0;
        if (pick >= (int)h->eh_entries) return -1;
        if (read_block(fs, idx_leaf_phys(&ix[pick]), g_blk) < 0) return -1;
        return extent_find_phys_from_node(fs, g_blk, lblock, phys_out);
    }
}

static int extent_find_phys(ext4_fs_t *fs, const ext4_inode_t *in, uint32_t lblock, uint32_t *phys_out) {
    return extent_find_phys_from_node(fs, (const uint8_t *)in->block, lblock, phys_out);
}

static int legacy_find_phys(ext4_fs_t *fs, const ext4_inode_t *in, uint32_t lblock, uint32_t *phys_out) {
    if (!fs || !in || !phys_out) return -1;
    if (lblock < 12) {
        if (!in->block[lblock]) return -1;
        *phys_out = in->block[lblock];
        return 0;
    }
    lblock -= 12;
    if (!in->block[12]) return -1;
    if (read_block(fs, in->block[12], g_io) < 0) return -1;
    {
        uint32_t ents = fs->block_size / 4;
        if (lblock >= ents) return -1;
        if (!((uint32_t *)g_io)[lblock]) return -1;
        *phys_out = ((uint32_t *)g_io)[lblock];
        return 0;
    }
}

static int map_find_phys(ext4_fs_t *fs, const ext4_inode_t *in, uint32_t lblock, uint32_t *phys_out) {
    if (in && (in->flags & EXT4_EXTENTS_FL)) {
        return extent_find_phys(fs, in, lblock, phys_out);
    }
    return legacy_find_phys(fs, in, lblock, phys_out);
}

static int extent_init_inode(ext4_inode_t *in) {
    ext4_extent_header_t *h;
    if (!in) return -1;
    memset(in->block, 0, sizeof(in->block));
    h = (ext4_extent_header_t *)in->block;
    h->eh_magic = EXT4_EXT_MAGIC;
    h->eh_entries = 0;
    h->eh_max = (uint16_t)((sizeof(in->block) - sizeof(*h)) / sizeof(ext4_extent_t));
    h->eh_depth = 0;
    h->eh_generation = 0;
    in->flags |= EXT4_EXTENTS_FL;
    return 0;
}

static int extent_map_insert_local(ext4_inode_t *in, uint32_t lblock, uint32_t pblock) {
    ext4_extent_header_t *h;
    ext4_extent_t *ex;
    uint16_t i;
    if (!in) return -1;
    h = (ext4_extent_header_t *)in->block;
    if (!extent_header_valid(h) || h->eh_depth != 0) return -1;
    ex = (ext4_extent_t *)((uint8_t *)in->block + sizeof(*h));

    for (i = 0; i < h->eh_entries; ++i) {
        uint32_t lb = ex[i].ee_block;
        uint32_t len = (uint32_t)(ex[i].ee_len & 0x7FFFu);
        uint32_t st = extent_start_phys(&ex[i]);
        if (len == 0) continue;
        if (lblock >= lb && lblock < lb + len) return 0;
        if (lblock == lb + len && pblock == st + len) {
            if (len < 0x7FFFu) {
                ex[i].ee_len = (uint16_t)(len + 1);
                return 0;
            }
        }
    }

    if (h->eh_entries >= h->eh_max) return -1;
    i = h->eh_entries++;
    ex[i].ee_block = lblock;
    ex[i].ee_len = 1;
    ex[i].ee_start_hi = 0;
    ex[i].ee_start_lo = pblock;
    return 0;
}

static int extent_map_create(ext4_fs_t *fs, ext4_inode_t *in, uint32_t lblock, uint32_t *phys_out) {
    uint32_t phys = 0;
    int nb;
    if (!fs || !in || !phys_out) return -1;
    if (extent_find_phys(fs, in, lblock, &phys) == 0) {
        *phys_out = phys;
        return 0;
    }
    nb = alloc_from_bitmap(fs, fs->bg.block_bitmap_lo, fs->sb.blocks_per_group, 0);
    if (nb < 0) return -1;
    if (extent_map_insert_local(in, lblock, (uint32_t)nb) < 0) return -1;
    *phys_out = (uint32_t)nb;
    return 0;
}

static int legacy_map_create(ext4_fs_t *fs, ext4_inode_t *in, uint32_t lblock, uint32_t *phys_out) {
    int nb;
    if (!fs || !in || !phys_out) return -1;
    if (legacy_find_phys(fs, in, lblock, phys_out) == 0) return 0;
    nb = alloc_from_bitmap(fs, fs->bg.block_bitmap_lo, fs->sb.blocks_per_group, 0);
    if (nb < 0) return -1;
    if (lblock < 12) {
        in->block[lblock] = (uint32_t)nb;
        *phys_out = (uint32_t)nb;
        return 0;
    }
    lblock -= 12;
    {
        uint32_t ents = fs->block_size / 4;
        if (lblock >= ents) return -1;
        if (!in->block[12]) {
            int ib = alloc_from_bitmap(fs, fs->bg.block_bitmap_lo, fs->sb.blocks_per_group, 0);
            if (ib < 0) return -1;
            in->block[12] = (uint32_t)ib;
            memset(g_io, 0, fs->block_size);
            if (write_block(fs, in->block[12], g_io) < 0) return -1;
        }
        if (read_block(fs, in->block[12], g_io) < 0) return -1;
        ((uint32_t *)g_io)[lblock] = (uint32_t)nb;
        if (write_block(fs, in->block[12], g_io) < 0) return -1;
        *phys_out = (uint32_t)nb;
        return 0;
    }
}

static int map_create_phys(ext4_fs_t *fs, ext4_inode_t *in, uint32_t lblock, uint32_t *phys_out) {
    if (in && (in->flags & EXT4_EXTENTS_FL)) return extent_map_create(fs, in, lblock, phys_out);
    return legacy_map_create(fs, in, lblock, phys_out);
}

static void ext4_debug_dump_dir(ext4_fs_t *fs, uint32_t ino, const char *label) {
    ext4_inode_t in;
    uint32_t size, pblk = 0;
    int map_rc;
    uint32_t off;
    if (!fs || !label) return;
    if (read_inode(fs, ino, &in) < 0) {
        EXT4_DBG("[ext4] dbg %s ino=%u read_inode failed\n", label, (unsigned)ino);
        return;
    }
    size = inode_size_get(&in);
    map_rc = map_find_phys(fs, &in, 0, &pblk);
    EXT4_DBG("[ext4] dbg %s ino=%u mode=0x%x flags=0x%x size=%u map0_rc=%d map0=%u\n",
             label, (unsigned)ino, (unsigned)in.mode, (unsigned)in.flags, (unsigned)size, map_rc, (unsigned)pblk);
    if (map_rc < 0) return;
    if (read_block(fs, pblk, g_io) < 0) {
        EXT4_DBG("[ext4] dbg %s ino=%u read_block(%u) failed\n", label, (unsigned)ino, (unsigned)pblk);
        return;
    }
    off = 0;
    while (off + 8 <= fs->block_size && off < 256) {
        ext4_dirent_t *de = (ext4_dirent_t *)(g_io + off);
        char nm[33];
        uint32_t nlen = de->name_len;
        if (de->rec_len < 8 || off + de->rec_len > fs->block_size) {
            EXT4_DBG("[ext4] dbg %s off=%u invalid rec_len=%u\n",
                     label, (unsigned)off, (unsigned)de->rec_len);
            break;
        }
        if (nlen > 32) nlen = 32;
        memcpy(nm, de->name, nlen);
        nm[nlen] = 0;
        EXT4_DBG("[ext4] dbg %s off=%u ino=%u rec=%u nlen=%u type=%u name='%s'\n",
                 label, (unsigned)off, (unsigned)de->inode, (unsigned)de->rec_len,
                 (unsigned)de->name_len, (unsigned)de->file_type, nm);
        off += de->rec_len;
    }
}

static int ext4_lookup(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, vfs_inode_t *out) {
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    ext4_inode_t dino;
    uint32_t dsz, blk_n, bi;
    if (!fs || !dir || !name || !out) return -1;
    if (read_inode(fs, dir->ino, &dino) < 0) return -1;
    if (strcmp(name, "init") == 0) {
        uint32_t p0 = 0;
        int mr = map_find_phys(fs, &dino, 0, &p0);
        EXT4_DBG("[ext4] dbg lookup-init dir_ino=%u flags=0x%x size=%u map0_rc=%d map0=%u\n",
                 (unsigned)dir->ino, (unsigned)dino.flags, (unsigned)inode_size_get(&dino), mr, (unsigned)p0);
    }
    dsz = inode_size_get(&dino);
    blk_n = (dsz + fs->block_size - 1) / fs->block_size;
    for (bi = 0; bi < blk_n; ++bi) {
        uint32_t pblk;
        uint32_t off = 0;
        if (map_find_phys(fs, &dino, bi, &pblk) < 0) continue;
        if (read_block(fs, pblk, g_io) < 0) continue;
        while (off + 8 <= fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(g_io + off);
            if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
            if (de->inode && de->name_len > 0 && de->name_len < VFS_NAME_MAX) {
                char dn[VFS_NAME_MAX];
                ext4_inode_t in;
                uint16_t mode;
                uint32_t found_ino = de->inode;
                memcpy(dn, de->name, de->name_len);
                dn[de->name_len] = 0;
                if (strcmp(dn, (char *)name) == 0) {
                    if (read_inode(fs, found_ino, &in) < 0) return -1;
                    mode = vfs_mode_from_ext(in.mode);
                    if (!mode) return -1;
                    out->ino = found_ino;
                    out->mode = mode;
                    out->size = inode_size_get(&in);
                    out->uid = in.uid;
                    out->gid = in.gid;
                    return 0;
                }
            }
            off += de->rec_len;
        }
    }
    return -1;
}

static int ext4_readdir(vfs_superblock_t *sb, vfs_inode_t *dir, uint32_t idx, char *name_out, vfs_inode_t *inode_out) {
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    ext4_inode_t dino;
    uint32_t dsz, blk_n, bi, seen = 0;
    if (!fs || !dir || !name_out || !inode_out) return -1;
    if (read_inode(fs, dir->ino, &dino) < 0) return -1;
    dsz = inode_size_get(&dino);
    blk_n = (dsz + fs->block_size - 1) / fs->block_size;
    for (bi = 0; bi < blk_n; ++bi) {
        uint32_t pblk;
        uint32_t off = 0;
        if (map_find_phys(fs, &dino, bi, &pblk) < 0) continue;
        if (read_block(fs, pblk, g_io) < 0) continue;
        while (off + 8 <= fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(g_io + off);
            if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
            if (de->inode && de->name_len > 0 && de->name_len < VFS_NAME_MAX) {
                if (seen == idx) {
                    ext4_inode_t in;
                    uint16_t mode;
                    uint32_t found_ino = de->inode;
                    memcpy(name_out, de->name, de->name_len);
                    name_out[de->name_len] = 0;
                    if (read_inode(fs, found_ino, &in) < 0) return -1;
                    mode = vfs_mode_from_ext(in.mode);
                    if (!mode) return -1;
                    inode_out->ino = found_ino;
                    inode_out->mode = mode;
                    inode_out->size = inode_size_get(&in);
                    inode_out->uid = in.uid;
                    inode_out->gid = in.gid;
                    return 0;
                }
                seen++;
            }
            off += de->rec_len;
        }
    }
    return -1;
}

static int ext4_read(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, void *buf, uint32_t len) {
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    ext4_inode_t in;
    uint32_t size;
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    if (!fs || !inode || !buf) return -1;
    if (read_inode(fs, inode->ino, &in) < 0) return -1;
    size = inode_size_get(&in);
    if (off >= size) return 0;
    if (off + len > size) len = size - off;
    while (done < len) {
        uint32_t pos = off + done;
        uint32_t lbi = pos / fs->block_size;
        uint32_t bo = pos % fs->block_size;
        uint32_t pblk;
        uint32_t n;
        n = fs->block_size - bo;
        if (n > len - done) n = len - done;
        if (map_find_phys(fs, &in, lbi, &pblk) < 0 || pblk == 0) {
            /* Sparse hole / unmapped logical block reads as zeroes. */
            memset(out + done, 0, n);
            done += n;
            continue;
        }
        if (read_block(fs, pblk, g_io) < 0) break;
        memcpy(out + done, g_io + bo, n);
        done += n;
    }
    return (int)done;
}

static int ext4_write(vfs_superblock_t *sb, vfs_inode_t *inode, uint32_t off, const void *buf, uint32_t len) {
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    ext4_inode_t in;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;
    uint32_t size;
    uint32_t sectors_per_block;
    if (!fs || !inode || !buf) return -1;
    if (read_inode(fs, inode->ino, &in) < 0) return -1;
    if (!(in.flags & EXT4_EXTENTS_FL)) {
        if (extent_init_inode(&in) < 0) return -1;
    }
    sectors_per_block = fs->block_size / fs->bdev->sector_size;
    if (sectors_per_block == 0) return -1;
    while (done < len) {
        uint32_t pos = off + done;
        uint32_t lbi = pos / fs->block_size;
        uint32_t bo = pos % fs->block_size;
        uint32_t pblk;
        uint32_t n = fs->block_size - bo;
        if (n > len - done) n = len - done;

        /* Fast path for sequential full-block writes: map a contiguous run
         * and submit a single large block-layer write. */
        if (bo == 0 && n == fs->block_size) {
            uint32_t run_blocks = 0;
            uint32_t run_first_pblk = 0;
            uint32_t run_prev_pblk = 0;
            uint32_t max_blocks = (len - done) / fs->block_size;
            if (max_blocks > 256u) max_blocks = 256u;

            while (run_blocks < max_blocks) {
                uint32_t rpblk;
                if (map_create_phys(fs, &in, lbi + run_blocks, &rpblk) < 0) break;
                if (run_blocks == 0) {
                    run_first_pblk = rpblk;
                } else if (rpblk != run_prev_pblk + 1u) {
                    break;
                }
                run_prev_pblk = rpblk;
                run_blocks++;
            }

            if (run_blocks > 0) {
                uint32_t run_secs = run_blocks * sectors_per_block;
                if (block_write_sectors(fs->bdev, run_first_pblk * sectors_per_block, run_secs, src + done) == 0) {
                    done += run_blocks * fs->block_size;
                    continue;
                }
            }
        }

        if (map_create_phys(fs, &in, lbi, &pblk) < 0) break;
        if (!(bo == 0 && n == fs->block_size)) {
            if (read_block(fs, pblk, g_io) < 0) break;
            memcpy(g_io + bo, src + done, n);
            if (write_block(fs, pblk, g_io) < 0) break;
        } else {
            if (write_block(fs, pblk, src + done) < 0) break;
        }
        done += n;
    }
    size = inode_size_get(&in);
    if (off + done > size) {
        in.size_lo = off + done;
        in.size_high = 0;
        size = off + done;
    }
    in.blocks_lo = (size + 511) / 512;
    if (write_inode(fs, inode->ino, &in) < 0) return -1;
    if (sync_super_bg_if_dirty(fs) < 0) return -1;
    inode->size = size;
    return (int)done;
}

static int dir_insert(ext4_fs_t *fs, ext4_inode_t *dino, uint32_t dir_ino, uint32_t child_ino, const char *name, int is_dir) {
    uint16_t nlen = (uint16_t)strlen(name);
    uint16_t need = rec_len_min((uint8_t)nlen);
    uint32_t dsz = inode_size_get(dino);
    uint32_t blk_n = (dsz + fs->block_size - 1) / fs->block_size;
    uint32_t bi;
    for (bi = 0; bi < blk_n; ++bi) {
        uint32_t pblk;
        uint32_t off = 0;
        if (map_find_phys(fs, dino, bi, &pblk) < 0) continue;
        if (read_block(fs, pblk, g_io) < 0) continue;
        while (off + 8 <= fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(g_io + off);
            if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
            if (de->inode == 0 && de->rec_len >= need) {
                uint16_t slot = de->rec_len;
                memset(de, 0, slot);
                de->inode = child_ino;
                de->rec_len = slot;
                de->name_len = (uint8_t)nlen;
                de->file_type = is_dir ? 2 : 1;
                memcpy(de->name, name, nlen);
                if (write_block(fs, pblk, g_io) < 0) return -1;
                return write_inode(fs, dir_ino, dino);
            }
            off += de->rec_len;
        }
    }
    {
        uint32_t pblk;
        uint32_t lbi = blk_n;
        if (map_create_phys(fs, dino, lbi, &pblk) < 0) return -1;
        memset(g_io, 0, fs->block_size);
        {
            ext4_dirent_t *de = (ext4_dirent_t *)g_io;
            de->inode = child_ino;
            de->rec_len = (uint16_t)fs->block_size;
            de->name_len = (uint8_t)nlen;
            de->file_type = is_dir ? 2 : 1;
            memcpy(de->name, name, nlen);
        }
        if (write_block(fs, pblk, g_io) < 0) return -1;
        dino->size_lo = dsz + fs->block_size;
        dino->size_high = 0;
        dino->blocks_lo = (dino->size_lo + 511) / 512;
        return write_inode(fs, dir_ino, dino);
    }
}

static int ext4_create_like(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out, int is_dir) {
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    ext4_inode_t dino, ni;
    vfs_inode_t exists;
    int nino;
    if (!fs || !dir || !name || !out || !name[0] || strlen(name) >= VFS_NAME_MAX) return -1;
    if (read_inode(fs, dir->ino, &dino) < 0) return -1;
    if (ext4_lookup(sb, dir, name, &exists) == 0) return -1;

    nino = alloc_from_bitmap(fs, fs->bg.inode_bitmap_lo, fs->sb.inodes_per_group, 1);
    if (nino < 0) return -1;
    memset(&ni, 0, sizeof(ni));
    ni.mode = (uint16_t)((is_dir ? 0x4000u : 0x8000u) | (mode & 0777u));
    ni.uid = (uint16_t)process_geteuid();
    ni.gid = (uint16_t)process_getegid();
    ni.links_count = is_dir ? 2 : 1;
    ni.flags = EXT4_EXTENTS_FL;
    if (extent_init_inode(&ni) < 0) return -1;

    if (is_dir) {
        int nb = alloc_from_bitmap(fs, fs->bg.block_bitmap_lo, fs->sb.blocks_per_group, 0);
        if (nb < 0) return -1;
        if (extent_map_insert_local(&ni, 0, (uint32_t)nb) < 0) return -1;
        memset(g_io, 0, fs->block_size);
        {
            uint16_t dot = rec_len_min(1);
            ext4_dirent_t *de1 = (ext4_dirent_t *)g_io;
            ext4_dirent_t *de2 = (ext4_dirent_t *)(g_io + dot);
            de1->inode = (uint32_t)nino;
            de1->rec_len = dot;
            de1->name_len = 1;
            de1->file_type = 2;
            de1->name[0] = '.';
            de2->inode = dir->ino;
            de2->rec_len = (uint16_t)(fs->block_size - dot);
            de2->name_len = 2;
            de2->file_type = 2;
            de2->name[0] = '.';
            de2->name[1] = '.';
        }
        if (write_block(fs, (uint32_t)nb, g_io) < 0) return -1;
        ni.size_lo = fs->block_size;
        ni.size_high = 0;
        ni.blocks_lo = fs->block_size / 512;
        dino.links_count++;
    }

    if (write_inode(fs, (uint32_t)nino, &ni) < 0) return -1;
    if (dir_insert(fs, &dino, dir->ino, (uint32_t)nino, name, is_dir) < 0) return -1;
    if (is_dir && write_inode(fs, dir->ino, &dino) < 0) return -1;
    if (sync_super_bg_if_dirty(fs) < 0) return -1;

    out->ino = (uint32_t)nino;
    out->mode = mode;
    out->size = inode_size_get(&ni);
    out->uid = ni.uid;
    out->gid = ni.gid;
    return 0;
}

static int ext4_create(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out) {
    return ext4_create_like(sb, dir, name, mode, out, 0);
}

static int ext4_mkdir(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name, uint16_t mode, vfs_inode_t *out) {
    return ext4_create_like(sb, dir, name, mode, out, 1);
}

static int ext4_unlink(vfs_superblock_t *sb, vfs_inode_t *dir, const char *name) {
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    ext4_inode_t dino;
    uint32_t dsz, blk_n, bi;
    if (!fs || !dir || !name) return -1;
    if (read_inode(fs, dir->ino, &dino) < 0) return -1;
    dsz = inode_size_get(&dino);
    blk_n = (dsz + fs->block_size - 1) / fs->block_size;
    for (bi = 0; bi < blk_n; ++bi) {
        uint32_t pblk;
        uint32_t off = 0;
        if (map_find_phys(fs, &dino, bi, &pblk) < 0) continue;
        if (read_block(fs, pblk, g_io) < 0) continue;
        while (off + 8 <= fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(g_io + off);
            if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
            if (de->inode && de->name_len > 0 && de->name_len < VFS_NAME_MAX) {
                char dn[VFS_NAME_MAX];
                memcpy(dn, de->name, de->name_len);
                dn[de->name_len] = 0;
                if (strcmp(dn, (char *)name) == 0) {
                    de->inode = 0;
                    if (write_block(fs, pblk, g_io) < 0) return -1;
                    return 0;
                }
            }
            off += de->rec_len;
        }
    }
    return -1;
}

static int ext4_statfs(vfs_superblock_t *sb, uint32_t *total_kb, uint32_t *used_kb) {
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    uint64_t total_bytes, free_bytes, used_bytes;
    if (!fs || !total_kb || !used_kb) return -1;
    total_bytes = (uint64_t)fs->sb.blocks_count_lo * (uint64_t)fs->block_size;
    free_bytes = (uint64_t)fs->sb.free_blocks_count_lo * (uint64_t)fs->block_size;
    used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    *total_kb = (uint32_t)(total_bytes / 1024ull);
    *used_kb = (uint32_t)(used_bytes / 1024ull);
    return 0;
}

static int ext4_feature_set_supported(const ext4_super_t *sb) {
    uint32_t unsupported_incompat, unsupported_ro;
    if (!sb) return 0;
    unsupported_incompat = sb->feature_incompat &
        ~(EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_EXTENTS | EXT4_INCOMPAT_FLEX_BG);
    if (unsupported_incompat) {
        printf("[ext4] unsupported incompat=0x%x\n", unsupported_incompat);
        return 0;
    }
    unsupported_ro = sb->feature_ro_compat &
        ~(EXT4_RO_COMPAT_SPARSE_SUPER | EXT4_RO_COMPAT_LARGE_FILE | EXT4_RO_COMPAT_BTREE_DIR | EXT4_RO_COMPAT_DIR_NLINK);
    if (unsupported_ro) {
        printf("[ext4] unsupported ro_compat=0x%x\n", unsupported_ro);
        return 0;
    }
    return 1;
}

static filesystem_ops_t g_ext4_ops = {
    ext4_lookup, ext4_read, ext4_write, ext4_create, ext4_mkdir, ext4_unlink, ext4_readdir, ext4_statfs
};

static int ext4_mount_common(block_device_t *b, const char *dev, const char *target) {
    ext4_fs_t *fs;
    ext4_inode_t root;
    vfs_superblock_t sb;
    uint16_t root_mode;
    if (!b || !target) return -1;
    if (g_ext4_mount_count >= (int)(sizeof(g_ext4_mounts) / sizeof(g_ext4_mounts[0]))) return -1;
    fs = &g_ext4_mounts[g_ext4_mount_count++];
    memset(fs, 0, sizeof(*fs));
    fs->bdev = b;

    if (block_read_sectors(b, 2, 2, g_io) < 0) {
        printf("[ext4] failed reading superblock from %s\n", dev);
        return -1;
    }
    memcpy(&fs->sb, g_io, sizeof(ext4_super_t));
    if (fs->sb.magic != 0xEF53) {
        printf("[ext4] invalid superblock magic on %s\n", dev);
        return -1;
    }
    if (!ext4_feature_set_supported(&fs->sb)) return -1;
    fs->block_size = 1024u << fs->sb.log_block_size;
    fs->desc_size = 32;
    fs->bitmap_cache_valid = 0;
    fs->bitmap_cache_dirty = 0;
    fs->bitmap_cache_block = 0;
    if (fs->block_size == 0 || fs->block_size > sizeof(g_io)) {
        printf("[ext4] unsupported block size on %s\n", dev);
        return -1;
    }

    if (read_block(fs, fs->sb.first_data_block + 1, g_io) < 0) {
        printf("[ext4] failed reading group descriptor on %s\n", dev);
        return -1;
    }
    memcpy(&fs->bg, g_io, sizeof(ext4_bgdesc_t));
    if (read_inode(fs, 2, &root) < 0) {
        printf("[ext4] failed to read root inode on %s\n", dev);
        return -1;
    }

    root_mode = vfs_mode_from_ext(root.mode);
    if (!root_mode || (root_mode & 0xF000u) != VFS_INODE_DIR) {
        printf("[ext4] invalid root inode mode on %s\n", dev);
        return -1;
    }

    memset(&sb, 0, sizeof(sb));
    strcpy(sb.fs_name, "ext4");
    if (dev[0] == '/') strcpy(sb.dev_name, dev); else { strcpy(sb.dev_name, "/dev/"); strcat(sb.dev_name, dev); }
    strcpy(sb.mountpoint, target);
    sb.root.ino = 2;
    sb.root.mode = root_mode;
    sb.root.size = inode_size_get(&root);
    sb.root.uid = root.uid;
    sb.root.gid = root.gid;
    sb.ops = &g_ext4_ops;
    sb.fs_private = fs;

    if (strcmp(target, "/") == 0) {
        vfs_inode_t sbin, init;
        memset(&sbin, 0, sizeof(sbin));
        memset(&init, 0, sizeof(init));
        int rc_sbin = ext4_lookup(&sb, &sb.root, "sbin", &sbin);
        int rc_init = -1;
        EXT4_DBG("[ext4] dbg rootcheck rc_sbin=%d sbin_ino=%u sbin_mode=0x%x\n",
                 rc_sbin, (unsigned)sbin.ino, (unsigned)sbin.mode);
        if (rc_sbin == 0 && (sbin.mode & 0xF000u) == VFS_INODE_DIR) {
            rc_init = ext4_lookup(&sb, &sbin, "init", &init);
        }
        if (rc_sbin < 0 || (sbin.mode & 0xF000u) != VFS_INODE_DIR ||
            rc_init < 0 || (init.mode & 0xF000u) != VFS_INODE_FILE) {
            printf("[ext4] rejecting %s as root: /sbin/init missing (sbin_rc=%d init_rc=%d sbin_mode=0x%x init_mode=0x%x)\n",
                   sb.dev_name, rc_sbin, rc_init, (unsigned)sbin.mode, (unsigned)init.mode);
            {
                char n[VFS_NAME_MAX];
                vfs_inode_t e;
                printf("[ext4] root entries:");
                for (int i = 0; i < 24; ++i) {
                    if (ext4_readdir(&sb, &sb.root, (uint32_t)i, n, &e) < 0) break;
                    printf(" %s", n);
                }
                printf("\n");
            }
            if (rc_sbin == 0 && (sbin.mode & 0xF000u) == VFS_INODE_DIR) {
                char n[VFS_NAME_MAX];
                vfs_inode_t e;
                printf("[ext4] /sbin entries:");
                for (int i = 0; i < 24; ++i) {
                    if (ext4_readdir(&sb, &sbin, (uint32_t)i, n, &e) < 0) break;
                    printf(" %s", n);
                }
                printf("\n");
                ext4_debug_dump_dir(fs, sbin.ino, "/sbin");
            }
            return -1;
        }
    }

    if (vfs_add_superblock(&sb) < 0) return -1;
    printf("[ext4] mounted %s on %s\n", sb.dev_name, target);
    return 0;
}

int ext4_mount(const char *dev, const char *target) {
    block_device_t *b;
    if (!dev || !target) return -1;
    b = block_find(dev[0] == '/' ? dev + 5 : dev);
    if (!b) return -1;
    return ext4_mount_common(b, dev, target);
}

int ext4_mount_block(block_device_t *bdev, const char *target) {
    if (!bdev || !target) return -1;
    return ext4_mount_common(bdev, bdev->name, target);
}

int ext4_setattr(vfs_superblock_t *sb, const vfs_inode_t *inode, uint16_t mode, uint16_t uid, uint16_t gid, uint32_t mask) {
    ext4_fs_t *fs;
    ext4_inode_t in;
    if (!sb || !inode) return -1;
    fs = (ext4_fs_t *)sb->fs_private;
    if (!fs) return -1;
    if (read_inode(fs, inode->ino, &in) < 0) return -1;
    if (mask & 1u) {
        uint16_t kind = in.mode & 0xF000u;
        in.mode = (uint16_t)(kind | (mode & 07777u));
    }
    if (mask & 2u) in.uid = uid;
    if (mask & 4u) in.gid = gid;
    if (write_inode(fs, inode->ino, &in) < 0) return -1;
    return sync_super_bg_if_dirty(fs);
}
