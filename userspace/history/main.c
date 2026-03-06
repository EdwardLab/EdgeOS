#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("Usage: history\n");
        return 0;
    }
    if (sys_cat("/root/.sh_history") < 0) {
        printf("history: no history\n");
        return 1;
    }
    return 0;
}
