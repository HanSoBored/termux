/*
 * sysinfo — custom system info fetcher with full layout control
 * Usage: sysinfo
 * Compile: cc -O2 -o sysinfo sysinfo.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <pwd.h>
#include <libgen.h>

/* ─── ANSI colors ─── */
#define RST  "\033[0m"
#define BLD  "\033[1m"
#define CYN  "\033[1;36m"
#define WHT  "\033[1;37m"
#define MAG  "\033[1;35m"
#define GRN  "\033[1;32m"
#define YLW  "\033[1;33m"
#define RED  "\033[1;31m"
#define GRY  "\033[0;37m"
#define YEL  "\033[0;33m"
#define BLU  "\033[0;34m"

/* ─── Android logo (small, 6 lines) ─── */
static const char *logo[6] = {
    "   ;,           ,;",
    "    ';,.-----.,;'",
    "   ,'           ',",
    "  /    O     O    \\",
    " |                 |",
    " '-----------------'",
};
#define LOGO_W 24

/* ─── helpers ─── */

static char *read_line(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[1024];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NULL; }
    fclose(f);
    char *p = buf + strlen(buf);
    while (p > buf && isspace((unsigned char)p[-1])) *--p = '\0';
    return strdup(buf);
}

static char *popen_read(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char buf[1024];
    if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); return NULL; }
    pclose(fp);
    char *p = buf + strlen(buf);
    while (p > buf && isspace((unsigned char)p[-1])) *--p = '\0';
    return strdup(buf);
}

/* ─── system info gatherers ─── */

static char *get_user_host(void) {
    struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : "unknown";
    char host[256] = "localhost";
    gethostname(host, sizeof(host));
    char *ret;
    asprintf(&ret, "\033[1;37m\033[0m            %s\033[1;37m@\033[1;35m%s\033[0m", user, host);
    return ret;
}

static char *get_os(void) {
    char *rel = popen_read("getprop ro.build.version.release");
    char *abi = popen_read("getprop ro.product.cpu.abi");
    char *ret;
    asprintf(&ret, WHT "" RST "  " CYN "OS" RST "        Android REL %s %s",
             rel ? rel : "?", abi ? abi : "?");
    free(rel); free(abi);
    return ret;
}

static char *get_kernel(void) {
    struct utsname u;
    if (uname(&u) == 0) {
        char *ret;
        asprintf(&ret, WHT "" RST "  " CYN "Kernel" RST "    %s %s", u.sysname, u.release);
        return ret;
    }
    char *line = read_line("/proc/version");
    if (line) {
        char *ret;
        asprintf(&ret, WHT "" RST "  " CYN "Kernel" RST "    %s", line);
        free(line);
        return ret;
    }
    return strdup(WHT "" RST "  " CYN "Kernel" RST "    ?");
}

static char *get_uptime(void) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        long up = si.uptime;
        int days = up / 86400;
        int hours = (up % 86400) / 3600;
        int mins = (up % 3600) / 60;
        char *ret;
        if (days > 0)
            asprintf(&ret, WHT "" RST "  " CYN "Uptime" RST "    %d day%s, %d hours, %d mins",
                     days, days > 1 ? "s" : "", hours, mins);
        else
            asprintf(&ret, WHT "" RST "  " CYN "Uptime" RST "    %d hours, %d mins", hours, mins);
        return ret;
    }
    return strdup(WHT "" RST "  " CYN "Uptime" RST "    ?");
}

static char *get_packages(void) {
    char *count = popen_read(
        "dpkg --list 2>/dev/null | tail -n +6 | wc -l");
    if (count) {
        char *ret;
        char *p = count + strlen(count);
        while (p > count && isspace((unsigned char)p[-1])) *--p = '\0';
        asprintf(&ret, WHT "󰏗" RST "  " CYN "Packages" RST "  %s (dpkg)", count);
        free(count);
        return ret;
    }
    return strdup(WHT "󰏗" RST "  " CYN "Packages" RST "  ?");
}

static const char *pct_color(int pct) {
    if (pct >= 80) return RED;
    if (pct >= 50) return YLW;
    return GRN;
}

static char *get_memory(void) {
    long total_kb = 0, avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            long val;
            if (sscanf(line, "MemTotal: %ld kB", &val) == 1) total_kb = val;
            if (sscanf(line, "MemAvailable: %ld kB", &val) == 1) avail_kb = val;
        }
        fclose(f);
    }
    if (total_kb == 0) return strdup(WHT "" RST "  " CYN "Memory" RST "    ?");

    double total = total_kb / 1024.0 / 1024.0;
    double used = (total_kb - avail_kb) / 1024.0 / 1024.0;
    int pct = (int)((total_kb - avail_kb) * 100 / total_kb);
    const char *clr = pct_color(pct);

    char *ret;
    asprintf(&ret, WHT "" RST "  " CYN "Memory" RST "    %.2f GiB / %.2f GiB (%s%d%%" RST ")",
             used, total, clr, pct);
    return ret;
}

