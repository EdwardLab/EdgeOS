#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int to_int(const char *s) { int v=0; while (*s>='0'&&*s<='9') { v=v*10+(*s-'0'); s++; } return v; }

int main(int argc, char **argv) {
    if (argc < 2 || (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))) {
        printf("Usage: kill PID\n");
        return argc < 2 ? 1 : 0;
    }
    int pid = to_int(argv[1]);
    if (pid <= 0 || sys_kill(pid, 9) < 0) {
        printf("kill: failed\n");
        return 1;
    }
    return 0;
}
