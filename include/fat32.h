#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

#define FAT32_PATH_MAX 256
#define FAT32_NAME_MAX 63
#define FAT32_CONTENT_MAX 10000
#define FAT32_NODES_MAX 256

#define FAT32_TYPE_DIR 1
#define FAT32_TYPE_FILE 2

typedef struct fat32_node {
    int used;
    int type;
    int parent;
    char name[FAT32_NAME_MAX + 1];
    char content[FAT32_CONTENT_MAX + 1];
    uint32_t size;
} fat32_node_t;

void fat32_init(void);
fat32_node_t *fat32_get_root(void);
fat32_node_t *fat32_open(const char *path, fat32_node_t *from);
int fat32_read(fat32_node_t *node, char *out, int max);
void fat32_list(fat32_node_t *dir, int longf);
int fat32_mkdir(const char *path, fat32_node_t *from);
int fat32_touch(const char *path, fat32_node_t *from);
int fat32_rm(const char *path, fat32_node_t *from);
int fat32_write_text(const char *path, fat32_node_t *from, const char *text);
int fat32_copy(const char *src, const char *dst, fat32_node_t *from);
int fat32_build_path(fat32_node_t *node, char *out, int outsz);

#endif
