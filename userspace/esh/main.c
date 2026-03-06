#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define SH_LINE_MAX 256
#define SH_ARGV_MAX 32
#define SH_HIST_MAX 64
#define SH_ENV_MAX 32
#define SH_CURSOR_BLINK_TICKS 400000
#define SH_TCGETS 0x5401u
#define SH_TCSETS 0x5402u
#define SH_ICANON 0x0002u
#define SH_ECHO 0x0008u
#define SH_ISIG 0x0001u

static char g_cwd[128] = "/";
static char g_hist[SH_HIST_MAX][SH_LINE_MAX];
static int g_hist_count = 0;

struct sh_termios {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_line;
    unsigned char c_cc[32];
    unsigned int c_ispeed;
    unsigned int c_ospeed;
};
static struct sh_termios g_tty_saved;
static struct sh_termios g_tty_editor;
static int g_tty_ready = 0;

typedef struct {
    char key[32];
    char val[128];
} sh_env_t;

static sh_env_t g_env[SH_ENV_MAX];
static int g_env_count = 0;
static int sh_buf_append_ch(char *out, int out_sz, int *off, char c);
static int sh_buf_append_str(char *out, int out_sz, int *off, const char *s);

static void sh_env_save(void) {
    static char blob[SH_ENV_MAX * 180];
    int off = 0;
    for (int i = 0; i < g_env_count; ++i) {
        if (sh_buf_append_str(blob, sizeof(blob), &off, g_env[i].key) < 0) break;
        if (sh_buf_append_ch(blob, sizeof(blob), &off, '=') < 0) break;
        if (sh_buf_append_str(blob, sizeof(blob), &off, g_env[i].val) < 0) break;
        if (sh_buf_append_ch(blob, sizeof(blob), &off, '\n') < 0) break;
    }
    sys_writefile("/root/.sh_env", blob, (size_t)off);
}

static int sh_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int sh_streq(const char *a, const char *b) {
    return strcmp((char *)a, (char *)b) == 0;
}

