#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("Usage: env\n");
        return 0;
    }
    if (sys_cat("/root/.sh_env") < 0) {
        return 0;
    }
    return 0;
}
