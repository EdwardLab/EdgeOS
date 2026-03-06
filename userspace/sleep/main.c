#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned to_uint(const char *s) { unsigned v=0; while (*s>='0'&&*s<='9') { v=v*10u+(unsigned)(*s-'0'); s++; } return v; }

int main(int argc, char **argv) {
    if (argc < 2 || (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))) {
        printf("Usage: sleep SECONDS\n");
        return argc < 2 ? 1 : 0;
    }
    unsigned sec = to_uint(argv[1]);
    return sys_sleep(sec * 1000u) < 0 ? 1 : 0;
}