static void sh_itoa(int v, char *out) {
    char tmp[16];
    int n = 0;
    int neg = 0;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    if (v < 0) {
        neg = 1;
        v = -v;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    if (neg) tmp[n++] = '-';
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static int sh_buf_append_ch(char *out, int out_sz, int *off, char c) {
    if (*off + 1 >= out_sz) return -1;
    out[*off] = c;
    (*off)++;
    out[*off] = 0;
    return 0;
}

static int sh_buf_append_str(char *out, int out_sz, int *off, const char *s) {
    for (int i = 0; s && s[i]; ++i) {
        if (sh_buf_append_ch(out, out_sz, off, s[i]) < 0) return -1;
    }
    return 0;
}

static void sh_refresh_cwd(void) {
    if (getcwd(g_cwd, sizeof(g_cwd)) < 0) strcpy(g_cwd, "/");
}

static void sh_write(const char *s) {
    write(1, s, strlen(s));
}

static void sh_setup_tty_for_line_editor(void) {
    struct sh_termios t;
    if (ioctl(0, SH_TCGETS, &t) < 0) return;
    g_tty_saved = t;
    g_tty_editor = t;
    g_tty_editor.c_lflag &= ~(SH_ICANON | SH_ECHO | SH_ISIG);
    if (ioctl(0, SH_TCSETS, &g_tty_editor) < 0) return;
    g_tty_ready = 1;
}

static void sh_set_tty_editor_mode(int editor_mode) {
    if (!g_tty_ready) return;
    (void)ioctl(0, SH_TCSETS, editor_mode ? &g_tty_editor : &g_tty_saved);
}

static void sh_get_hostname(char *out, int out_sz) {
    if (!out || out_sz < 2) return;
    if (gethostname(out, (size_t)out_sz) == 0 && out[0]) return;
    strcpy(out, "edgeos");
}

static int sh_prompt(char *out, int max) {
    int n = 0;
    char host[65];
    const char *user = (geteuid() == 0) ? "root@" : "user@";
    const char *b = (geteuid() == 0) ? "# " : "$ ";
    sh_get_hostname(host, sizeof(host));
    while (*user && n < max - 1) out[n++] = *user++;
    for (int i = 0; host[i] && n < max - 1; ++i) out[n++] = host[i];
    if (n < max - 1) out[n++] = ':';
    for (int i = 0; g_cwd[i] && n < max - 1; ++i) out[n++] = g_cwd[i];
    while (*b && n < max - 1) out[n++] = *b++;
    out[n] = 0;
    return n;
}

static const char *sh_env_get(const char *key) {
    for (int i = 0; i < g_env_count; ++i) {
        if (sh_streq(g_env[i].key, key)) return g_env[i].val;
    }
    return "";
}

static void sh_env_set(const char *key, const char *val) {
    if (!key || !key[0]) return;
    for (int i = 0; i < g_env_count; ++i) {
        if (sh_streq(g_env[i].key, key)) {
            int j = 0;
            while (val && val[j] && j < (int)sizeof(g_env[i].val) - 1) {
                g_env[i].val[j] = val[j];
                ++j;
            }
            g_env[i].val[j] = 0;
            sh_env_save();
            return;
        }
    }
    if (g_env_count >= SH_ENV_MAX) return;
    int k = 0;
    while (key[k] && k < (int)sizeof(g_env[g_env_count].key) - 1) {
        g_env[g_env_count].key[k] = key[k];
        ++k;
    }
    g_env[g_env_count].key[k] = 0;
    int v = 0;
    while (val && val[v] && v < (int)sizeof(g_env[g_env_count].val) - 1) {
        g_env[g_env_count].val[v] = val[v];
        ++v;
    }
    g_env[g_env_count].val[v] = 0;
    g_env_count++;
    sh_env_save();
}

static void sh_hist_save(void) {
    static char blob[SH_HIST_MAX * SH_LINE_MAX + 8];
    int off = 0;
    for (int i = 0; i < g_hist_count; ++i) {
        if (sh_buf_append_str(blob, sizeof(blob), &off, g_hist[i]) < 0) break;
        if (sh_buf_append_ch(blob, sizeof(blob), &off, '\n') < 0) break;
    }
    sys_writefile("/root/.sh_history", blob, (size_t)off);
}

static void sh_hist_add(const char *line) {
    if (!line[0]) return;
    if (g_hist_count > 0 && strcmp(g_hist[g_hist_count - 1], (char *)line) == 0) return;
    if (g_hist_count < SH_HIST_MAX) {
        strcpy(g_hist[g_hist_count++], line);
        sh_hist_save();
        return;
    }
    for (int i = 1; i < SH_HIST_MAX; ++i) strcpy(g_hist[i - 1], g_hist[i]);
    strcpy(g_hist[SH_HIST_MAX - 1], line);
    sh_hist_save();
}

static void sh_set_line_from_hist(char *line, int *len, int *cursor, const char *src) {
    int n = (int)strlen(src);
    if (n >= SH_LINE_MAX) n = SH_LINE_MAX - 1;
    memcpy(line, src, (size_t)n);
    line[n] = 0;
    *len = n;
    *cursor = n;
}

static void sh_redraw_line(const char *prompt, const char *line, int len, int cursor, int show_cursor) {
    static int prev_total = 0;
    int plen = (int)strlen(prompt);
    (void)show_cursor;
    int payload = len;
    int total = plen + payload;
    int desired = plen + cursor;
    write(1, "\r", 1);
    sh_write(prompt);

    for (int i = 0; i < len; ++i) {
        write(1, &line[i], 1);
    }

    while (total < prev_total) {
        write(1, " ", 1);
        total++;
    }
    while (total > desired) {
        write(1, "\b", 1);
        total--;
    }
    prev_total = plen + payload;
}

static int sh_readline(const char *prompt, char *out, int max_len) {
    int len = 0;
    int cursor = 0;
    int hist_pos = -1;
    out[0] = 0;
    sh_redraw_line(prompt, out, len, cursor, 1);

    while (len + 1 < max_len) {
        char ch = 0;
        ssize_t n = read(0, &ch, 1);
        if (n < 0) {
            sys_sleep(5);
            continue;
        }
        if (n == 0) {
            sys_sleep(5);
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            sh_redraw_line(prompt, out, len, cursor, 0);
            write(1, "\n", 1);
            break;
        }
        if ((unsigned char)ch == 3) {
            len = 0;
            cursor = 0;
            out[0] = 0;
            sh_redraw_line(prompt, out, len, cursor, 0);
            write(1, "^C\n", 3);
            return 0;
        }

        if ((unsigned char)ch == 0x80) {
            if (g_hist_count > 0) {
                if (hist_pos < 0) hist_pos = g_hist_count - 1;
                else if (hist_pos > 0) hist_pos--;
                sh_set_line_from_hist(out, &len, &cursor, g_hist[hist_pos]);
                sh_redraw_line(prompt, out, len, cursor, 1);
            }
            continue;
        }

        if ((unsigned char)ch == 0x81) {
            if (hist_pos >= 0) {
                hist_pos++;
                if (hist_pos >= g_hist_count) {
                    hist_pos = -1;
                    len = 0;
                    cursor = 0;
                    out[0] = 0;
                } else {
                    sh_set_line_from_hist(out, &len, &cursor, g_hist[hist_pos]);
                }
                sh_redraw_line(prompt, out, len, cursor, 1);
            }
            continue;
        }

        if ((unsigned char)ch == 0x82) {
            if (cursor > 0) cursor--;
            sh_redraw_line(prompt, out, len, cursor, 1);
            continue;
        }

        if ((unsigned char)ch == 0x83) {
            if (cursor < len) cursor++;
            sh_redraw_line(prompt, out, len, cursor, 1);
            continue;
        }

        if ((unsigned char)ch == 27) {
            char s1 = 0, s2 = 0;
            if (read(0, &s1, 1) <= 0 || read(0, &s2, 1) <= 0) continue;
            if (s1 == '[' && s2 == 'A') {
                if (g_hist_count > 0) {
                    if (hist_pos < 0) hist_pos = g_hist_count - 1;
                    else if (hist_pos > 0) hist_pos--;
                    sh_set_line_from_hist(out, &len, &cursor, g_hist[hist_pos]);
                    sh_redraw_line(prompt, out, len, cursor, 1);
                }
                continue;
            }
            if (s1 == '[' && s2 == 'B') {
                if (hist_pos >= 0) {
                    hist_pos++;
                    if (hist_pos >= g_hist_count) {
                        hist_pos = -1;
                        len = 0;
                        cursor = 0;
                        out[0] = 0;
                    } else {
                        sh_set_line_from_hist(out, &len, &cursor, g_hist[hist_pos]);
                    }
                    sh_redraw_line(prompt, out, len, cursor, 1);
                }
                continue;
            }
            if (s1 == '[' && s2 == 'D') {
                if (cursor > 0) cursor--;
                sh_redraw_line(prompt, out, len, cursor, 1);
                continue;
            }
            if (s1 == '[' && s2 == 'C') {
                if (cursor < len) cursor++;
                sh_redraw_line(prompt, out, len, cursor, 1);
                continue;
            }
            continue;
        }

        hist_pos = -1;

        if (ch == '\b' || (unsigned char)ch == 127) {
            if (cursor > 0) {
                cursor--;
                for (int i = cursor; i < len - 1; ++i) out[i] = out[i + 1];
                len--;
                out[len] = 0;
                sh_redraw_line(prompt, out, len, cursor, 1);
            }
            continue;
        }

        if ((unsigned char)ch < 32) continue;

        if (cursor == len) {
            out[len++] = ch;
            cursor++;
        } else {
            for (int i = len; i > cursor; --i) out[i] = out[i - 1];
            out[cursor] = ch;
            len++;
            cursor++;
        }
        out[len] = 0;
        sh_redraw_line(prompt, out, len, cursor, 1);
    }

    out[len] = 0;
    sh_hist_add(out);
    return len;
}

static void sh_trim(char *s) {
    int i = 0;
    while (s[i] && sh_isspace(s[i])) ++i;
    if (i > 0) {
        int j = 0;
        while (s[i]) s[j++] = s[i++];
        s[j] = 0;
    }
    int n = (int)strlen(s);
    while (n > 0 && sh_isspace(s[n - 1])) s[--n] = 0;
}

static int sh_parse_argv(char *line, char **argv, int max_argv) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_argv) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = 0;
    }

    return argc;
}

