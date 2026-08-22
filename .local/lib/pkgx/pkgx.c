/*
 * pkgx — modern, clean wrapper around Termux's pkg/apt.
 *
 * What it does:
 *   - collapses the mirror-testing spam into a compact live counter
 *   - suppresses noisy apt/dpkg lines, keeps errors & warnings visible
 *   - renders a real progress bar via APT::Status-Fd (fd 3)
 *   - prints a colorized summary at the end
 *
 * Build: clang -O2 -Wall -Wextra -o ~/bin/pkgx pkgx.c
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_DIM    "\033[2m"
#define C_RED    "\033[31m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_CYAN   "\033[36m"
#define C_GRAY   "\033[90m"

static int tty = 0;
static int line_active = 0;

static const char *g_cmd = "";

/* mirror state: 0 = none, 1 = full test, 2 = single current-mirror check */
static int mirror_state = 0;
static int mirror_ok = 0, mirror_bad = 0;
static int mirror_picked = 0;

static int get_count = 0;
static char fetched_rest[256] = "";
static char upgrade_rest[256] = "";

static int setup_count = 0, remove_count = 0;
static int saw_error = 0;

/* upgrade/install package list state: 0 none, 1 NEW, 2 UPGRADED, 3 REMOVED, 4 KEPT */
static int list_state = 0;
static char pkg_names[8192] = "";
static int pkg_count = 0;

/* summary numbers from "N upgraded, M newly installed, ..." */
static int sum_up = -1, sum_new = -1, sum_rem = -1, sum_kept = -1;
static char need_get[64] = "";
static char disk_use[64] = "";
static int summary_rendered = 0;

static char status_conf[512] = "";

static pid_t child_pid = 0;
static int assume_yes = 0;
static int assume_no = 0;
static int prompted = 0;

static void clear_line(void) {
    if (line_active && tty) {
        printf("\r\033[K");
        line_active = 0;
    }
}

static void print_line(const char *s, const char *color) {
    clear_line();
    if (color && tty)
        printf("%s%s%s\n", color, s, C_RESET);
    else
        printf("%s\n", s);
    fflush(stdout);
}

static void draw_mirror_counter(void) {
    if (!tty) return;
    printf("\r\033[K⟳ Testing mirrors: %d ok / %d bad", mirror_ok, mirror_bad);
    fflush(stdout);
    line_active = 1;
}

static void draw_bar(int percent, const char *label) {
    if (!tty) return;
    int w = 20;
    int filled = (percent * w) / 100;
    if (filled < 0) filled = 0;
    if (filled > w) filled = w;
    char bar[3 * 20 + 1];
    int i, o = 0;
    for (i = 0; i < filled; i++) { bar[o++] = 0xE2; bar[o++] = 0x96; bar[o++] = 0x88; }
    for (; i < w; i++) { bar[o++] = 0xE2; bar[o++] = 0x96; bar[o++] = 0x91; }
    bar[o] = 0;
    char short_label[64];
    snprintf(short_label, sizeof short_label, "%.48s", label);
    printf("\r\033[K⟳ [%s] %3d%% %s", bar, percent, short_label);
    fflush(stdout);
    line_active = 1;
}

static void handle_status_line(const char *line) {
    int is_dl = strncmp(line, "dlstatus:", 9) == 0;
    int is_pm = strncmp(line, "pmstatus:", 9) == 0;
    if (!is_dl && !is_pm) return;

    /* both formats: prefix:NAME:PERCENT:DESC
     *   dlstatus:1:0.0000:Retrieving file 1 of 4
     *   pmstatus:figlet:20.0000:Unpacking figlet (aarch64) */
    const char *p = line + 9;
    const char *c1 = strchr(p, ':');
    if (!c1) return;
    const char *name = c1 + 1;
    const char *c2 = strchr(name, ':');
    if (!c2) return;

    int percent = atoi(name);   /* "20.0000" -> 20 */
    const char *desc = c2 + 1;

    if (!strcmp(desc, "Running dpkg")) {
        clear_line();
        return;
    }

    draw_bar(percent, desc);
}

