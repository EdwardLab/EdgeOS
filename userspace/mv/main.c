#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("Usage: mv SOURCE DEST\n");
        return 0;
    }
    if (argc < 3) {
        printf("mv: missing operand\n");
        return 1;
    }
    if (sys_mv(argv[1], argv[2]) < 0) {
        printf("mv: failed\n");
        return 1;
    }
    return 0;
}
