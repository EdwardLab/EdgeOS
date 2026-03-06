#include "elf/elf_loader.h"

#include "stdio.h"
#include "string.h"
#include "vfs/vfs.h"

#include <stdint.h>

#define ELF64_EHDR_SIZE 64
#define ELF64_PHDR_SIZE 56
#define ELF64_DYN_SIZE  16
#define ELF64_RELA_SIZE 24

#define ELF_MAX_IMAGE (4 * 1024 * 1024)
#define ELF_IO_CHUNK   4096
#define ELF_MAX_PHNUM  128

#define PT_LOAD   1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_PHDR 6u

#define ET_EXEC 2u
#define ET_DYN  3u

#define PF_W 2u

#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9

#define R_X86_64_RELATIVE 8u

#define USER_ADDR_MIN 0x0000000000001000ULL
#define USER_ADDR_MAX 0x0000000040000000ULL

#define EDGE_MAIN_ET_DYN_BASE   0x0000000000400000ULL
#define EDGE_MAIN_ET_DYN_BASE_LARGE 0x0000000030000000ULL
#define EDGE_INTERP_ET_DYN_BASE 0x0000000020000000ULL
#define USER_LOW_BASE_ADDR      0x0000000000400000ULL
#define USER_LOW_LIMIT_ADDR     (USER_LOW_BASE_ADDR + (4ULL * 1024ULL * 1024ULL))

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

typedef struct {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} elf64_dyn_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} elf64_rela_t;

typedef struct {
    uint64_t load_bias;
    uint64_t entry;
    uint64_t phdr;
    uint64_t phnum;
    uint64_t load_hi;
    uint64_t interp_off;
    uint64_t interp_sz;
    uint8_t has_interp;
    char interp_path[256];
} edge_elf_loaded_t;

static unsigned char g_main_file[ELF_MAX_IMAGE];
static unsigned char g_interp_file[ELF_MAX_IMAGE];
static elf64_phdr_t g_elf_phdr_tmp[ELF_MAX_PHNUM];
static uint8_t g_elf_io_chunk[ELF_IO_CHUNK];

static int elf_valid(const elf64_ehdr_t *eh) {
    if (!eh) return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return 0;
    if (eh->e_ident[4] != 2) return 0;
    if (eh->e_machine != 62) return 0;
    if (!(eh->e_type == ET_EXEC || eh->e_type == ET_DYN)) return 0;
    return 1;
}

static int user_addr_range_ok(uint64_t addr, uint64_t len) {
    if (addr < USER_ADDR_MIN) return 0;
    if (addr >= USER_ADDR_MAX) return 0;
    if (len == 0) return 1;
    if (addr + len < addr) return 0;
    if (addr + len > USER_ADDR_MAX) return 0;
    return 1;
}

static int edge_load_relative_relocs(const elf64_ehdr_t *eh, const elf64_phdr_t *ph, uint64_t phnum, uint64_t load_bias) {
    (void)eh;
    const elf64_dyn_t *dyn = 0;
    uint64_t rela_addr = 0;
    uint64_t rela_sz = 0;
    uint64_t rela_ent = ELF64_RELA_SIZE;

    for (uint64_t i = 0; i < phnum; ++i) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        dyn = (const elf64_dyn_t *)(uintptr_t)(load_bias + ph[i].p_vaddr);
        break;
    }
    if (!dyn) return 0;

    for (;;) {
        if (dyn->d_tag == DT_NULL) break;
        if (dyn->d_tag == DT_RELA) rela_addr = dyn->d_un.d_ptr;
        else if (dyn->d_tag == DT_RELASZ) rela_sz = dyn->d_un.d_val;
        else if (dyn->d_tag == DT_RELAENT) rela_ent = dyn->d_un.d_val;
        dyn++;
    }
    if (rela_addr == 0 || rela_sz == 0 || rela_ent < ELF64_RELA_SIZE) return 0;

    {
        uint64_t rela_count = rela_sz / rela_ent;
        const elf64_rela_t *rela = (const elf64_rela_t *)(uintptr_t)(load_bias + rela_addr);
        for (uint64_t i = 0; i < rela_count; ++i) {
            const elf64_rela_t *r = (const elf64_rela_t *)((const uint8_t *)rela + i * rela_ent);
            uint32_t rtype = (uint32_t)(r->r_info & 0xFFFFFFFFu);
            uint64_t where = load_bias + r->r_offset;
            uint64_t value = load_bias + (uint64_t)r->r_addend;
            if (rtype != R_X86_64_RELATIVE) continue;
            if (!user_addr_range_ok(where, sizeof(uint64_t))) return -1;
            *(uint64_t *)(uintptr_t)where = value;
        }
    }
    return 0;
}