static void pkg_add(const char *name, size_t n) {
    size_t len = strlen(pkg_names);
    if (len + n + 2 >= sizeof pkg_names) return;
    if (len) pkg_names[len++] = ' ';
    memcpy(pkg_names + len, name, n);
    pkg_names[len + n] = 0;
    pkg_count++;
}

static void render_pkg_grid(void) {
    if (pkg_count == 0) return;
    const char *label, *color;
    switch (list_state) {
        case 1: label = "NEW"; color = C_GREEN; break;
        case 2: label = "UP";  color = C_YELLOW; break;
        case 3: label = "RM";  color = C_RED; break;
        default: label = "KEPT"; color = C_GRAY; break;
    }

    size_t maxlen = 0;
    char *copy = strdup(pkg_names);
    char *tok, *save = NULL;
    for (tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        size_t l = strlen(tok);
        if (l > maxlen) maxlen = l;
    }
    free(copy);

    size_t colw = maxlen + 2;
    int cols = 4;
    if (tty) {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 8) {
            int c = (ws.ws_col - 8) / (int)(colw + 1);
            if (c >= 1) cols = c;
        }
    }

    clear_line();
    int i = 0;
    char *copy2 = strdup(pkg_names);
    char *save2 = NULL;
    for (tok = strtok_r(copy2, " ", &save2); tok; tok = strtok_r(NULL, " ", &save2)) {
        if (i % cols == 0) {
            if (i > 0) printf("\n      ");
            else printf("  ");
            if (i == 0) {
                if (tty) printf("%s%-4s%s ", color, label, C_RESET);
                else printf("%-4s ", label);
            } else {
                printf(" ");
            }
        } else {
            printf(" ");
        }
        if (i % cols == cols - 1) {
            if (tty) printf("%s%s%s", C_BOLD, tok, C_RESET);
            else printf("%s", tok);
        } else {
            if (tty) printf("%s%-*s%s", C_BOLD, (int)colw, tok, C_RESET);
            else printf("%-*s", (int)colw, tok);
        }
        i++;
    }
    printf("\n");
    fflush(stdout);
    free(copy2);
}

static int cmd_is_mutating(void) {
    return strncmp(g_cmd, "in", 2) == 0 ||    /* install */
           strncmp(g_cmd, "up", 2) == 0 ||    /* update/upgrade/up */
           strncmp(g_cmd, "re", 2) == 0 ||    /* reinstall */
           strncmp(g_cmd, "rm", 2) == 0 ||    /* remove */
           strncmp(g_cmd, "un", 2) == 0 ||    /* uninstall */
           strncmp(g_cmd, "rem", 3) == 0;     /* remove */
}

static void cleanup(void);

/* apt never prompts when its stdout is a pipe, so pkgx asks on its behalf.
 * The child tree is frozen with SIGSTOP while we wait for the answer. */
static void confirm_proceed(void) {
    if (prompted) return;
    prompted = 1;
    if (assume_yes || assume_no || !tty || !isatty(STDIN_FILENO)) return;
    if (!cmd_is_mutating() || child_pid <= 0) return;

    kill(-child_pid, SIGSTOP);

    printf("Proceed? [Y/n] ");
    fflush(stdout);

    char ans[16] = "";
    if (fgets(ans, sizeof ans, stdin) && (ans[0] == 'n' || ans[0] == 'N')) {
        kill(-child_pid, SIGKILL);
        printf("✗ Aborted\n");
        int st;
        waitpid(child_pid, &st, 0);
        cleanup();
        exit(1);
    }

    kill(-child_pid, SIGCONT);
}

static void render_summary(void) {
    if (summary_rendered || sum_up < 0) return;
    summary_rendered = 1;
    if (sum_up == 0 && sum_new == 0 && sum_rem == 0) return;

    char buf[256];
    int n = 0;
    if (sum_up > 0) n += snprintf(buf + n, sizeof buf - n, "%d upgraded", sum_up);
    if (sum_new > 0) n += snprintf(buf + n, sizeof buf - n, "%s%d new", n ? " · " : "", sum_new);
    if (sum_rem > 0) n += snprintf(buf + n, sizeof buf - n, "%s%d removed", n ? " · " : "", sum_rem);
    if (need_get[0]) n += snprintf(buf + n, sizeof buf - n, "%s%s", n ? " · " : "", need_get);
    if (disk_use[0]) n += snprintf(buf + n, sizeof buf - n, "%s%s", n ? " · " : "", disk_use);
    if (n == 0) return;

    char out[300];
    snprintf(out, sizeof out, "✓ %s", buf);
    print_line(out, C_GREEN);
    confirm_proceed();
}

