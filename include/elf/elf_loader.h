#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>

int elf_loader_probe(const char *path);

typedef struct edge_elf_image {
    uint64_t entry_rip;
    uint64_t at_phdr;
    uint64_t at_phnum;
    uint64_t at_entry;
    uint64_t at_base;
    uint64_t main_load_hi;
} edge_elf_image_t;

int elf_loader_exec(const char *path, edge_elf_image_t *out);

#endif