static int edge_load_elf_from_buf(const unsigned char *file, int file_sz, uint64_t et_dyn_base, edge_elf_loaded_t *out) {
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)file;
    const elf64_phdr_t *ph;
    uint64_t load_bias;

    if (!file || !out || file_sz < ELF64_EHDR_SIZE) return -1;
    if (!elf_valid(eh)) return -1;
    if (eh->e_phentsize != ELF64_PHDR_SIZE) return -1;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * ELF64_PHDR_SIZE > (uint64_t)file_sz) return -1;

    load_bias = (eh->e_type == ET_DYN) ? et_dyn_base : 0;
    ph = (const elf64_phdr_t *)(file + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        uint64_t dst;
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > (uint64_t)file_sz) return -1;
        if (ph[i].p_memsz < ph[i].p_filesz) return -1;
        dst = load_bias + ph[i].p_vaddr;
        if (!user_addr_range_ok(dst, ph[i].p_memsz)) return -1;
        memcpy((void *)(uintptr_t)dst, file + ph[i].p_offset, (uint32_t)ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset((void *)(uintptr_t)(dst + ph[i].p_filesz), 0, (uint32_t)(ph[i].p_memsz - ph[i].p_filesz));
        }
    }

    if (edge_load_relative_relocs(eh, ph, eh->e_phnum, load_bias) < 0) return -1;

    memset(out, 0, sizeof(*out));
    out->load_bias = load_bias;
    out->entry = load_bias + eh->e_entry;
    out->phdr = 0;
    out->phnum = eh->e_phnum;

    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_PHDR) out->phdr = load_bias + ph[i].p_vaddr;
        if (ph[i].p_type != PT_INTERP) continue;
        out->has_interp = 1;
        out->interp_off = ph[i].p_offset;
        out->interp_sz = ph[i].p_filesz;
    }
    if (!out->phdr) {
        for (uint16_t i = 0; i < eh->e_phnum; ++i) {
            if (ph[i].p_type != PT_LOAD) continue;
            if (eh->e_phoff < ph[i].p_offset) continue;
            if (eh->e_phoff + (uint64_t)eh->e_phnum * ELF64_PHDR_SIZE > ph[i].p_offset + ph[i].p_filesz) continue;
            out->phdr = load_bias + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
            break;
        }
    }
    if (!out->phdr) return -1;
    return 0;
}

static int vfs_pread_exact(vfs_superblock_t *sb, vfs_inode_t *ino, uint32_t off, void *buf, uint32_t len) {
    uint32_t done = 0;
    if (!sb || !ino || !buf) return -1;
    if (len == 0) return 0;
    if (!sb->ops || !sb->ops->read) return -1;
    while (done < len) {
        int r = sb->ops->read(sb, ino, off + done, (uint8_t *)buf + done, len - done);
        if (r < 0) return -1;
        if (r == 0) {
            uint32_t pos = off + done;
            /* Some fs backends can expose sparse file holes as short/zero reads
             * at nonzero offsets. For executable file contents this should read
             * as zero bytes, not fail the load. */
            if (pos < ino->size) {
                memset((uint8_t *)buf + done, 0, len - done);
                return 0;
            }
            return -1;
        }
        done += (uint32_t)r;
    }
    return 0;
}