static void handle_stdout_line(const char *line) {
    /* --- mirror testing --- */
    if (strncmp(line, "[*]", 3) == 0) {
        const char *p = strrchr(line, ':');
        int ok = p && strstr(p, "ok");
        if (ok) mirror_ok++; else mirror_bad++;
        if (mirror_state == 2) {
            if (ok) print_line("✓ Current mirror ok", C_GREEN);
            else    print_line("✗ Current mirror bad", C_RED);
            mirror_state = 0;
        } else {
            draw_mirror_counter();
        }
        return;
    }
    if (strstr(line, "No mirror or mirror group selected")) {
        print_line(line, C_GRAY);
        return;
    }
    if (strstr(line, "Testing the available mirrors")) {
        mirror_state = 1;
        print_line("⟳ Testing mirrors...", C_DIM);
        return;
    }
    if (strstr(line, "Checking availability of current mirror")) {
        mirror_state = 2;
        return;
    }
    if (strncmp(line, "Picking mirror:", 15) == 0) {
        clear_line();
        if (mirror_ok + mirror_bad > 0) {
            char buf[128];
            snprintf(buf, sizeof buf, "✓ %d/%d mirrors ok", mirror_ok, mirror_ok + mirror_bad);
            print_line(buf, C_GREEN);
        }
        mirror_picked = 1;
        mirror_state = 0;
        return;
    }

    /* --- upgrade/install package lists --- */
    if (strstr(line, "Calculating upgrade")) {
        print_line("⟳ Calculating upgrade...", C_DIM);
        return;
    }
    if (list_state != 0) {
        if (line[0] == ' ' && line[1] == ' ') {
            const char *p = line;
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                const char *start = p;
                while (*p && *p != ' ') p++;
                pkg_add(start, (size_t)(p - start));
            }
            return;
        }
        render_pkg_grid();
        list_state = 0;
        /* fall through to header checks */
    }
    if (strstr(line, "The following NEW packages will be installed")) {
        list_state = 1; pkg_count = 0; pkg_names[0] = 0;
        return;
    }
    if (strstr(line, "The following packages will be upgraded")) {
        list_state = 2; pkg_count = 0; pkg_names[0] = 0;
        return;
    }
    if (strstr(line, "The following packages will be REMOVED")) {
        list_state = 3; pkg_count = 0; pkg_names[0] = 0;
        return;
    }
    if (strstr(line, "The following packages have been kept back")) {
        list_state = 4; pkg_count = 0; pkg_names[0] = 0;
        return;
    }

    /* stray blank lines from apt during update/upgrade */
    if (line[0] == '\0' &&
        (strncmp(g_cmd, "upd", 3) == 0 || strncmp(g_cmd, "upg", 3) == 0 || strcmp(g_cmd, "up") == 0))
        return;

    /* --- apt update noise --- */
    if (strncmp(line, "Get:", 4) == 0) { get_count++; return; }
    if (strncmp(line, "Hit:", 4) == 0 || strncmp(line, "Ign:", 4) == 0) return;
    if (strncmp(line, "Fetched ", 8) == 0) {
        snprintf(fetched_rest, sizeof fetched_rest, "%s", line + 8);
        return;
    }
    if (strstr(line, "packages can be upgraded")) {
        const char *p = strstr(line, "Run '");
        size_t n = strlen(line);
        if (p) n = (size_t)(p - line);
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '.')) n--;
        if (n >= sizeof upgrade_rest) n = sizeof upgrade_rest - 1;
        memcpy(upgrade_rest, line, n);
        upgrade_rest[n] = 0;
        return;
    }
    if (strstr(line, "apt does not have a stable CLI interface")) return;

    /* apt --dry-run simulation noise */
    if (strncmp(line, "Inst ", 5) == 0 || strncmp(line, "Conf ", 5) == 0) return;
    if (strstr(line, "This is only a simulation") ||
        strstr(line, "apt needs root privileges") ||
        strstr(line, "locking is deactivated") ||
        strstr(line, "don't depend on the relevance")) return;

    /* summary line: "42 upgraded, 12 newly installed, 0 to remove and 0 not upgraded." */
    {
        int a, b, c, d;
        if (sscanf(line, "%d upgraded, %d newly installed, %d to remove and %d not upgraded.", &a, &b, &c, &d) == 4 ||
            sscanf(line, "%d upgraded, %d reinstalled, %d to remove and %d not upgraded.", &a, &b, &c, &d) == 4) {
            sum_up = a; sum_new = b; sum_rem = c; sum_kept = d;
            return;
        }
    }
    if (strncmp(line, "Need to get ", 12) == 0) {
        /* "Need to get 0 B/89.8 kB of archives." — take the total after '/' */
        const char *start = line + 12;
        const char *end = strstr(start, " of archives.");
        if (end) {
            size_t n = (size_t)(end - start);
            const char *slash = NULL;
            for (const char *p = start; p < end; p++)
                if (*p == '/') slash = p;
            if (slash) { start = slash + 1; n = (size_t)(end - start); }
            while (n > 0 && start[n - 1] == ' ') n--;
            if (n >= sizeof need_get) n = sizeof need_get - 1;
            memcpy(need_get, start, n);
            need_get[n] = 0;
        }
        return;
    }
    if (strncmp(line, "After this operation, ", 22) == 0) {
        char sz[32], unit[16], verb[16];
        if (sscanf(line, "After this operation, %31[^ ] %15[^ ] of additional disk space will be %15[^.].", sz, unit, verb) == 3) {
            if (!strcmp(verb, "freed"))
                snprintf(disk_use, sizeof disk_use, "-%s %s disk", sz, unit);
            else
                snprintf(disk_use, sizeof disk_use, "+%s %s disk", sz, unit);
        }
        render_summary();
        return;
    }
    if (strstr(line, "Reading package lists") ||
        strstr(line, "Building dependency tree") ||
        strstr(line, "Reading state information") ||
        strstr(line, "Sorting...") ||
        strstr(line, "Full Text Search")) return;

    /* apt's own progress lines (shouldn't appear when piped, but just in case) */
    if (line[0] == '[' && strchr(line, '%') && strchr(line, ']')) return;

    /* --- dpkg noise --- */
    if (strstr(line, "(Reading database")) return;
    if (strstr(line, "Selecting previously unselected")) return;
    if (strncmp(line, "Preparing to unpack", 19) == 0) return;
    if (strncmp(line, "Unpacking ", 10) == 0) return;
    if (strncmp(line, "Setting up ", 11) == 0) { setup_count++; return; }
    if (strncmp(line, "Removing ", 9) == 0) { remove_count++; return; }
    if (strncmp(line, "Processing triggers", 19) == 0) return;
    if (strncmp(line, "dpkg: ", 6) == 0) {
        if (strstr(line, "error")) { saw_error = 1; print_line(line, C_RED); }
        else print_line(line, C_YELLOW);
        return;
    }
    if (strstr(line, "Errors were encountered while processing")) {
        saw_error = 1;
        print_line(line, C_RED);
        return;
    }

    /* --- apt errors / warnings --- */
    if (strncmp(line, "E:", 2) == 0 || strncmp(line, "Err:", 4) == 0) {
        saw_error = 1;
        print_line(line, C_RED);
        return;
    }
    if (strncmp(line, "W:", 2) == 0) {
        print_line(line, C_YELLOW);
        return;
    }

    /* --- search output: bold the package name --- */
    if (strncmp(g_cmd, "se", 2) == 0 && line[0] != ' ') {
        const char *slash = strchr(line, '/');
        if (slash) {
            size_t n = (size_t)(slash - line);
            if (n > 0 && n < 128) {
                char name[128];
                memcpy(name, line, n);
                name[n] = 0;
                clear_line();
                if (tty) printf("%s%s%s%s\n", C_BOLD, name, C_RESET, slash);
                else printf("%s\n", line);
                fflush(stdout);
                return;
            }
        }
    }

    print_line(line, NULL);
}