static void sh_expand_vars(int *argc, char **argv) {
    for (int i = 0; i < *argc; ++i) {
        if (argv[i][0] == '$' && argv[i][1]) {
            const char *v = sh_env_get(argv[i] + 1);
            argv[i] = (char *)v;
        }
    }
}

static int sh_has_slash(const char *s) {
    while (*s) {
        if (*s == '/') return 1;
        ++s;
    }
    return 0;
}

static int sh_resolve_cmd(const char *cmd, char *out, int out_sz) {
    if (!cmd || !cmd[0] || !out || out_sz < 2) return -1;

    if (sh_has_slash(cmd)) {
        int n = (int)strlen(cmd);
        if (n + 1 > out_sz) return -1;
        strcpy(out, cmd);
        return 0;
    }

    if ((int)strlen(cmd) + 6 > out_sz) return -1;
    strcpy(out, "/bin/");
    strcat(out, cmd);
    return 0;
}

static int sh_builtin_help_to_buf(char *out, int out_sz) {
    int off = 0;
    sh_buf_append_str(out, out_sz, &off, "builtins: cd exit history env export pwd echo uname whoami hostname help\n");
    return 0;
}

static int sh_builtin_history_to_buf(char *out, int out_sz) {
    int off = 0;
    char nbuf[16];
    for (int i = 0; i < g_hist_count; ++i) {
        sh_itoa(i + 1, nbuf);
        if (sh_buf_append_str(out, out_sz, &off, nbuf) < 0) break;
        if (sh_buf_append_str(out, out_sz, &off, "  ") < 0) break;
        if (sh_buf_append_str(out, out_sz, &off, g_hist[i]) < 0) break;
        if (sh_buf_append_ch(out, out_sz, &off, '\n') < 0) break;
    }
    return 0;
}