static int edge_load_elf_from_vfs(const char *path, uint64_t et_dyn_base, edge_elf_loaded_t *out) {
    vfs_inode_t ino;
    vfs_superblock_t *sb = 0;
    elf64_ehdr_t eh;
    elf64_phdr_t *ph = g_elf_phdr_tmp;
    uint64_t load_bias;
    uint64_t ph_bytes;
    uint64_t load_hi = 0;

    if (!path || !out) return -1;
    if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) { printf("[elf][err] resolve %s\n", path); return -1; }
    if ((ino.mode & 0xF000u) == VFS_INODE_DIR) { printf("[elf][err] dir %s\n", path); return -1; }
    if (ino.size < ELF64_EHDR_SIZE) { printf("[elf][err] short %s sz=%u\n", path, ino.size); return -1; }
    if (vfs_pread_exact(sb, &ino, 0, &eh, ELF64_EHDR_SIZE) < 0) { printf("[elf][err] read ehdr %s\n", path); return -1; }
    if (!elf_valid(&eh)) { printf("[elf][err] invalid ehdr %s\n", path); return -1; }
    if (eh.e_phentsize != ELF64_PHDR_SIZE) { printf("[elf][err] phentsz %s %u\n", path, eh.e_phentsize); return -1; }
    if (eh.e_phnum == 0 || eh.e_phnum > ELF_MAX_PHNUM) { printf("[elf][err] phnum %s %u\n", path, eh.e_phnum); return -1; }

    ph_bytes = (uint64_t)eh.e_phnum * ELF64_PHDR_SIZE;
    if (eh.e_phoff + ph_bytes > (uint64_t)ino.size) { printf("[elf][err] phoff range %s\n", path); return -1; }
    if (vfs_pread_exact(sb, &ino, (uint32_t)eh.e_phoff, ph, (uint32_t)ph_bytes) < 0) { printf("[elf][err] read phdr %s\n", path); return -1; }

    load_bias = (eh.e_type == ET_DYN) ? et_dyn_base : 0;
    if (eh.e_type == ET_DYN && et_dyn_base == EDGE_MAIN_ET_DYN_BASE) {
        uint64_t rel_hi = 0;
        for (uint16_t i = 0; i < eh.e_phnum; ++i) {
            uint64_t end;
            if (ph[i].p_type != PT_LOAD) continue;
            end = ph[i].p_vaddr + ph[i].p_memsz;
            if (end < ph[i].p_vaddr) { printf("[elf][err] rel_hi ovf %s\n", path); return -1; }
            if (end > rel_hi) rel_hi = end;
        }
        if (EDGE_MAIN_ET_DYN_BASE + rel_hi > USER_LOW_LIMIT_ADDR) {
            load_bias = EDGE_MAIN_ET_DYN_BASE_LARGE;
        }
    }

    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        uint64_t dst;
        uint64_t end;
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > (uint64_t)ino.size) { printf("[elf][err] seg file range %s i=%u\n", path, i); return -1; }
        if (ph[i].p_memsz < ph[i].p_filesz) { printf("[elf][err] seg mem<file %s i=%u\n", path, i); return -1; }
        dst = load_bias + ph[i].p_vaddr;
        if (!user_addr_range_ok(dst, ph[i].p_memsz)) { printf("[elf][err] seg addr %s i=%u dst=0x%x mem=0x%x bias=0x%x\n", path, i, (uint32_t)dst, (uint32_t)ph[i].p_memsz, (uint32_t)load_bias); return -1; }
        end = dst + ph[i].p_memsz;
        if (end < dst) { printf("[elf][err] seg end ovf %s i=%u\n", path, i); return -1; }
        if (end > load_hi) load_hi = end;

        for (uint64_t done = 0; done < ph[i].p_filesz; ) {
            uint64_t foff = ph[i].p_offset + done;
            uint32_t n = (uint32_t)(ph[i].p_filesz - done);
            uint32_t mis = (uint32_t)(foff & (ELF_IO_CHUNK - 1));
            if (mis != 0) {
                uint32_t prefix = ELF_IO_CHUNK - mis;
                if (n > prefix) n = prefix;
            } else if (n > ELF_IO_CHUNK) {
                n = ELF_IO_CHUNK;
            }
            if (vfs_pread_exact(sb, &ino, (uint32_t)foff, g_elf_io_chunk, n) < 0) { printf("[elf][err] seg read %s i=%u off=0x%x n=0x%x\n", path, i, (uint32_t)foff, n); return -1; }
            memcpy((void *)(uintptr_t)(dst + done), g_elf_io_chunk, n);
            done += n;
        }
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset((void *)(uintptr_t)(dst + ph[i].p_filesz), 0, (uint32_t)(ph[i].p_memsz - ph[i].p_filesz));
        }
    }

    if (edge_load_relative_relocs(&eh, ph, eh.e_phnum, load_bias) < 0) { printf("[elf][err] rela %s bias=0x%x\n", path, (uint32_t)load_bias); return -1; }

    memset(out, 0, sizeof(*out));
    out->load_bias = load_bias;
    out->entry = load_bias + eh.e_entry;
    out->phdr = 0;
    out->phnum = eh.e_phnum;
    out->load_hi = load_hi;

    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type == PT_PHDR) out->phdr = load_bias + ph[i].p_vaddr;
        if (ph[i].p_type != PT_INTERP) continue;
        out->has_interp = 1;
        out->interp_off = ph[i].p_offset;
        out->interp_sz = ph[i].p_filesz;
        if (ph[i].p_filesz < 2 || ph[i].p_filesz >= sizeof(out->interp_path)) { printf("[elf][err] interp size %s\n", path); return -1; }
        if (ph[i].p_offset + ph[i].p_filesz > (uint64_t)ino.size) { printf("[elf][err] interp range %s\n", path); return -1; }
        if (vfs_pread_exact(sb, &ino, (uint32_t)ph[i].p_offset, out->interp_path, (uint32_t)ph[i].p_filesz) < 0) { printf("[elf][err] interp read %s\n", path); return -1; }
        out->interp_path[ph[i].p_filesz - 1] = 0;
    }
    if (!out->phdr) {
        for (uint16_t i = 0; i < eh.e_phnum; ++i) {
            if (ph[i].p_type != PT_LOAD) continue;
            if (eh.e_phoff < ph[i].p_offset) continue;
            if (eh.e_phoff + ph_bytes > ph[i].p_offset + ph[i].p_filesz) continue;
            out->phdr = load_bias + ph[i].p_vaddr + (eh.e_phoff - ph[i].p_offset);
            break;
        }
    }
    if (!out->phdr) { printf("[elf][err] no phdr %s\n", path); return -1; }
    return 0;
}

