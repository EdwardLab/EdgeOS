#include "fat32.h"
#include "string.h"
#include "stdio.h"

static fat32_node_t g_nodes[FAT32_NODES_MAX];
static uint8_t g_sector_buffer[512];

static int streq(const char *a, const char *b) { return strcmp((char *)a, (char *)b) == 0; }

static int fat32_find_child(int parent, const char *name) {
    for (int i = 0; i < FAT32_NODES_MAX; ++i) {
        if (g_nodes[i].used && g_nodes[i].parent == parent && streq(g_nodes[i].name, name)) return i;
    }
    return -1;
}

static int fat32_alloc_node(void) {
    for (int i = 0; i < FAT32_NODES_MAX; ++i) if (!g_nodes[i].used) return i;
    return -1;
}

static int fat32_create_node(int parent, const char *name, int type) {
    if (!name || !name[0] || streq(name, ".") || streq(name, "..")) return -1;
    if ((int)strlen(name) > FAT32_NAME_MAX) return -1;
    if (fat32_find_child(parent, name) >= 0) return -1;
    int idx = fat32_alloc_node();
    if (idx < 0) return -1;
    memset(&g_nodes[idx], 0, sizeof(g_nodes[idx]));
    g_nodes[idx].used = 1;
    g_nodes[idx].type = type;
    g_nodes[idx].parent = parent;
    strcpy(g_nodes[idx].name, name);
    return idx;
}

static int fat32_resolve(const char *path, int from) {
    char part[FAT32_NAME_MAX + 1];
    int idx = (path && path[0] == '/') ? 0 : from;
    if (!path || !path[0]) return idx;

    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        int pi = 0;
        while (*path && *path != '/') {
            if (pi < FAT32_NAME_MAX) part[pi++] = *path;
            path++;
        }
        part[pi] = 0;
        if (streq(part, ".")) continue;
        if (streq(part, "..")) {
            if (idx != 0) idx = g_nodes[idx].parent;
            continue;
        }
        idx = fat32_find_child(idx, part);
        if (idx < 0) return -1;
    }
    return idx;
}

static int fat32_resolve_parent(const char *path, int from, char *leaf) {
    int len = (int)strlen(path);
    int end = len;
    while (end > 1 && path[end - 1] == '/') end--;
    int start = end - 1;
    while (start >= 0 && path[start] != '/') start--;
    int n = end - (start + 1);
    if (n <= 0 || n > FAT32_NAME_MAX) return -1;
    memcpy(leaf, path + start + 1, n);
    leaf[n] = 0;
    if (start < 0) return from;
    if (start == 0) return 0;

    char parent[FAT32_PATH_MAX];
    if (start >= FAT32_PATH_MAX) return -1;
    memcpy(parent, path, start);
    parent[start] = 0;
    return fat32_resolve(parent, from);
}

void fat32_init(void) {
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_sector_buffer, 0, sizeof(g_sector_buffer));

    g_nodes[0].used = 1;
    g_nodes[0].type = FAT32_TYPE_DIR;
    g_nodes[0].parent = 0;
    g_nodes[0].name[0] = 0;

    fat32_create_node(0, "bin", FAT32_TYPE_DIR);
    fat32_create_node(0, "dev", FAT32_TYPE_DIR);
    fat32_create_node(0, "root", FAT32_TYPE_DIR);
    fat32_create_node(0, "etc", FAT32_TYPE_DIR);
    fat32_create_node(0, "boot", FAT32_TYPE_DIR);
    fat32_create_node(0, "home", FAT32_TYPE_DIR);
    fat32_create_node(0, "tmp", FAT32_TYPE_DIR);
}

fat32_node_t *fat32_get_root(void) { return &g_nodes[0]; }

fat32_node_t *fat32_open(const char *path, fat32_node_t *from) {
    int from_idx = 0;
    if (from) from_idx = (int)(from - g_nodes);
    int idx = fat32_resolve(path, from_idx);
    if (idx < 0) return 0;
    return &g_nodes[idx];
}

