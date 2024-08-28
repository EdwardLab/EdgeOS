#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "fs.h"
uint16_t get_fat_entry(uint16_t cluster);

#define SECTOR_SIZE 512
#define MAX_FILENAME_LENGTH 11 // 8.3 format
#define MAX_FILE_COUNT 224 // Maximum number of files supported in the root directory

// FAT12 Disk Layout
#define BOOT_SECTOR_SIZE 1
#define FAT_COUNT 2
#define FAT_SIZE 9 // Number of sectors per FAT table
#define ROOT_DIR_SIZE 14 // Number of sectors in the root directory

typedef struct {
    char name[MAX_FILENAME_LENGTH];
    uint8_t attr; // File attributes (read-only, hidden, system, etc.)
    uint8_t reserved[10];
    uint16_t time; // Last modification time
    uint16_t date; // Last modification date
    uint16_t start_cluster; // Starting cluster of the file
    uint32_t size; // File size in bytes
} DirectoryEntry;

typedef struct {
    uint8_t boot_sector[SECTOR_SIZE];
    uint8_t fat[FAT_COUNT][FAT_SIZE * SECTOR_SIZE];
    DirectoryEntry root_directory[MAX_FILE_COUNT];
    uint8_t data_area[2880 - (BOOT_SECTOR_SIZE + FAT_COUNT * FAT_SIZE + ROOT_DIR_SIZE)][SECTOR_SIZE];
} FAT12FileSystem;

FAT12FileSystem fs;

void initFileSystem() {
    // Initialize the boot sector
    memset(fs.boot_sector, 0, SECTOR_SIZE);
    fs.boot_sector[0x00] = 0xEB; // JMP instruction
    fs.boot_sector[0x01] = 0x3C; // JMP instruction
    fs.boot_sector[0x02] = 0x90; // NOP instruction
    memcpy(&fs.boot_sector[0x03], "MSDOS5.0", 8); // OEM identifier

    // Initialize the FAT tables
    memset(fs.fat, 0, sizeof(fs.fat));
    fs.fat[0][0] = 0xF0; // Media descriptor byte (indicating a floppy disk)
    fs.fat[0][1] = 0xFF;
    fs.fat[0][2] = 0xFF;

    // Initialize the root directory
    memset(fs.root_directory, 0, sizeof(fs.root_directory));
}

uint16_t find_free_cluster() {
    for (uint16_t i = 2; i < FAT_SIZE * SECTOR_SIZE * 8 / 12; i++) {
        uint16_t value = ((fs.fat[0][i * 3 / 2 + 1] & 0x0F) << 8) | fs.fat[0][i * 3 / 2];
        if (value == 0x000) {
            return i;
        }
    }
    return 0xFFFF; // No free clusters
}

void set_fat(uint16_t cluster, uint16_t value) {
    if (cluster % 2 == 0) {
        fs.fat[0][cluster * 3 / 2] = value & 0xFF;
        fs.fat[0][cluster * 3 / 2 + 1] = (fs.fat[0][cluster * 3 / 2 + 1] & 0xF0) | ((value >> 8) & 0x0F);
    } else {
        fs.fat[0][cluster * 3 / 2] = (fs.fat[0][cluster * 3 / 2] & 0x0F) | ((value << 4) & 0xF0);
        fs.fat[0][cluster * 3 / 2 + 1] = (value >> 4) & 0xFF;
    }
}

void createFile(char *name, char *content) {
    for (int i = 0; i < MAX_FILE_COUNT; i++) {
        if (fs.root_directory[i].name[0] == 0) { // Find an empty directory entry
            strncpy(fs.root_directory[i].name, name, MAX_FILENAME_LENGTH);
            fs.root_directory[i].attr = 0x20; // Regular file

            uint16_t free_cluster = find_free_cluster();
            if (free_cluster == 0xFFFF) {
                printf("No free clusters.\n");
                return;
            }
            fs.root_directory[i].start_cluster = free_cluster;
            fs.root_directory[i].size = strlen(content);

            // Write content to the data area
            uint8_t *data_ptr = fs.data_area[free_cluster - 2];
            strncpy((char *)data_ptr, content, SECTOR_SIZE);
            set_fat(free_cluster, 0xFFF); // Mark end of file

            printf("File '%s' created successfully in FAT12 FS.\n", name);
            return;
        }
    }
    printf("Root directory is full. Cannot create more files.\n");
}

void listFiles() {
    for (int i = 0; i < MAX_FILE_COUNT; ++i) {
        if (fs.root_directory[i].name[0] != 0) {
            printf("- %s, %d bytes\n", fs.root_directory[i].name, fs.root_directory[i].size);
        }
    }
}

void fat_catFile(const char *filename) {
    for (int i = 0; i < MAX_FILE_COUNT; ++i) {
        if (fs.root_directory[i].name[0] == 0) {
            continue;
        }
        if (strncmp(fs.root_directory[i].name, filename, MAX_FILENAME_LENGTH) == 0) {
            uint16_t cluster = fs.root_directory[i].start_cluster;
            uint32_t size = fs.root_directory[i].size;
            uint8_t *data_ptr;

            while (cluster < 0xFFF8 && size > 0) {
                data_ptr = fs.data_area[cluster - 2];
                uint32_t bytes_to_read = (size > SECTOR_SIZE) ? SECTOR_SIZE : size;

                for (uint32_t j = 0; j < bytes_to_read; j++) {
                    console_putchar(data_ptr[j]);
                }

                size -= bytes_to_read;
                cluster = get_fat_entry(cluster);
            }
            printf("\n");
            return;
        }
    }
    printf("File '%s' not found.\n", filename);
}

uint16_t get_fat_entry(uint16_t cluster) {
    uint16_t value;
    if (cluster % 2 == 0) {
        value = (fs.fat[0][cluster * 3 / 2 + 1] << 8) | fs.fat[0][cluster * 3 / 2];
        value &= 0x0FFF;
    } else {
        value = (fs.fat[0][cluster * 3 / 2] >> 4) | (fs.fat[0][cluster * 3 / 2 + 1] << 4);
        value &= 0x0FFF;
    }
    return value;
}