static int sh_builtin_env_to_buf(char *out, int out_sz) {
    int off = 0;
    for (int i = 0; i < g_env_count; ++i) {
        if (sh_buf_append_str(out, out_sz, &off, g_env[i].key) < 0) break;
        if (sh_buf_append_ch(out, out_sz, &off, '=') < 0) break;
        if (sh_buf_append_str(out, out_sz, &off, g_env[i].val) < 0) break;
        if (sh_buf_append_ch(out, out_sz, &off, '\n') < 0) break;
    }
    return 0;
}

static int sh_builtin_simple_text(int argc, char **argv, char *out, int out_sz) {
    int off = 0;
    if (sh_streq(argv[0], "pwd")) {
        sh_buf_append_str(out, out_sz, &off, g_cwd);
        sh_buf_append_ch(out, out_sz, &off, '\n');
        return 0;
    }
    if (sh_streq(argv[0], "whoami")) {
        sh_buf_append_str(out, out_sz, &off, (geteuid() == 0) ? "root\n" : "user\n");
        return 0;
    }
    if (sh_streq(argv[0], "uname")) {
        if (argc > 1 && sh_streq(argv[1], "-a")) {
            char host[65];
            sh_get_hostname(host, sizeof(host));
            sh_buf_append_str(out, out_sz, &off, "EdgeOS ");
            sh_buf_append_str(out, out_sz, &off, host);
            sh_buf_append_str(out, out_sz, &off, " 0.2 x86_64 userspace\n");
        } else {
            sh_buf_append_str(out, out_sz, &off, "EdgeOS\n");
        }
        return 0;
    }
    if (sh_streq(argv[0], "echo")) {
        for (int i = 1; i < argc; ++i) {
            sh_buf_append_str(out, out_sz, &off, argv[i]);
            if (i + 1 < argc) sh_buf_append_ch(out, out_sz, &off, ' ');
        }
        sh_buf_append_ch(out, out_sz, &off, '\n');
        return 0;
    }
    if (sh_streq(argv[0], "help")) return sh_builtin_help_to_buf(out, out_sz);
    if (sh_streq(argv[0], "history")) return sh_builtin_history_to_buf(out, out_sz);
    if (sh_streq(argv[0], "env")) return sh_builtin_env_to_buf(out, out_sz);
    if (sh_streq(argv[0], "hostname")) {
        char host[65];
        if (argc > 2) {
            sh_buf_append_str(out, out_sz, &off, "usage: hostname [name]\n");
            return 0;
        }
        if (argc == 2) {
            if (sethostname(argv[1], strlen(argv[1])) < 0) {
                sh_buf_append_str(out, out_sz, &off, "hostname: failed\n");
                return 0;
            }
            sh_env_set("HOSTNAME", argv[1]);
            return 0;
        }
        sh_get_hostname(host, sizeof(host));
        sh_buf_append_str(out, out_sz, &off, host);
        sh_buf_append_ch(out, out_sz, &off, '\n');
        return 0;
    }
    return -1;
}