int fat32_read(fat32_node_t *node, char *out, int max) {
    if (!node || node->type != FAT32_TYPE_FILE || !out || max <= 0) return -1;
    int n = (int)node->size;
    if (n >= max) n = max - 1;
    memcpy(out, node->content, n);
    out[n] = 0;
    return n;
}

void fat32_list(fat32_node_t *dir, int longf) {
    if (!dir || dir->type != FAT32_TYPE_DIR) return;
    int idx = (int)(dir - g_nodes);
    for (int i = 0; i < FAT32_NODES_MAX; ++i) {
        if (!g_nodes[i].used || g_nodes[i].parent != idx) continue;
        if (longf) printf("%c %5u %s\n", g_nodes[i].type == FAT32_TYPE_DIR ? 'd' : '-', g_nodes[i].size, g_nodes[i].name);
        else printf("%s  ", g_nodes[i].name);
    }
    if (!longf) printf("\n");
}

int fat32_mkdir(const char *path, fat32_node_t *from) {
    char name[FAT32_NAME_MAX + 1];
    int from_idx = from ? (int)(from - g_nodes) : 0;
    int parent = fat32_resolve_parent(path, from_idx, name);
    if (parent < 0 || !g_nodes[parent].used || g_nodes[parent].type != FAT32_TYPE_DIR) return -1;
    return fat32_create_node(parent, name, FAT32_TYPE_DIR) >= 0 ? 0 : -1;
}

int fat32_touch(const char *path, fat32_node_t *from) {
    char name[FAT32_NAME_MAX + 1];
    int from_idx = from ? (int)(from - g_nodes) : 0;
    int parent = fat32_resolve_parent(path, from_idx, name);
    if (parent < 0 || !g_nodes[parent].used || g_nodes[parent].type != FAT32_TYPE_DIR) return -1;
    return fat32_create_node(parent, name, FAT32_TYPE_FILE) >= 0 ? 0 : -1;
}

int fat32_rm(const char *path, fat32_node_t *from) {
    fat32_node_t *n = fat32_open(path, from);
    if (!n || n->type != FAT32_TYPE_FILE) return -1;
    memset(n, 0, sizeof(*n));
    return 0;
}

int fat32_write_text(const char *path, fat32_node_t *from, const char *text) {
    fat32_node_t *n = fat32_open(path, from);
    if (!n) {
        if (fat32_touch(path, from) < 0) return -1;
        n = fat32_open(path, from);
    }
    if (!n || n->type != FAT32_TYPE_FILE) return -1;
    int size = (int)strlen(text);
    if (size > FAT32_CONTENT_MAX) size = FAT32_CONTENT_MAX;
    memcpy(n->content, text, size);
    n->content[size] = 0;
    n->size = (uint32_t)size;
    return 0;
}

int fat32_copy(const char *src, const char *dst, fat32_node_t *from) {
    fat32_node_t *s = fat32_open(src, from);
    if (!s || s->type != FAT32_TYPE_FILE) return -1;
    if (fat32_write_text(dst, from, s->content) < 0) return -1;
    return 0;
}

int fat32_build_path(fat32_node_t *node, char *out, int outsz) {
    int stack[FAT32_NODES_MAX];
    int n = 0;
    int p = node ? (int)(node - g_nodes) : 0;
    if (!out || outsz < 2) return -1;
    if (p == 0) {
        out[0] = '/'; out[1] = 0;
        return 0;
    }
    while (p != 0 && n < FAT32_NODES_MAX) {
        stack[n++] = p;
        p = g_nodes[p].parent;
    }
    int pos = 0;
    out[pos++] = '/';
    for (int i = n - 1; i >= 0; --i) {
        int len = (int)strlen(g_nodes[stack[i]].name);
        if (pos + len + 1 >= outsz) break;
        memcpy(out + pos, g_nodes[stack[i]].name, len);
        pos += len;
        if (i > 0) out[pos++] = '/';
    }
    out[pos] = 0;
    return 0;
}
