#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        printf("Usage: stat FILE...\n");
        return argc < 2 ? 1 : 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        if (sys_stat(argv[i]) < 0) {
            printf("stat: cannot stat '%s'\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