int elf_loader_probe(const char *path) {
    int n = vfs_read_file(path, (char *)g_main_file, ELF64_EHDR_SIZE);
    if (n < ELF64_EHDR_SIZE) return -1;
    return elf_valid((const elf64_ehdr_t *)g_main_file) ? 0 : -1;
}

int elf_loader_exec(const char *path, edge_elf_image_t *out) {
    edge_elf_loaded_t main_img;
    edge_elf_loaded_t interp_img;

    if (!path || !out) return -1;
    if (edge_load_elf_from_vfs(path, EDGE_MAIN_ET_DYN_BASE, &main_img) < 0) {
        printf("[elf][err] main load failed %s\n", path);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->entry_rip = main_img.entry;
    out->at_phdr = main_img.phdr;
    out->at_phnum = main_img.phnum;
    out->at_entry = main_img.entry;
    out->at_base = 0;
    out->main_load_hi = main_img.load_hi;

    if (!main_img.has_interp) return 0;
    if (main_img.interp_path[0] != '/') { printf("[elf][err] bad interp path %s\n", path); return -1; }
    if (edge_load_elf_from_vfs(main_img.interp_path, EDGE_INTERP_ET_DYN_BASE, &interp_img) < 0) {
        printf("[elf][err] interp load failed %s -> %s\n", path, main_img.interp_path);
        return -1;
    }

    out->entry_rip = interp_img.entry;
    out->at_base = interp_img.load_bias;
    return 0;
}
