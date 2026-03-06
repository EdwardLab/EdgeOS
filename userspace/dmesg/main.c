#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("Usage: dmesg\n");
        return 0;
    }
    return sys_dmesg() < 0 ? 1 : 0;
}
