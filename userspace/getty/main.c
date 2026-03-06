#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <crypt.h>

#define GT_TCGETS 0x5401u
#define GT_TCSETS 0x5402u
#define GT_ICANON 0x0002u
#define GT_ECHO 0x0008u
#define GT_ISIG 0x0001u

static void putstr(const char *s) { write(1, s, strlen(s)); }

struct gt_termios {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_line;
    unsigned char c_cc[32];
    unsigned int c_ispeed;
    unsigned int c_ospeed;
};
static struct gt_termios g_tty_saved;
static struct gt_termios g_tty_getty;
static int g_tty_ready = 0;

static void getty_setup_tty(void) {
    struct gt_termios t;
    if (ioctl(0, GT_TCGETS, &t) < 0) return;
    g_tty_saved = t;
    g_tty_getty = t;
    g_tty_getty.c_lflag &= ~(GT_ICANON | GT_ECHO | GT_ISIG);
    (void)ioctl(0, GT_TCSETS, &g_tty_getty);
    g_tty_ready = 1;
}

static void getty_restore_tty_for_shell(void) {
    if (!g_tty_ready) return;
    g_tty_saved.c_lflag |= (GT_ICANON | GT_ECHO | GT_ISIG);
    (void)ioctl(0, GT_TCSETS, &g_tty_saved);
}

static int readline(char *out, int max, int echo) {
    int n = 0, cur = 0;
    while (n + 1 < max) {
        char c = 0;
        if (read(0, &c, 1) <= 0) {
            sys_sleep(5);
            continue;
        }

        if ((unsigned char)c == 27) {
            char s1 = 0, s2 = 0;
            if (read(0, &s1, 1) <= 0 || read(0, &s2, 1) <= 0) continue;
            if (s1 != '[') continue;
            if (s2 == 'D') {
                if (cur > 0 && echo) write(1, "\b", 1);
                if (cur > 0) cur--;
            } else if (s2 == 'C') {
                if (cur < n && echo) write(1, &out[cur], 1);
                if (cur < n) cur++;
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            write(1, "\n", 1);
            break;
        }

        if (c == 127 || c == '\b') {
            if (cur > 0) {
                int tail = n - cur;
                cur--;
                for (int i = cur; i < n - 1; ++i) out[i] = out[i + 1];
                n--;
                if (echo) {
                    write(1, "\b", 1);
                    if (tail > 0) write(1, &out[cur], (size_t)tail);
                    write(1, " ", 1);
                    for (int i = 0; i < tail + 1; ++i) write(1, "\b", 1);
                }
            }
            continue;
        }

        if ((unsigned char)c < 32) continue;
        if (cur == n) {
            out[n++] = c;
            cur++;
            if (echo) write(1, &c, 1);
        } else {
            int tail = n - cur;
            for (int i = n; i > cur; --i) out[i] = out[i - 1];
            out[cur] = c;
            n++;
            cur++;
            if (echo) {
                write(1, &c, 1);
                write(1, &out[cur], (size_t)tail);
                for (int i = 0; i < tail; ++i) write(1, "\b", 1);
            }
        }
    }
    out[n] = 0;
    return n;
}


static int split_fields(char *line, char **f, int maxf) {
    int n = 0;
    char *p = line;
    while (*p && n < maxf) {
        f[n++] = p;
        while (*p && *p != ':') p++;
        if (!*p) break;
        *p++ = 0;
    }
    return n;
}

static int load_file(const char *path, char *buf, int max) {
    int n = sys_readfile(path, buf, (size_t)(max - 1));
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

static int find_passwd_user(const char *user, char *pw, int pw_sz,
                            char *home, int home_sz, char *shell, int shell_sz,
                            int *uid, int *gid) {
    static char buf[4096];
    if (load_file("/etc/passwd", buf, sizeof(buf)) < 0) return -1;
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        if (!line[0]) continue;
        char *f[8] = {0};
        int nf = split_fields(line, f, 8);
        if (nf < 7) continue;
        if (strcmp(f[0], (char *)user) != 0) continue;
        *uid = 0; *gid = 0;
        for (int i = 0; f[2][i] >= '0' && f[2][i] <= '9'; ++i) *uid = *uid * 10 + (f[2][i] - '0');
        for (int i = 0; f[3][i] >= '0' && f[3][i] <= '9'; ++i) *gid = *gid * 10 + (f[3][i] - '0');
        int pi = 0;
        while (f[1][pi] && pi < pw_sz - 1) { pw[pi] = f[1][pi]; pi++; }
        pw[pi] = 0;
        int hi = 0;
        while (f[5][hi] && hi < home_sz - 1) { home[hi] = f[5][hi]; hi++; }
        home[hi] = 0;
        int si = 0;
        while (f[6][si] && si < shell_sz - 1) { shell[si] = f[6][si]; si++; }
        shell[si] = 0;
        return 0;
    }
    return -1;
}

static int verify_pw_field(const char *stored, const char *pass) {
    if (!stored || !stored[0]) return pass[0] ? -1 : 0;
    if (stored[0] == '!' || stored[0] == '*') return -1;
    {
        const char *c = crypt(pass, stored);
        if (c && strcmp(c, stored) == 0) return 0;
    }
    return strcmp(stored, pass) == 0 ? 0 : -1;
}

static int verify_shadow(const char *user, const char *pass) {
    static char buf[4096];
    if (load_file("/etc/shadow", buf, sizeof(buf)) < 0) return -1;
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        if (!line[0]) continue;
        char *f[4] = {0};
        int nf = split_fields(line, f, 4);
        if (nf < 2) continue;
        if (strcmp(f[0], (char *)user) != 0) continue;
        if (!f[1][0]) return pass[0] ? -1 : 0;
        if (f[1][0] == '!' || f[1][0] == '*') return -1;
        {
            const char *c = crypt(pass, f[1]);
            if (c && strcmp(c, f[1]) == 0) return 0;
        }
        /* Compatibility fallback for legacy plaintext shadow entries. */
        return strcmp(f[1], (char *)pass) == 0 ? 0 : -1;
    }
    return -1;
}