typedef struct {
    char buf[16384];
    size_t len;
} reader_t;

static void reader_feed(reader_t *r, const char *data, size_t n, void (*emit)(const char *)) {
    size_t i = 0;
    while (i < n) {
        const char *nl = memchr(data + i, '\n', n - i);
        if (!nl) break;
        size_t seg = (size_t)(nl - (data + i));
        if (r->len + seg < sizeof r->buf) {
            memcpy(r->buf + r->len, data + i, seg);
            r->buf[r->len + seg] = 0;
            emit(r->buf);
        }
        r->len = 0;
        i += seg + 1;
    }
    size_t rem = n - i;
    if (rem > 0 && r->len + rem < sizeof r->buf) {
        memcpy(r->buf + r->len, data + i, rem);
        r->len += rem;
    }
}

static void write_status_conf(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(status_conf, sizeof status_conf, "%s/pkgx-status-%d.conf", tmp, (int)getpid());
    int fd = open(status_conf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { status_conf[0] = 0; return; }
    /* apt 2.8+ always prompts for full-upgrade (Ask=true) and aborts on
     * non-tty stdin — force Assume-Yes/No so pkgx's own prompt is the
     * only confirmation */
    char content[256];
    int n = snprintf(content, sizeof content, "APT::Status-Fd \"3\";\n");
    if (assume_no)
        n += snprintf(content + n, sizeof content - n, "APT::Get::Assume-No \"true\";\n");
    else
        n += snprintf(content + n, sizeof content - n, "APT::Get::Assume-Yes \"true\";\n");
    (void)!write(fd, content, (size_t)n);
    close(fd);
    setenv("APT_CONFIG", status_conf, 1);
}

static void cleanup(void) {
    if (status_conf[0]) unlink(status_conf);
}

static void on_signal(int sig) {
    (void)sig;
    if (child_pid > 0) kill(-child_pid, SIGKILL);
    cleanup();
    _exit(128 + sig);
}

static void print_mirror_summary(void) {
    FILE *f = fopen("/data/data/com.termux/files/usr/etc/apt/sources.list", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "https://");
        if (p) p += 8;
        else {
            p = strstr(line, "http://");
            if (p) p += 7;
        }
        if (!p) continue;
        char *slash = strchr(p, '/');
        char *sp = strchr(p, ' ');
        char *end = slash;
        if (!end || (sp && sp < end)) end = sp;
        if (end && end > p) {
            size_t n = (size_t)(end - p);
            if (n >= 128) n = 127;
            char host[128];
            memcpy(host, p, n);
            host[n] = 0;
            char buf[256];
            snprintf(buf, sizeof buf, "✓ Mirror: %s", host);
            print_line(buf, C_CYAN);
            break;
        }
    }
    fclose(f);
}