static int sh_builtin(int argc, char **argv, int redirect_mode, const char *redirect_path) {
    static char outbuf[8192];
    outbuf[0] = 0;

    if (argc <= 0) return 0;

    if (sh_streq(argv[0], "exit")) {
        exit(0);
        return 0;
    }

    if (sh_streq(argv[0], "cd")) {
        const char *path = argc < 2 ? "/" : argv[1];
        if (chdir(path) < 0) {
            printf("cd: %s: No such directory\n", path);
            return 1;
        }
        sh_refresh_cwd();
        return 0;
    }

    if (sh_streq(argv[0], "export")) {
        if (argc < 2) {
            printf("usage: export KEY=VALUE\n");
            return 1;
        }
        char *eq = 0;
        for (int i = 0; argv[1][i]; ++i) {
            if (argv[1][i] == '=') {
                eq = &argv[1][i];
                break;
            }
        }
        if (!eq) {
            printf("usage: export KEY=VALUE\n");
            return 1;
        }
        *eq = 0;
        sh_env_set(argv[1], eq + 1);
        return 0;
    }

    if (sh_builtin_simple_text(argc, argv, outbuf, sizeof(outbuf)) == 0) {
        if (redirect_mode) {
            if (sys_writefile(redirect_path, outbuf, strlen(outbuf)) < 0) {
                printf("sh: cannot write %s\n", redirect_path);
                return 1;
            }
        } else {
            sh_write(outbuf);
        }
        return 0;
    }

    return -1;
}

static int sh_run_external(int argc, char **argv) {
    char path[160];
    (void)argc;

    if (sh_resolve_cmd(argv[0], path, sizeof(path)) < 0) {
        printf("sh: command not found: %s\n", argv[0]);
        return 127;
    }

    sh_set_tty_editor_mode(0);
    int pid = spawn(path, argv);
    if (pid < 0) {
        sh_set_tty_editor_mode(1);
        printf("sh: command not found: %s\n", argv[0]);
        return 127;
    }

    int st = 0;
    if (wait(&st) < 0) {
        sh_setup_tty_for_line_editor();
        sh_set_tty_editor_mode(1);
        return 1;
    }
    sh_setup_tty_for_line_editor();
    sh_set_tty_editor_mode(1);
    return st;
}