static int file_exists(const char *path) {
    return path && path[0] && access(path, F_OK) == 0;
}

static const char *select_login_shell(const char *passwd_shell) {
    if (file_exists(passwd_shell)) return passwd_shell;
    if (file_exists("/bin/bash")) return "/bin/bash";
    if (file_exists("/bin/sh")) return "/bin/sh";
    return "/bin/esh";
}

static void print_motd_if_exists(void) {
    static char motd[4096];
    int n = sys_readfile("/etc/motd", motd, sizeof(motd));
    if (n <= 0) return;
    (void)write(1, motd, (size_t)n);
    if (motd[n - 1] != '\n') (void)write(1, "\n", 1);
}

static void exec_login_shell(const char *shell_path) {
    char *argv_shell[] = { (char *)(shell_path && shell_path[0] ? shell_path : "/bin/esh"), 0 };
    char *argv_esh[] = { "esh", 0 };

    getty_restore_tty_for_shell();
    execve(argv_shell[0], argv_shell, 0);
    execve("/bin/esh", argv_esh, 0);
}

int main(void) {
    char user[64], pass[64], pw[192], home[128], shell[128];
    int uid, gid;
    getty_setup_tty();

    for (;;) {
        putstr("EdgeOS login: ");
        readline(user, sizeof(user), 1);
        putstr("Password: ");
        readline(pass, sizeof(pass), 0);
        putstr("\n");

        if (find_passwd_user(user, pw, sizeof(pw), home, sizeof(home), shell, sizeof(shell), &uid, &gid) < 0) {
            putstr("Login incorrect\n");
            continue;
        }
        if (pw[0] && strcmp(pw, "x") != 0) {
            if (verify_pw_field(pw, pass) < 0) {
                putstr("Login incorrect\n");
                continue;
            }
        } else {
            if (verify_shadow(user, pass) < 0) {
                putstr("Login incorrect\n");
                continue;
            }
        }

        (void)setgid(gid);
        (void)setuid(uid);
        (void)chdir(home[0] ? home : "/");
        print_motd_if_exists();
        exec_login_shell(select_login_shell(shell));
        putstr("getty: exec failed\n");
    }
}