static void usage(void) {
    printf("pkgx — modern wrapper for pkg/apt\n\n");
    printf("Usage: pkgx [--check-mirror] <command> [arguments]\n\n");
    printf("Commands (same as pkg):\n");
    printf("  update, upgrade, install, reinstall, remove, search,\n");
    printf("  show, files, list-all, list-installed, autoclean, clean\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }

    g_cmd = argv[1];
    if (!strcmp(g_cmd, "--check-mirror")) {
        if (argc < 3) { usage(); return 1; }
        g_cmd = argv[2];
    }

    tty = isatty(STDOUT_FILENO);
    if (getenv("NO_COLOR")) tty = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes") ||
            !strcmp(argv[i], "--assume-yes") || !strcmp(argv[i], "--force-yes"))
            assume_yes = 1;
        if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--assume-no"))
            assume_no = 1;
    }

    int outpipe[2], statuspipe[2];
    if (pipe(outpipe) < 0 || pipe(statuspipe) < 0) {
        perror("pipe");
        return 1;
    }

    write_status_conf();

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); cleanup(); return 1; }

    if (pid == 0) {
        /* own process group so the parent can SIGSTOP the whole pkg/apt tree */
        setpgid(0, 0);
        /* detach stdin from the terminal: a background group reading the tty
         * would get SIGTTIN and freeze; apt auto-continues on non-tty stdin
         * and pkgx asks for confirmation on its behalf */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        /* keep the status pipe write end on a safe high fd while shuffling
         * (outpipe[0] may occupy fd 3 and would clobber it) */
        int stw = fcntl(statuspipe[1], F_DUPFD, 10);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(outpipe[1], STDERR_FILENO);
        close(outpipe[0]); close(outpipe[1]);
        close(statuspipe[0]); close(statuspipe[1]);
        dup2(stw, 3);
        close(stw);

        char **args = malloc(sizeof(char *) * (size_t)(argc + 1));
        if (!args) _exit(127);
        args[0] = "pkg";
        for (int i = 1; i < argc; i++) args[i] = argv[i];
        args[argc] = NULL;
        execvp("pkg", args);
        perror("execvp pkg");
        _exit(127);
    }

    close(outpipe[1]);
    close(statuspipe[1]);
    child_pid = pid;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    reader_t out_r = {0}, st_r = {0};
    int out_open = 1, st_open = 1;

    while (out_open || st_open) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (out_open) { FD_SET(outpipe[0], &rfds); maxfd = outpipe[0]; }
        if (st_open) { FD_SET(statuspipe[0], &rfds); if (statuspipe[0] > maxfd) maxfd = statuspipe[0]; }

        int r = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (out_open && FD_ISSET(outpipe[0], &rfds)) {
            char buf[4096];
            ssize_t n = read(outpipe[0], buf, sizeof buf);
            if (n <= 0) { out_open = 0; close(outpipe[0]); }
            else reader_feed(&out_r, buf, (size_t)n, handle_stdout_line);
        }
        if (st_open && FD_ISSET(statuspipe[0], &rfds)) {
            char buf[4096];
            ssize_t n = read(statuspipe[0], buf, sizeof buf);
            if (n <= 0) { st_open = 0; close(statuspipe[0]); }
            else reader_feed(&st_r, buf, (size_t)n, handle_status_line);
        }
    }

    if (out_r.len) { out_r.buf[out_r.len] = 0; handle_stdout_line(out_r.buf); }
    if (st_r.len) { st_r.buf[st_r.len] = 0; handle_status_line(st_r.buf); }

    int status = 0;
    waitpid(pid, &status, 0);
    child_pid = 0;  /* reaped — no more prompting */
    cleanup();

    clear_line();

    int is_update = strncmp(g_cmd, "upd", 3) == 0;
    int is_upgrade = strncmp(g_cmd, "upg", 3) == 0 || strcmp(g_cmd, "up") == 0;

    if (mirror_picked) print_mirror_summary();
    render_summary();

    if ((is_update || is_upgrade) && get_count > 0) {
        char buf[256];
        if (fetched_rest[0])
            snprintf(buf, sizeof buf, "✓ %d repo files · %s", get_count, fetched_rest);
        else
            snprintf(buf, sizeof buf, "✓ %d repo files fetched", get_count);
        print_line(buf, C_GREEN);
    } else if (fetched_rest[0]) {
        char buf[256];
        snprintf(buf, sizeof buf, "✓ Downloaded %s", fetched_rest);
        print_line(buf, C_GREEN);
    }
    if (upgrade_rest[0] && !is_upgrade) {
        char buf[256];
        snprintf(buf, sizeof buf, "⚠ %s", upgrade_rest);
        print_line(buf, C_YELLOW);
    }
    if (setup_count > 0) {
        char buf[128];
        snprintf(buf, sizeof buf, "✓ %d package%s set up", setup_count, setup_count == 1 ? "" : "s");
        print_line(buf, C_GREEN);
    }
    if (remove_count > 0) {
        char buf[128];
        snprintf(buf, sizeof buf, "✓ %d package%s removed", remove_count, remove_count == 1 ? "" : "s");
        print_line(buf, C_GREEN);
    }

    int rc = 1;
    if (WIFEXITED(status)) rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) rc = 128 + WTERMSIG(status);

    if (rc != 0) {
        char buf[128];
        snprintf(buf, sizeof buf, "✗ Command failed (exit %d)", rc);
        print_line(buf, C_RED);
    } else if (saw_error) {
        print_line("⚠ Finished with errors", C_YELLOW);
    }

    return rc;
}