static int sh_exec_one(char *segment, int piped_from_left) {
    char *args[SH_ARGV_MAX];
    char *redir = 0;

    sh_trim(segment);
    if (!segment[0]) return 0;

    for (int i = 0; segment[i]; ++i) {
        if (segment[i] == '>') {
            redir = &segment[i];
            break;
        }
    }

    if (redir) {
        *redir = 0;
        ++redir;
        sh_trim(segment);
        sh_trim(redir);
        if (!redir[0]) {
            printf("sh: redirection requires file\n");
            return 1;
        }
    }

    int argc = sh_parse_argv(segment, args, SH_ARGV_MAX - 1);
    if (argc <= 0) return 0;
    args[argc] = 0;

    sh_expand_vars(&argc, args);

    if (piped_from_left) {
        if (sh_streq(args[0], "cat") && argc == 1) {
            return 0;
        }
    }

    int brc = sh_builtin(argc, args, redir != 0, redir);
    if (brc >= 0) return brc;

    if (redir) {
        printf("sh: redirection for external commands is not supported yet\n");
        return 1;
    }

    return sh_run_external(argc, args);
}

static int sh_parse_chain(char *line, char segs[][SH_LINE_MAX], char ops[][3], int max_seg) {
    int n = 0;
    int start = 0;
    int i = 0;

    while (line[i] && n < max_seg - 1) {
        int oplen = 0;
        char op0 = 0;
        char op1 = 0;

        if (line[i] == '&' && line[i + 1] == '&') {
            oplen = 2;
            op0 = '&';
            op1 = '&';
        } else if (line[i] == '|' && line[i + 1] == '|') {
            oplen = 2;
            op0 = '|';
            op1 = '|';
        } else if (line[i] == '|') {
            oplen = 1;
            op0 = '|';
            op1 = 0;
        }

        if (oplen > 0) {
            int k = 0;
            for (int j = start; j < i && k < SH_LINE_MAX - 1; ++j) segs[n][k++] = line[j];
            segs[n][k] = 0;
            ops[n][0] = op0;
            ops[n][1] = op1;
            ops[n][2] = 0;
            ++n;
            i += oplen;
            start = i;
            continue;
        }

        ++i;
    }

    if (n < max_seg) {
        int k = 0;
        for (int j = start; line[j] && k < SH_LINE_MAX - 1; ++j) segs[n][k++] = line[j];
        segs[n][k] = 0;
        ops[n][0] = 0;
        ops[n][1] = 0;
        ops[n][2] = 0;
        ++n;
    }

    return n;
}

static int sh_run_line(char *line) {
    char segs[16][SH_LINE_MAX];
    char ops[16][3];
    int nseg = sh_parse_chain(line, segs, ops, 16);
    int status = 0;

    for (int i = 0; i < nseg; ++i) {
        int run = 1;
        int piped_from_left = 0;

        if (i > 0) {
            if (ops[i - 1][0] == '&' && ops[i - 1][1] == '&') run = (status == 0);
            else if (ops[i - 1][0] == '|' && ops[i - 1][1] == '|') run = (status != 0);
            else if (ops[i - 1][0] == '|') {
                run = 1;
                piped_from_left = 1;
            }
        }

        if (!run) continue;
        status = sh_exec_one(segs[i], piped_from_left);
    }

    return status;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    sh_env_set("USER", "root");
    sh_env_set("HOME", "/root");
    sh_env_set("PWD", "/root");
    {
        char host[65];
        sh_get_hostname(host, sizeof(host));
        sh_env_set("HOSTNAME", host);
    }

    if (chdir("/root") < 0) chdir("/");
    sh_refresh_cwd();
    sh_env_set("PWD", g_cwd);
    sh_setup_tty_for_line_editor();
    for (;;) {
        char line[SH_LINE_MAX];
        char prompt[192];

        sh_prompt(prompt, sizeof(prompt));
        if (sh_readline(prompt, line, sizeof(line)) < 0) exit(0);

        sh_trim(line);
        if (!line[0]) continue;

        sh_run_line(line);
        sh_refresh_cwd();
        sh_env_set("PWD", g_cwd);
    }

    return 0;
}
