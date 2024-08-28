#include "kernel.h"
#include "console.h"
#include "string.h"
#include "gdt.h"
#include "idt.h"
#include "keyboard.h"
#include "io_ports.h"
#include "framebuffer.h"
#include "multiboot.h"
#include "stdint-gcc.h"
#include "ctypes.h"
#include "qemu.h"
#include "romfont.h"
#include "fs/fs.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "stdio.h"

#define BRAND_QEMU 1
#define BRAND_VBOX 2

#define VERSION "0.05"
#define MAX_HISTORY 10

#define MAX_FILENAME_LENGTH 20
#define MAX_FILE_COUNT 100
#define MAX_FILE_CONTENT_LENGTH 10000

typedef struct {
    char name[MAX_FILENAME_LENGTH];
    int size;
    char content[MAX_FILE_CONTENT_LENGTH];
} File;

typedef struct {
    char name[MAX_FILENAME_LENGTH];
    int file_count;
    File files[MAX_FILE_COUNT];
} Directory;

Directory root_directory;
char command_history[MAX_HISTORY][255];
int history_count = 0;
int current_history_index = 0;

void add_to_history(const char *command) {
    if (strlen(command) > 0) {
        strcpy(command_history[history_count % MAX_HISTORY], command);
        history_count++;
        current_history_index = history_count;
    }
}

void scan(const char *shell, bool init_all) {
    char new_buffer[255];
    printf("%s", shell);
    memset(new_buffer, 0, sizeof(new_buffer));
    getstr_bound(new_buffer, strlen(shell));
}

void custom_strcpy(char *dest, const char *src) {
    while (*src != '\0') {
        *dest = *src;
        src++;
        dest++;
    }
    *dest = '\0';
}


void removeFile(const char *filename) {
    int found = 0;
    for (int i = 0; i < root_directory.file_count; ++i) {
        if (strcmp(root_directory.files[i].name, filename) == 0) {
            found = 1;
            for (int j = i; j < root_directory.file_count - 1; ++j) {
                root_directory.files[j] = root_directory.files[j + 1];
            }
            root_directory.file_count--;
            printf("File '%s' removed successfully.\n", filename);
            break;
        }
    }
    if (!found) {
        printf("File '%s' not found.\n", filename);
    }
}

void __cpuid(uint32 type, uint32 *eax, uint32 *ebx, uint32 *ecx, uint32 *edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "0"(type));
}

int cpuid_info(int print) {
    uint32 brand[12];
    uint32 eax, ebx, ecx, edx;
    uint32 type;

    memset(brand, 0, sizeof(brand));
    __cpuid(0x80000002, (uint32 *)brand + 0x0, (uint32 *)brand + 0x1, (uint32 *)brand + 0x2, (uint32 *)brand + 0x3);
    __cpuid(0x80000003, (uint32 *)brand + 0x4, (uint32 *)brand + 0x5, (uint32 *)brand + 0x6, (uint32 *)brand + 0x7);
    __cpuid(0x80000004, (uint32 *)brand + 0x8, (uint32 *)brand + 0x9, (uint32 *)brand + 0xa, (uint32 *)brand + 0xb);

    if (print) {
        printf("Brand: %s\n", (char *)brand);
        for (type = 0; type < 4; type++) {
            __cpuid(type, &eax, &ebx, &ecx, &edx);
            printf("type:0x%x, eax:0x%x, ebx:0x%x, ecx:0x%x, edx:0x%x\n", type, eax, ebx, ecx, edx);
        }
    }

    if (strstr((char *)brand, "QEMU") != NULL)
        return BRAND_QEMU;

    return BRAND_VBOX;
}

BOOL is_echo(char *b) {
    if ((b[0] == 'e') && (b[1] == 'c') && (b[2] == 'h') && (b[3] == 'o'))
        if (b[4] == ' ' || b[4] == '\0')
            return TRUE;
    return FALSE;
}

void shutdown() {
    int brand = cpuid_info(0);
    if (brand == BRAND_QEMU)
        outports(0x604, 0x2000);
    else
        outports(0x4004, 0x3400);
}

void unameCommand(const char *arg) {
    if (arg == NULL || strcmp(arg, "") == 0) {
        printf("EdgeOS\n");
    } else if (strcmp(arg, "-a") == 0) {
        printf("EdgeOS localhost %s %s x86 EdgeOS Kernel\n", VERSION, COMPILE_TIME);
    } else {
        printf("Invalid argument for uname: %s\n", arg);
    }
}

void new_kernel_instance(char *cmd_to_run) {
    printf("\nRunning Command/Program '%s' in Quantum instance...\n\n", cmd_to_run);
    printf("Command '%s' not found!!\n", cmd_to_run);
}

void vim() {
    char name[255];
    const char *shell_file = "File Name> ";
    printf("%s", shell_file);
    memset(name, 0, sizeof(name));
    getstr_bound(name, strlen(shell_file));
    printf("Edge VIM Editor\n\n");
    printf_color(COLOR_GREEN, "EXIT(.q vim)\n");

    char file_content[255];
    const char *shell_file_content = "> ";

    while (1) {
        if (strcmp(file_content, ".q vim") == 0) {
            main_loop();
        } else {
            printf("%s", shell_file_content);
            memset(file_content, 0, sizeof(file_content));
            getstr_bound(file_content, strlen(shell_file_content));
        }
    }

    createFile(name, file_content);
}

