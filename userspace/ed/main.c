#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ED_LINE_MAX 256
#define ED_BUF_MAX 8192

static int read_line(char *out, int max) {
    int n = 0;
    while (n + 1 < max) {
        char ch = 0;
        ssize_t r = read(0, &ch, 1);
        if (r < 0) return -1;
        if (r == 0) continue;
        if (ch == '\r') ch = '\n';
        if (ch == '\n') {
            write(1, "\n", 1);
            break;
        }
        if (ch == '\b' || (unsigned char)ch == 127) {
            if (n > 0) {
                n--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        out[n++] = ch;
        write(1, &ch, 1);
    }
    out[n] = 0;
    return n;
}

int main(int argc, char **argv) {
    static char text[ED_BUF_MAX];
    static char line[ED_LINE_MAX];
    int off = 0;

    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        printf("Usage: ed FILE\n");
        printf("Enter lines, single '.' line to save and quit.\n");
        return argc < 2 ? 1 : 0;
    }

    printf("ed: editing %s\n", argv[1]);
    for (;;) {
        write(1, ":", 1);
        int n = read_line(line, sizeof(line));
        if (n < 0) return 1;
        if (n == 1 && line[0] == '.') break;
        if (off + n + 1 >= ED_BUF_MAX) {
            printf("ed: buffer full\n");
            break;
        }
        memcpy(text + off, line, (size_t)n);
        off += n;
        text[off++] = '\n';
    }

    if (sys_writefile(argv[1], text, (size_t)off) < 0) {
        printf("ed: write failed\n");
        return 1;
    }

    return 0;
}