/* ─── hardware name lookup (fastfetch-compatible) ─── */
static const char *hw_lookup(const char *model) {
    if (!model) return NULL;
    /* Realme sdm670/sdm710 */
    if (strcmp(model, "RMX1851") == 0) return "realme 3 Pro";
    if (strcmp(model, "RMX1971") == 0) return "realme 5 Pro";
    /* Oppo */
    if (strcmp(model, "CPH1871") == 0) return "Oppo Find X";
    if (strcmp(model, "CPH1919") == 0) return "Oppo Reno";
    /* Xiaomi sdm710 */
    if (strcmp(model, "beryllium") == 0) return "Poco F1";
    if (strcmp(model, "equuleus") == 0) return "Mi 8 Pro";
    if (strcmp(model, "sirius") == 0) return "Mi 8 SE";
    return NULL;
}

static char *get_device(void) {
    char *model = popen_read("getprop ro.product.model");
    const char *pretty = hw_lookup(model);
    char *ret;
    if (pretty)
        asprintf(&ret, WHT "󰢌" RST "  " CYN "Device" RST "    %s (%s)", pretty, model);
    else if (model)
        asprintf(&ret, WHT "󰢌" RST "  " CYN "Device" RST "    %s", model);
    else
        asprintf(&ret, WHT "󰢌" RST "  " CYN "Device" RST "    ?");
    free(model);
    return ret;
}

static char *get_shell(void) {
    const char *shell_path = getenv("SHELL");
    if (!shell_path) shell_path = "/bin/sh";
    char *copy = strdup(shell_path);
    const char *name = basename(copy);
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "dpkg -s %s 2>/dev/null | grep Version | awk '{print $2}'", name);
    char *ver = popen_read(cmd);
    if (!ver) ver = strdup("");
    char *ret;
    asprintf(&ret, WHT "" RST "  " CYN "Shell" RST "     %s %s", name, ver);
    free(ver); free(copy);
    return ret;
}

static char *get_terminal(void) {
    char *ver = getenv("TERMUX_VERSION");
    if (ver) {
        char *ret;
        asprintf(&ret, WHT "" RST "  " CYN "Terminal" RST "  Termux %s", ver);
        return ret;
    }
    const char *name = getenv("TERM_PROGRAM");
    const char *termver = getenv("TERM_PROGRAM_VERSION");
    if (name) {
        char *ret;
        if (termver)
            asprintf(&ret, WHT "" RST "  " CYN "Terminal" RST "  %s %s", name, termver);
        else
            asprintf(&ret, WHT "" RST "  " CYN "Terminal" RST "  %s", name);
        return ret;
    }
    char *tty = ttyname(STDIN_FILENO);
    char *ret;
    asprintf(&ret, WHT "" RST "  " CYN "Terminal" RST "  %s", tty ? tty : "?");
    return ret;
}

static void print_cpu(void) {
    char *vendor = NULL;
    int big = 0, little = 0;
    double freq = 0, temp = 0;

    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strncmp(line, "CPU implementer", 15) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    int impl = (int)strtol(colon + 1, NULL, 16);
                    switch (impl) {
                        case 0x41: vendor = strdup("ARM"); break;
                        case 0x42: vendor = strdup("Broadcom"); break;
                        case 0x43: vendor = strdup("CAVIUM"); break;
                        case 0x44: vendor = strdup("DEC"); break;
                        case 0x4e: vendor = strdup("NVIDIA"); break;
                        case 0x51: vendor = strdup("Qualcomm"); break;
                        case 0x53: vendor = strdup("SAMSUNG"); break;
                        case 0x56: vendor = strdup("Marvell"); break;
                        case 0x61: vendor = strdup("Apple"); break;
                        case 0x69: vendor = strdup("Intel"); break;
                        case 0xc0: vendor = strdup("AMD"); break;
                        default:   vendor = strdup("Unknown");
                    }
                }
                break;
            }
        }
        fclose(cpuinfo);
    }
    if (!vendor) vendor = strdup("Unknown");

    for (int i = 0; i < 8; i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE *f = fopen(path, "r");
        if (f) {
            int val;
            if (fscanf(f, "%d", &val) == 1) {
                if (val > 2000000) big++;
                else if (val > 0) little++;
            }
            fclose(f);
        }
    }
    if (big == 0 && little == 0) { big = 2; little = 6; }

    int prio[] = {6, 7, 4, 5, 0, 1, 2, 3};
    for (int j = 0; j < 8; j++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", prio[j]);
        FILE *f = fopen(path, "r");
        if (f) {
            int val;
            if (fscanf(f, "%d", &val) == 1 && val > 0) {
                freq = val / 1000000.0;
                fclose(f);
                break;
            }
            fclose(f);
        }
    }
    if (freq == 0) freq = 2.21;

    for (int z = 0; z < 8; z++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        FILE *f = fopen(path, "r");
        if (f) {
            int t;
            if (fscanf(f, "%d", &t) == 1 && t > 0 && t < 120000) {
                temp = t / 1000.0;
                fclose(f);
                break;
            }
            fclose(f);
        }
    }

    const char *tclr = (temp >= 55) ? RED : (temp >= 40) ? YLW : GRN;
    printf(WHT "󰍛" RST "  " CYN "CPU" RST "       %s Technologies, Inc\n", vendor);
    printf("              └─ (%d+%d) @ %.2f GHz, %s%.1f°C" RST "\n",
           big, little, freq, tclr, temp);
    free(vendor);
}