void boot() {
    char buffer[255];
    gdt_init();
    idt_init();

    console_init(COLOR_WHITE, COLOR_BLUE);
    keyboard_init();
    printf("EdgeOS Operating System\n");
    printf("\n");
    printf("Loading Kernel...\n");

    for (volatile int i = 0; i < 200000000; i++);

    main_loop();
}

void calculator() {
    char input[255];
    const char *prompt = "Enter calculation (e.g., 3 + 4) or type 'exit' to quit: ";
    int num1, num2, result;
    char operator;

    while (1) {
        printf("%s", prompt);
        memset(input, 0, sizeof(input));
        getstr_bound(input, strlen(prompt));

        if (strcmp(input, "exit") == 0) {
            break;
        }

        if (sscanf(input, "%d %c %d", &num1, &operator, &num2) == 3) {
            switch (operator) {
                case '+':
                    result = num1 + num2;
                    break;
                case '-':
                    result = num1 - num2;
                    break;
                case '*':
                    result = num1 * num2;
                    break;
                case '/':
                    if (num2 != 0) {
                        result = num1 / num2;
                    } else {
                        printf("Error: Division by zero.\n");
                        continue;
                    }
                    break;
                default:
                    printf("Error: Unknown operator '%c'.\n", operator);
                    continue;
            }
            printf("Result: %d\n", result);
        } else {
            printf("Error: Invalid format. Please use 'number operator number'.\n");
        }
    }
}

void getstr_bound_shell(char *buffer, uint8 bound) {
    if (!buffer) return;
    uint8 i = 0;
    int c;

    while (1) {
        c = kb_getchar();

        if (c == '\n') {
            printf("\n");
            buffer[i] = '\0';
            add_to_history(buffer);
            return;
        } else if (c == '\b') {
            if (i > 0) {
                console_ungetchar();
                i--;
            }
        } else {
            if (i < bound - 1) {
                buffer[i++] = c;
                console_putchar(c);
            }
        }
    }
}



void main_loop() {
    char buffer[255];
    const char *shell_root = "root";
    const char *shell_at = "@";
    const char *shell_edgeos = "edgeos";
    const char *shell_prompt = "~$ ";
    gdt_init();
    idt_init();
    initFileSystem();

    console_init(COLOR_WHITE, COLOR_BLACK);
    keyboard_init();

    printf("Welcome to EdgeOS %s\n", VERSION);
    printf("Type 'help' for a list of commands.\n");

    while (1) {

        printf_color(COLOR_BLUE, shell_root);
        printf_color(COLOR_WHITE, shell_at);
        printf_color(COLOR_GREEN, shell_edgeos);
        printf_color(COLOR_WHITE, shell_prompt);

        memset(buffer, 0, sizeof(buffer));

        getstr_bound_shell(buffer, sizeof(buffer));

        if (strlen(buffer) == 0)
            continue;

        if (strcmp(buffer, "cpuid") == 0) {
            cpuid_info(1);
        } else if (strcmp(buffer, "help") == 0) {
            printf("EdgeOS Operating System\n");
            printf("Commands:\n\n"
                   " help\n"
                   " cpuid\n"
                   " clear\n"
                   " uname [-a]\n"
                   " touch <filename>\n"
                   " ls\n"
                   " cat <filename> (Show file content)\n"
                   " rm <filename>\n"
                   " whoami\n"
                   " echo\n"
                   " exec (Execute a file/program)\n"
                   " shutdown\n\n");

            printf("Important Info: 'MAX FILES: 100', 'MAX FILE CONTENT: 10,000'\n\n");
        } else if (strncmp(buffer, "touch ", 6) == 0) {
            char *filename = buffer + 6;
            char file_content[255];
            const char *shell_file_content = "File Content> ";
            printf("%s", shell_file_content);
            memset(file_content, 0, sizeof(file_content));
            getstr_bound(file_content, strlen(shell_file_content));
            createFile(filename, file_content);
        } else if (strncmp(buffer, "rm ", 3) == 0) {
            char *filename = buffer + 3;
            removeFile(filename);
        } else if (strcmp(buffer, "ls") == 0) {
            listFiles();
        } else if (strncmp(buffer, "cat ", 4) == 0) {
            char *filename = buffer + 4;
            fat_catFile(filename);
        } else if (strncmp(buffer, "uname", 5) == 0) {
            char *arg = buffer + 5;
            while (*arg == ' ') arg++;
            unameCommand(arg);
        } else if (strcmp(buffer, "exec") == 0) {
            char program_name[255];
            const char *prompt = "Run a Program> ";
            printf("Available Programs/Commands to execute:\n");
            printf_color(COLOR_GREEN, " - VIM (Text Editor)\n - calc (Simple Calculator)\n\n");

            printf("%s", prompt);
            memset(program_name, 0, sizeof(program_name));
            getstr_bound(program_name, strlen(prompt));
            if (strcmp(program_name, "vim") == 0) {
                vim();
            } else if (strcmp(program_name, "calc") == 0) {
                calculator();
            } else {
                printf("ERROR: Command '%s' not found :(\n\n");
            }
        } else if (strcmp(buffer, "whoami") == 0) {
            printf("root\n");
        } else if (strcmp(buffer, "clear") == 0) {
            console_clear(COLOR_WHITE, COLOR_BLACK);
        } else if (is_echo(buffer)) {
            printf("%s\n", buffer + 5);
        } else if (strcmp(buffer, "shutdown") == 0) {
            shutdown();
        } else {
            printf("%s: command not found\n", buffer);
        }
    }
}

void kmain() {
    boot();
}