static void print_storage(void) {
    char *pct_str = NULL;
    FILE *df = popen("df -h / 2>/dev/null | tail -1", "r");
    if (df) {
        char line[256];
        if (fgets(line, sizeof(line), df)) {
            char used[32] = "?", total[32] = "?", pct[16] = "?";
            sscanf(line, "%*s %31s %31s %*s %15s", total, used, pct);
            if (total[0] && used[0]) {
                char num[16];
                strncpy(num, pct, sizeof(num) - 1);
                num[sizeof(num) - 1] = '\0';
                size_t n = strlen(num);
                while (n > 0 && !isdigit((unsigned char)num[n-1])) num[--n] = '\0';
                const char *clr = pct_color(atoi(num));
                char *ret;
                asprintf(&ret, "%s / %s (%s%s" RST ")", used, total, clr, pct);
                pct_str = ret;
            }
        }
        pclose(df);
    }
    if (!pct_str) pct_str = strdup("? / ? (?)");
    printf(WHT "󰋊" RST "  " CYN "Storage" RST "   %s\n", pct_str);

    FILE *pm = fopen("/proc/mounts", "r");
    char *fs_line = NULL;
    if (pm) {
        char line[256];
        while (fgets(line, sizeof(line), pm)) {
            char mnt[256], fs[64], opts[64];
            if (sscanf(line, "%*s %255s %63s %63s", mnt, fs, opts) >= 3) {
                if (strcmp(mnt, "/") == 0) {
                    if (strstr(opts, "ro"))
                        asprintf(&fs_line, "              └─ %s [" YLW "Read-only" RST "]", fs);
                    else
                        asprintf(&fs_line, "              └─ %s", fs);
                    break;
                }
            }
        }
        fclose(pm);
    }
    if (!fs_line) fs_line = strdup("              └─ ext4");
    printf("%s\n", fs_line);
    free(fs_line); free(pct_str);
}

static void print_localip(void) {
    FILE *fp = popen(
        "ip -4 addr show 2>/dev/null | grep inet | awk '{print $2}'", "r");
    if (!fp) { printf(WHT "󰩟" RST "  " CYN "LAN IP" RST "    ?\n"); return; }
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line + strlen(line);
        while (p > line && isspace((unsigned char)p[-1])) *--p = '\0';
        if (line[0] && strncmp(line, "127.", 4) != 0) {
            int first = (count == 0);
            if (first)
                printf(WHT "󰩟" RST "  " CYN "LAN IP" RST "    %s", line);
            else
                printf("             %s", line);
            if (strstr(line, "/24"))
                printf(" *");
            printf("\n");
            count++;
        }
    }
    pclose(fp);
    if (count == 0) printf(WHT "󰩟" RST "  " CYN "LAN IP" RST "    ?\n");
}

/* ─── print side-by-side ─── */
static void print_side(const char *l, const char *info) {
    printf("%-*s%s\n", LOGO_W, l, info);
}

/* ─── Main ─── */

int main(void) {
    /* gather SYSTEM info */
    char *sys[6];
    sys[1] = get_user_host();
    sys[2] = get_os();
    sys[3] = get_kernel();
    sys[4] = get_uptime();
    sys[5] = get_packages();
    asprintf(&sys[0], "%s", RST);

    for (int i = 0; i < 6; i++) {
        printf("%s%-*s%s%s\n", GRN, LOGO_W, logo[i], RST, sys[i] ? sys[i] : "");
        free(sys[i]);
    }

    /* DEVICE */
    char *d1 = get_device();
    char *d2 = get_shell();
    char *d3 = get_terminal();
    printf("%s\n", d1);
    printf("%s\n", d2);
    printf("%s\n", d3);
    free(d1); free(d2); free(d3);

    /* HARDWARE */
    print_cpu();
    char *mem = get_memory();
    printf("%s\n", mem);
    free(mem);
    print_storage();
    print_localip();

    /* colors */
    printf("\n");
    const char *dots[] = {
        RED, GRN, YLW, BLU, MAG, CYN, GRY
    };
    for (int i = 0; i < 7; i++)
        printf("%s● " RST, dots[i]);
    printf("\n");
    fflush(stdout);

    return 0;
}
