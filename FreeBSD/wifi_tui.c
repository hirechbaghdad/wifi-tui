/*
 * wifi-tui - interactive ncurses Wi-Fi controller for FreeBSD.
 *
 * FreeBSD has no NetworkManager, so this port drives the base system directly:
 *
 *   wpa_cli(8)   scanning, association, saved network profiles
 *   ifconfig(8)  wireless interface discovery and IPv4 address/netmask
 *   netstat(1)   default gateway for the interface
 *   resolv.conf  DNS servers
 *   dhclient(8)  DHCP, only when the interface is still address-less
 *
 * When wpa_supplicant is not reachable the program degrades to a read-only
 * mode that lists the net80211 scan cache through "ifconfig <iface> list scan".
 *
 * Note: _POSIX_C_SOURCE is deliberately NOT defined. FreeBSD's <sys/cdefs.h>
 * already exposes POSIX 2008 by default, and defining it would set
 * __BSD_VISIBLE to 0 and hide BSD interfaces such as issetugid(2).
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_APS 512
#define MAX_FIELD 256
#define MAX_OUTPUT 65536
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define SCAN_SETTLE_MS 3000
#define ASSOC_TIMEOUT_MS 20000
#define DHCP_TIMEOUT_MS 12000
#define POLL_INTERVAL_MS 400

typedef enum {
    BACKEND_NONE = 0,   /* nothing usable was found */
    BACKEND_WPA,        /* wpa_supplicant control socket: scan and connect */
    BACKEND_IFCONFIG    /* net80211 scan cache only: list, but do not connect */
} Backend;

typedef struct {
    bool in_use;
    char ssid[MAX_FIELD];
    char bssid[24];
    char channel[16];
    char security[96];
    int signal;
    bool open;
    bool wep;
    bool sae_only;
    bool enterprise;
} AccessPoint;

typedef struct {
    char iface[64];
    char state[64];
    char connection[MAX_FIELD];
    char bssid[24];
    char ip[64];
    char netmask[64];
    char gateway[64];
    char dns[4][64];
    size_t dns_count;
} NetStatus;

typedef struct {
    AccessPoint aps[MAX_APS];
    size_t ap_count;
    int selected;
    int scroll;
    int focus;
    char message[512];
    bool message_error;
    char notice[256];   /* startup warning, re-shown after the first scan */
    Backend backend;
    NetStatus status;
} App;

/* Everything needed to turn one access point into a wpa_supplicant network. */
typedef struct {
    char ssid[MAX_FIELD];   /* wpa_supplicant text encoding, quoted when used */
    bool hidden;
    bool open;
    bool wep;
    bool sae_only;
} ConnectRequest;

static volatile sig_atomic_t resized = 0;

static void draw(App *app);

static void on_resize(int sig)
{
    (void)sig;
    resized = 1;
}

/* ------------------------------------------------------------------ *
 * Subprocess plumbing
 * ------------------------------------------------------------------ */

static int run_command_input(char *const argv[], const char *input,
                             char *output, size_t capacity)
{
    int pipefd[2];
    int inputfd[2] = {-1, -1};
    if (pipe(pipefd) < 0) {
        snprintf(output, capacity, "pipe: %s", strerror(errno));
        return -1;
    }
    if (input && pipe(inputfd) < 0) {
        snprintf(output, capacity, "pipe: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(output, capacity, "fork: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        if (input) {
            close(inputfd[0]);
            close(inputfd[1]);
        }
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (input) {
            close(inputfd[1]);
            if (dup2(inputfd[0], STDIN_FILENO) < 0) _exit(126);
            close(inputfd[0]);
        }
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(pipefd[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    close(pipefd[1]);
    if (input) {
        close(inputfd[0]);
        size_t remaining = strlen(input);
        const char *cursor = input;
        while (remaining > 0) {
            ssize_t count = write(inputfd[1], cursor, remaining);
            if (count > 0) {
                cursor += count;
                remaining -= (size_t)count;
            } else if (count < 0 && errno != EINTR) {
                break;
            }
        }
        close(inputfd[1]);
    }
    size_t used = 0;
    while (used + 1 < capacity) {
        ssize_t count = read(pipefd[0], output + used, capacity - used - 1);
        if (count > 0) {
            used += (size_t)count;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    output[used] = '\0';
    char discard[512];
    while (read(pipefd[0], discard, sizeof(discard)) > 0) {
    }
    close(pipefd[0]);

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static int run_command(char *const argv[], char *output, size_t capacity)
{
    return run_command_input(argv, NULL, output, capacity);
}

static void nap(int milliseconds)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(milliseconds / 1000);
    ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ *
 * String helpers
 * ------------------------------------------------------------------ */

static void trim_newlines(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0) return;
    size_t count = strlen(source);
    if (count >= capacity) count = capacity - 1;
    memcpy(destination, source, count);
    destination[count] = '\0';
}

static size_t split_ws(const char *line, char fields[][MAX_FIELD], size_t field_count)
{
    size_t item = 0;
    const char *p = line;
    memset(fields, 0, field_count * MAX_FIELD);

    while (item < field_count) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r') break;
        size_t pos = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            if (pos + 1 < MAX_FIELD) fields[item][pos++] = *p;
            p++;
        }
        fields[item][pos] = '\0';
        item++;
    }
    return item;
}

/* Split on tabs; the final field keeps any remaining tabs verbatim so that a
   trailing SSID column is never cut short. */
static size_t split_tabs(const char *line, char fields[][MAX_FIELD], size_t field_count)
{
    size_t item = 0;
    size_t pos = 0;
    memset(fields, 0, field_count * MAX_FIELD);

    for (const char *p = line;; p++) {
        if (*p == '\0') {
            fields[item][pos] = '\0';
            item++;
            break;
        }
        if (*p == '\t' && item + 1 < field_count) {
            fields[item][pos] = '\0';
            item++;
            pos = 0;
        } else if (pos + 1 < MAX_FIELD) {
            fields[item][pos++] = *p;
        }
    }
    return item;
}

/* True when some line of wpa_cli output is exactly "OK", ignoring the "> "
   prompts that interactive mode interleaves with the replies. */
static bool output_has_word(const char *text, const char *word)
{
    size_t wlen = strlen(word);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *start = line;
        while (len > 0 && (*start == '>' || *start == ' ' || *start == '\t')) {
            start++;
            len--;
        }
        while (len > 0 && (start[len - 1] == '\r' || start[len - 1] == ' ')) len--;
        if (len == wlen && strncmp(start, word, wlen) == 0) return true;
        line = end ? end + 1 : NULL;
    }
    return false;
}

/* Last non-empty line; wpa_cli sometimes prefixes replies with a banner. */
static const char *last_line(const char *text)
{
    const char *best = text;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        if (*line != '\n' && *line != '\r') best = line;
        line = end ? end + 1 : NULL;
    }
    return best;
}

static void set_message(App *app, bool error, const char *text)
{
    snprintf(app->message, sizeof(app->message), "%s", text && *text ? text : "Done");
    trim_newlines(app->message);
    app->message_error = error;
}

/* Show progress during the blocking association/DHCP waits. */
static void progress(App *app, const char *text)
{
    set_message(app, false, text);
    draw(app);
}

/* ------------------------------------------------------------------ *
 * Environment overrides (ignored for set-user-ID execution)
 * ------------------------------------------------------------------ */

static const char *env_override(const char *name)
{
    if (issetugid()) return NULL;
    const char *value = getenv(name);
    return (value && *value) ? value : NULL;
}

static const char *resolv_conf_path(void)
{
    const char *path = env_override("WIFI_TUI_RESOLV_CONF");
    return path ? path : "/etc/resolv.conf";
}

static const char *wpa_ctrl_dir(void)
{
    return env_override("WIFI_TUI_WPA_CTRL");
}

/* ------------------------------------------------------------------ *
 * wpa_cli wrappers
 * ------------------------------------------------------------------ */

/* Build "wpa_cli [-p dir] -i iface <args...>" and run it. */
static int wpa_vrun(App *app, const char *input, char *output, size_t capacity,
                    va_list ap)
{
    char *argv[24];
    size_t n = 0;

    argv[n++] = "wpa_cli";
    const char *ctrl = wpa_ctrl_dir();
    if (ctrl) {
        argv[n++] = "-p";
        argv[n++] = (char *)ctrl;
    }
    argv[n++] = "-i";
    argv[n++] = app->status.iface;

    char *arg;
    while ((arg = va_arg(ap, char *)) != NULL && n + 1 < ARRAY_LEN(argv)) {
        argv[n++] = arg;
    }
    argv[n] = NULL;
    return run_command_input(argv, input, output, capacity);
}

static int wpa_cmd(App *app, char *output, size_t capacity, ...)
{
    va_list ap;
    va_start(ap, capacity);
    int rc = wpa_vrun(app, NULL, output, capacity, ap);
    va_end(ap);
    return rc;
}

/* Feed commands to wpa_cli's interactive mode over stdin. Used for anything
   carrying a secret so that the passphrase never appears in the argument
   vector, where ps(1) would expose it to every user on the machine. */
static int wpa_script(App *app, const char *input, char *output, size_t capacity)
{
    char *argv[8];
    size_t n = 0;

    argv[n++] = "wpa_cli";
    const char *ctrl = wpa_ctrl_dir();
    if (ctrl) {
        argv[n++] = "-p";
        argv[n++] = (char *)ctrl;
    }
    argv[n++] = "-i";
    argv[n++] = app->status.iface;
    argv[n] = NULL;
    return run_command_input(argv, input, output, capacity);
}

/* set_network <id> <variable> <value> for values that are not secret. */
static bool wpa_set(App *app, const char *id, const char *variable, const char *value)
{
    char output[4096];
    int rc = wpa_cmd(app, output, sizeof(output), "set_network", (char *)id,
                     (char *)variable, (char *)value, (char *)NULL);
    return rc == 0 && output_has_word(output, "OK");
}

/* Wrap a raw string as a quoted wpa_supplicant value, escaping the two
   characters the configuration parser treats specially. */
static bool wpa_quote_raw(char *dst, size_t capacity, const char *src)
{
    size_t n = 0;
    if (capacity < 3) return false;
    dst[n++] = '"';
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;
        size_t need = (c == '"' || c == '\\') ? 2u : 1u;
        if (n + need + 2 > capacity) return false;
        if (need == 2) dst[n++] = '\\';
        dst[n++] = (char)c;
    }
    dst[n++] = '"';
    dst[n] = '\0';
    return true;
}

/* SSIDs read back from wpa_cli are already in wpa_supplicant's text encoding
   (backslash escapes for quotes, backslashes and non-printables), so they only
   need the surrounding quotes -- escaping them again would corrupt them. */
static bool wpa_quote_encoded(char *dst, size_t capacity, const char *src)
{
    size_t len = strlen(src);
    size_t trailing = 0;
    if (len + 3 > capacity) return false;
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;
    }
    while (trailing < len && src[len - 1 - trailing] == '\\') trailing++;
    if (trailing % 2) return false;  /* dangling escape would eat the quote */
    snprintf(dst, capacity, "\"%s\"", src);
    return true;
}

static bool is_hex_key(const char *text, size_t want)
{
    if (strlen(text) != want) return false;
    for (const char *p = text; *p; p++) {
        if (!isxdigit((unsigned char)*p)) return false;
    }
    return true;
}

/* set_network for values that must not be visible in the process table. */
static bool wpa_set_secret(App *app, const char *id, const char *variable,
                           const char *secret)
{
    char value[MAX_FIELD * 2 + 8];
    bool raw = (strcmp(variable, "psk") == 0 && is_hex_key(secret, 64)) ||
               (strcmp(variable, "wep_key0") == 0 &&
                (is_hex_key(secret, 10) || is_hex_key(secret, 26)));

    if (raw) {
        copy_text(value, sizeof(value), secret);
    } else if (!wpa_quote_raw(value, sizeof(value), secret)) {
        return false;
    }

    char script[sizeof(value) + 64];
    int written = snprintf(script, sizeof(script), "set_network %s %s %s\nquit\n",
                           id, variable, value);
    explicit_bzero(value, sizeof(value));
    if (written < 0 || (size_t)written >= sizeof(script)) {
        explicit_bzero(script, sizeof(script));
        return false;
    }

    char out[4096];
    int rc = wpa_script(app, script, out, sizeof(out));
    explicit_bzero(script, sizeof(script));
    bool ok = rc == 0 && output_has_word(out, "OK") && !output_has_word(out, "FAIL");
    explicit_bzero(out, sizeof(out));
    return ok;
}

/* ------------------------------------------------------------------ *
 * ifconfig / netstat / resolv.conf
 * ------------------------------------------------------------------ */

static bool looks_like_bssid(const char *p)
{
    for (int i = 0; i < 17; i++) {
        if ((i % 3) == 2) {
            if (p[i] != ':') return false;
        } else if (!isxdigit((unsigned char)p[i])) {
            return false;
        }
    }
    return true;
}

static const char *find_bssid(const char *line)
{
    size_t len = strlen(line);
    if (len < 17) return NULL;
    for (size_t i = 0; i + 17 <= len; i++) {
        if (looks_like_bssid(line + i)) return line + i;
    }
    return NULL;
}

/* ifconfig prints IPv4 netmasks as 0xffffff00 unless told otherwise. */
static void netmask_to_dotted(const char *text, char *out, size_t capacity)
{
    if (strchr(text, '.')) {
        copy_text(out, capacity, text);
        return;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || (end && *end != '\0')) {
        snprintf(out, capacity, "-");
        return;
    }
    struct in_addr addr;
    addr.s_addr = htonl((uint32_t)value);
    if (!inet_ntop(AF_INET, &addr, out, (socklen_t)capacity)) {
        snprintf(out, capacity, "-");
    }
}

/* Read the value of "ssid" out of an ifconfig line. The line is indented with
   a tab, so anchor on a whitespace-delimited "ssid" token rather than a plain
   substring. ifconfig only quotes the SSID when it contains whitespace. */
static bool ifconfig_ssid(const char *line, char *out, size_t capacity)
{
    const char *p = NULL;
    for (const char *scan = line; (scan = strstr(scan, "ssid")) != NULL; scan += 4) {
        bool at_boundary = scan == line || scan[-1] == ' ' || scan[-1] == '\t';
        if (at_boundary && (scan[4] == ' ' || scan[4] == '\t')) {
            p = scan + 4;
            break;
        }
    }
    if (!p) return false;
    while (*p == ' ' || *p == '\t') p++;

    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (n + 1 < capacity) out[n++] = *p;
            p++;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (n + 1 < capacity) out[n++] = *p;
            p++;
        }
    }
    out[n] = '\0';
    return n > 0;
}

/* 0 = not wireless, 1 = wireless, 2 = wireless and currently associated. */
static int probe_interface(const char *name)
{
    char output[MAX_OUTPUT];
    char *argv[] = {"ifconfig", (char *)name, NULL};
    if (run_command(argv, output, sizeof(output)) != 0) return 0;
    if (!strstr(output, "IEEE 802.11") && !strstr(output, "\tssid ")) return 0;
    return strstr(output, "status: associated") ? 2 : 1;
}

static bool find_wifi_interface(App *app)
{
    char output[MAX_OUTPUT];
    char candidates[32][MAX_FIELD];
    size_t count = 0;

    /* FreeBSD puts every cloned wlan(4) device into the "wlan" group. */
    char *grouped[] = {"ifconfig", "-g", "wlan", NULL};
    if (run_command(grouped, output, sizeof(output)) == 0) {
        count = split_ws(output, candidates, ARRAY_LEN(candidates));
    }
    /* Older setups may not use the group, so fall back to every interface. */
    if (count == 0 || !candidates[0][0]) {
        char *listed[] = {"ifconfig", "-l", NULL};
        if (run_command(listed, output, sizeof(output)) != 0) {
            set_message(app, true, "Unable to run ifconfig");
            return false;
        }
        count = split_ws(output, candidates, ARRAY_LEN(candidates));
    }

    char fallback[sizeof(app->status.iface)] = "";
    for (size_t i = 0; i < count; i++) {
        if (!candidates[i][0]) continue;
        int kind = probe_interface(candidates[i]);
        if (kind == 2) {
            copy_text(app->status.iface, sizeof(app->status.iface), candidates[i]);
            return true;
        }
        if (kind == 1 && !fallback[0]) {
            copy_text(fallback, sizeof(fallback), candidates[i]);
        }
    }
    if (fallback[0]) {
        copy_text(app->status.iface, sizeof(app->status.iface), fallback);
        return true;
    }

    set_message(app, true,
                "No 802.11 interface found; create one with "
                "ifconfig wlan create wlandev <device>");
    return false;
}

static void read_gateway(App *app)
{
    char output[MAX_OUTPUT];
    char *argv[] = {"netstat", "-rn", "-f", "inet", NULL};
    if (run_command(argv, output, sizeof(output)) != 0) return;

    char fallback[sizeof(app->status.gateway)] = "";
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char fields[5][MAX_FIELD];
        size_t count = split_ws(line, fields, ARRAY_LEN(fields));
        if (count < 2 || strcmp(fields[0], "default") != 0) continue;
        if (count >= 4 && strcmp(fields[3], app->status.iface) == 0) {
            copy_text(app->status.gateway, sizeof(app->status.gateway), fields[1]);
            return;
        }
        if (!fallback[0]) copy_text(fallback, sizeof(fallback), fields[1]);
    }
    if (fallback[0]) copy_text(app->status.gateway, sizeof(app->status.gateway), fallback);
}

static void read_dns(App *app)
{
    FILE *file = fopen(resolv_conf_path(), "r");
    if (!file) return;
    char line[512];
    while (fgets(line, sizeof(line), file) &&
           app->status.dns_count < ARRAY_LEN(app->status.dns)) {
        char fields[2][MAX_FIELD];
        if (split_ws(line, fields, ARRAY_LEN(fields)) < 2) continue;
        if (strcmp(fields[0], "nameserver") != 0) continue;
        copy_text(app->status.dns[app->status.dns_count++],
                  sizeof(app->status.dns[0]), fields[1]);
    }
    fclose(file);
}

/* wpa_supplicant knows the association state more precisely than ifconfig. */
static void read_wpa_status(App *app)
{
    char output[MAX_OUTPUT];
    if (wpa_cmd(app, output, sizeof(output), "status", (char *)NULL) != 0) return;

    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *value = eq + 1;
        if (strcmp(key, "wpa_state") == 0) {
            copy_text(app->status.state, sizeof(app->status.state), value);
        } else if (strcmp(key, "ssid") == 0) {
            copy_text(app->status.connection, sizeof(app->status.connection), value);
        } else if (strcmp(key, "bssid") == 0) {
            copy_text(app->status.bssid, sizeof(app->status.bssid), value);
        }
    }
}

static void refresh_status(App *app)
{
    char iface[sizeof(app->status.iface)];
    snprintf(iface, sizeof(iface), "%s", app->status.iface);
    memset(&app->status, 0, sizeof(app->status));
    snprintf(app->status.iface, sizeof(app->status.iface), "%s", iface);
    snprintf(app->status.state, sizeof(app->status.state), "unknown");
    snprintf(app->status.connection, sizeof(app->status.connection), "-");
    snprintf(app->status.ip, sizeof(app->status.ip), "-");
    snprintf(app->status.netmask, sizeof(app->status.netmask), "-");
    snprintf(app->status.gateway, sizeof(app->status.gateway), "-");

    if (!app->status.iface[0] && !find_wifi_interface(app)) {
        return;
    }

    char output[MAX_OUTPUT];
    char *argv[] = {"ifconfig", app->status.iface, NULL};
    if (run_command(argv, output, sizeof(output)) == 0) {
        bool have_address = false;
        char *save = NULL;
        for (char *line = strtok_r(output, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char fields[8][MAX_FIELD];
            size_t count = split_ws(line, fields, ARRAY_LEN(fields));
            if (count == 0) continue;

            if (!have_address && strcmp(fields[0], "inet") == 0 && count >= 2) {
                copy_text(app->status.ip, sizeof(app->status.ip), fields[1]);
                for (size_t i = 2; i + 1 < count; i++) {
                    if (strcmp(fields[i], "netmask") == 0) {
                        netmask_to_dotted(fields[i + 1], app->status.netmask,
                                          sizeof(app->status.netmask));
                        break;
                    }
                }
                have_address = true;
            } else if (strcmp(fields[0], "status:") == 0 && count >= 2) {
                copy_text(app->status.state, sizeof(app->status.state), fields[1]);
            }
            char ssid[MAX_FIELD];
            if (ifconfig_ssid(line, ssid, sizeof(ssid))) {
                copy_text(app->status.connection, sizeof(app->status.connection), ssid);
            }
        }
    }

    if (app->backend == BACKEND_WPA) {
        read_wpa_status(app);
    }
    read_gateway(app);
    read_dns(app);
}

/* ------------------------------------------------------------------ *
 * Scanning
 * ------------------------------------------------------------------ */

static int frequency_to_channel(int frequency)
{
    if (frequency == 2484) return 14;
    if (frequency >= 2412 && frequency <= 2472) return (frequency - 2407) / 5;
    if (frequency >= 4900 && frequency < 5000) return (frequency - 4000) / 5;
    if (frequency >= 5000 && frequency <= 5895) return (frequency - 5000) / 5;
    if (frequency >= 5955 && frequency <= 7115) return (frequency - 5955) / 5 + 1;
    return 0;
}

static int dbm_to_percent(int dbm)
{
    if (dbm > 0) return dbm > 100 ? 100 : dbm;  /* already a quality value */
    if (dbm <= -100) return 0;
    if (dbm >= -50) return 100;
    return 2 * (dbm + 100);
}

static void append_token(char *buffer, size_t capacity, const char *token)
{
    size_t len = strlen(buffer);
    if (len == 0) {
        copy_text(buffer, capacity, token);
    } else if (len + 1 + strlen(token) < capacity) {
        buffer[len] = ' ';
        copy_text(buffer + len + 1, capacity - len - 1, token);
    }
}

/* Turn the wpa_supplicant flags column, e.g. "[WPA2-PSK-CCMP][WPS][ESS]",
   into something short enough for the table. */
static void describe_security(const char *flags, AccessPoint *ap)
{
    bool wpa1 = strstr(flags, "[WPA-") != NULL;
    bool wpa2 = strstr(flags, "[WPA2-") != NULL || strstr(flags, "[RSN-") != NULL;
    bool sae = strstr(flags, "SAE") != NULL;
    bool eap = strstr(flags, "EAP") != NULL;
    bool wep = strstr(flags, "[WEP") != NULL;

    ap->security[0] = '\0';
    if (wep) append_token(ap->security, sizeof(ap->security), "WEP");
    if (wpa1) append_token(ap->security, sizeof(ap->security), "WPA");
    if (wpa2 && !sae) append_token(ap->security, sizeof(ap->security), "WPA2");
    if (wpa2 && sae) append_token(ap->security, sizeof(ap->security), "WPA2/3");
    if (sae && !wpa2) append_token(ap->security, sizeof(ap->security), "WPA3");
    if (eap) append_token(ap->security, sizeof(ap->security), "802.1X");
    if (!ap->security[0]) copy_text(ap->security, sizeof(ap->security), "Open");

    ap->enterprise = eap;
    ap->open = !wep && !wpa1 && !wpa2 && !sae && !eap;
    ap->wep = wep && !wpa1 && !wpa2 && !sae;
    ap->sae_only = sae && !wpa1 && !wpa2;
}

/* Keep the strongest entry per SSID; a busy band shows the same network on
   several BSSIDs and the list is more useful collapsed. */
static void add_access_point(App *app, const AccessPoint *candidate)
{
    if (candidate->ssid[0] && strcmp(candidate->ssid, "<hidden>") != 0) {
        for (size_t i = 0; i < app->ap_count; i++) {
            if (strcmp(app->aps[i].ssid, candidate->ssid) != 0) continue;
            if (candidate->signal > app->aps[i].signal) app->aps[i] = *candidate;
            return;
        }
    }
    if (app->ap_count < MAX_APS) app->aps[app->ap_count++] = *candidate;
}

static int compare_aps(const void *left, const void *right)
{
    const AccessPoint *a = left;
    const AccessPoint *b = right;
    if (a->in_use != b->in_use) {
        return b->in_use - a->in_use;
    }
    return b->signal - a->signal;
}

static void mark_current(App *app)
{
    for (size_t i = 0; i < app->ap_count; i++) {
        AccessPoint *ap = &app->aps[i];
        ap->in_use = false;
        if (app->status.bssid[0] && ap->bssid[0] &&
            strcasecmp(ap->bssid, app->status.bssid) == 0) {
            ap->in_use = true;
        } else if (app->status.connection[0] &&
                   strcmp(app->status.connection, "-") != 0 &&
                   strcmp(ap->ssid, app->status.connection) == 0) {
            ap->in_use = true;
        }
    }
}

static bool scan_via_wpa(App *app, bool request_rescan)
{
    char output[MAX_OUTPUT];

    if (request_rescan) {
        /* FAIL-BUSY just means a scan is already under way, which is fine. */
        if (wpa_cmd(app, output, sizeof(output), "scan", (char *)NULL) != 0) {
            set_message(app, true, output[0] ? output : "wpa_cli scan failed");
            return false;
        }
        nap(SCAN_SETTLE_MS);
    }

    if (wpa_cmd(app, output, sizeof(output), "scan_results", (char *)NULL) != 0) {
        set_message(app, true, output[0] ? output : "Unable to read scan results");
        return false;
    }

    app->ap_count = 0;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "bssid", 5) == 0) continue;  /* column header */
        char fields[5][MAX_FIELD];
        if (split_tabs(line, fields, ARRAY_LEN(fields)) < 5) continue;
        if (!looks_like_bssid(fields[0])) continue;

        AccessPoint ap;
        memset(&ap, 0, sizeof(ap));
        copy_text(ap.bssid, sizeof(ap.bssid), fields[0]);
        copy_text(ap.ssid, sizeof(ap.ssid), fields[4][0] ? fields[4] : "<hidden>");
        snprintf(ap.channel, sizeof(ap.channel), "%d",
                 frequency_to_channel(atoi(fields[1])));
        ap.signal = dbm_to_percent(atoi(fields[2]));
        describe_security(fields[3], &ap);
        add_access_point(app, &ap);
    }
    return true;
}

/* Read-only path: parse "ifconfig <iface> list scan". The SSID column is
   fixed width and may contain spaces, so anchor on the BSSID instead. */
static bool scan_via_ifconfig(App *app, bool request_rescan)
{
    char output[MAX_OUTPUT];

    if (request_rescan) {
        char *rescan[] = {"ifconfig", app->status.iface, "scan", NULL};
        if (run_command(rescan, output, sizeof(output)) != 0) {
            set_message(app, true,
                        output[0] ? output
                                  : "Triggering a scan needs root; showing the cache");
        }
    }

    char *argv[] = {"ifconfig", "-v", app->status.iface, "list", "scan", NULL};
    if (run_command(argv, output, sizeof(output)) != 0) {
        set_message(app, true, output[0] ? output : "Unable to list the scan cache");
        return false;
    }

    app->ap_count = 0;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        const char *mac = find_bssid(line);
        if (!mac || mac == line) continue;

        AccessPoint ap;
        memset(&ap, 0, sizeof(ap));
        memcpy(ap.bssid, mac, 17);
        ap.bssid[17] = '\0';

        size_t ssid_len = (size_t)(mac - line);
        while (ssid_len > 0 && (line[ssid_len - 1] == ' ' || line[ssid_len - 1] == '\t')) {
            ssid_len--;
        }
        if (ssid_len >= sizeof(ap.ssid)) ssid_len = sizeof(ap.ssid) - 1;
        memcpy(ap.ssid, line, ssid_len);
        ap.ssid[ssid_len] = '\0';
        if (!ap.ssid[0]) copy_text(ap.ssid, sizeof(ap.ssid), "<hidden>");

        const char *rest = mac + 17;
        char fields[5][MAX_FIELD];
        size_t count = split_ws(rest, fields, ARRAY_LEN(fields));
        if (count >= 1) copy_text(ap.channel, sizeof(ap.channel), fields[0]);
        if (count >= 3) ap.signal = dbm_to_percent(atoi(fields[2]));

        bool rsn = strstr(rest, "RSN") != NULL;
        bool wpa = strstr(rest, "WPA") != NULL;
        bool privacy = count >= 5 && strchr(fields[4], 'P') != NULL;
        ap.open = !rsn && !wpa && !privacy;
        ap.wep = privacy && !rsn && !wpa;
        copy_text(ap.security, sizeof(ap.security),
                  rsn && wpa ? "WPA/WPA2" : rsn ? "WPA2" : wpa ? "WPA" :
                  privacy ? "WEP" : "Open");
        add_access_point(app, &ap);
    }
    return true;
}

static bool scan_networks(App *app, bool request_rescan)
{
    if (!app->status.iface[0] && !find_wifi_interface(app)) {
        return false;
    }

    bool ok = app->backend == BACKEND_WPA ? scan_via_wpa(app, request_rescan)
                                          : scan_via_ifconfig(app, request_rescan);
    if (!ok) return false;

    refresh_status(app);
    mark_current(app);
    qsort(app->aps, app->ap_count, sizeof(app->aps[0]), compare_aps);
    if (app->selected >= (int)app->ap_count) {
        app->selected = app->ap_count ? (int)app->ap_count - 1 : 0;
    }
    if (app->ap_count == 0) {
        set_message(app, false, "Scan completed; no networks found");
    } else {
        snprintf(app->message, sizeof(app->message),
                 "Scan completed: %zu access points", app->ap_count);
        app->message_error = false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * Rendering
 * ------------------------------------------------------------------ */

static void clipped(WINDOW *win, int y, int x, int width, const char *text)
{
    if (width > 0) {
        mvwaddnstr(win, y, x, text ? text : "", width);
    }
}

static void signal_bar(char *buffer, size_t size, int signal)
{
    int bars = (signal + 19) / 20;
    if (bars < 0) bars = 0;
    if (bars > 5) bars = 5;
    snprintf(buffer, size, "[%.*s%.*s] %3d%%", bars, "#####", 5 - bars, ".....", signal);
}

static void draw_status_panel(App *app, int top, int left, int height, int width)
{
    if (height < 3 || width < 3) return;
    WINDOW *win = newwin(height, width, top, left);
    if (!win) return;
    if (app->focus == 1) wattron(win, COLOR_PAIR(2));
    box(win, 0, 0);
    if (app->focus == 1) wattroff(win, COLOR_PAIR(2));
    mvwaddstr(win, 0, 2, " Status ");

    int y = 2;
    char line[384];
#define STATUS_ROW(label, value) do { \
    if (y < height - 1) { \
        snprintf(line, sizeof(line), "%-11s %s", label, value); \
        clipped(win, y++, 2, width - 4, line); \
    } \
} while (0)
    STATUS_ROW("Interface", app->status.iface[0] ? app->status.iface : "-");
    STATUS_ROW("State", app->status.state);
    STATUS_ROW("Network", app->status.connection);
    STATUS_ROW("IPv4", app->status.ip);
    STATUS_ROW("Netmask", app->status.netmask);
    STATUS_ROW("Gateway", app->status.gateway);
    if (app->status.dns_count == 0) {
        STATUS_ROW("DNS", "-");
    } else {
        for (size_t i = 0; i < app->status.dns_count; i++) {
            STATUS_ROW(i == 0 ? "DNS" : "", app->status.dns[i]);
        }
    }
#undef STATUS_ROW
    wnoutrefresh(win);
    delwin(win);
}

static void draw_network_panel(App *app, int top, int left, int height, int width)
{
    if (height < 3 || width < 3) return;
    WINDOW *win = newwin(height, width, top, left);
    if (!win) return;
    if (app->focus == 0) wattron(win, COLOR_PAIR(2));
    box(win, 0, 0);
    if (app->focus == 0) wattroff(win, COLOR_PAIR(2));
    mvwaddstr(win, 0, 2, " Wi-Fi networks ");

    int rows = height - 4;
    if (rows < 1) rows = 1;
    if (app->selected < app->scroll) app->scroll = app->selected;
    if (app->selected >= app->scroll + rows) app->scroll = app->selected - rows + 1;

    if (width >= 58) {
        mvwprintw(win, 1, 2, "  %-24s %4s %-13s %s", "SSID", "CH", "SECURITY", "SIGNAL");
        mvwhline(win, 2, 1, ACS_HLINE, width - 2);
    }
    for (int row = 0; row < rows; row++) {
        int index = app->scroll + row;
        if (index >= (int)app->ap_count) break;
        AccessPoint *ap = &app->aps[index];
        int y = row + 3;
        if (index == app->selected && app->focus == 0) wattron(win, A_REVERSE);
        char bar[32];
        signal_bar(bar, sizeof(bar), ap->signal);
        char line[512];
        if (width >= 58) {
            snprintf(line, sizeof(line), "%c %-24.24s %4.4s %-13.13s %s",
                     ap->in_use ? '*' : ' ', ap->ssid, ap->channel, ap->security, bar);
        } else {
            snprintf(line, sizeof(line), "%c %-20.20s %s",
                     ap->in_use ? '*' : ' ', ap->ssid, bar);
        }
        clipped(win, y, 2, width - 4, line);
        if (index == app->selected && app->focus == 0) wattroff(win, A_REVERSE);
    }
    wnoutrefresh(win);
    delwin(win);
}

static const char *backend_label(const App *app)
{
    switch (app->backend) {
    case BACKEND_WPA: return "wpa_supplicant";
    case BACKEND_IFCONFIG: return "ifconfig (read-only)";
    default: return "no backend";
    }
}

static void draw(App *app)
{
    erase();
    int height, width;
    getmaxyx(stdscr, height, width);
    if (height < 12 || width < 54) {
        mvaddstr(0, 0, "Terminal too small (minimum 54x12)");
        wnoutrefresh(stdscr);
        doupdate();
        return;
    }

    attron(A_BOLD | COLOR_PAIR(1));
    mvaddstr(0, 2, "Wi-Fi TUI");
    attroff(A_BOLD | COLOR_PAIR(1));

    const char *label = backend_label(app);
    int label_x = width - (int)strlen(label) - 2;
    if (label_x > 14) mvaddstr(0, label_x, label);

    attron(app->message_error ? COLOR_PAIR(3) : COLOR_PAIR(1));
    mvaddnstr(height - 3, 1, app->message, width - 2);
    attroff(app->message_error ? COLOR_PAIR(3) : COLOR_PAIR(1));
    mvaddnstr(height - 2, 1,
             "Arrows: navigate/panels  Enter: connect  a: add  r: scan  d: disconnect  q: quit",
             width - 2);

    /* Stage stdscr first. Otherwise getch() may implicitly refresh its mostly
       empty buffer and erase the panels that were staged before it. */
    wnoutrefresh(stdscr);

    int content_height = height - 4;
    int network_width = width >= 90 ? (width * 2) / 3 : width;
    if (width >= 90) {
        draw_network_panel(app, 1, 0, content_height, network_width);
        draw_status_panel(app, 1, network_width, content_height, width - network_width);
    } else {
        int list_height = content_height * 2 / 3;
        draw_network_panel(app, 1, 0, list_height, width);
        draw_status_panel(app, 1 + list_height, 0, content_height - list_height, width);
    }
    doupdate();
}

static bool text_dialog(const char *title, const char *label, char *value, size_t capacity,
                        bool password, bool allow_empty)
{
    int screen_h, screen_w;
    getmaxyx(stdscr, screen_h, screen_w);
    int width = screen_w > 70 ? 66 : screen_w - 4;
    if (width < 30 || screen_h < 8) return false;
    WINDOW *win = newwin(7, width, (screen_h - 7) / 2, (screen_w - width) / 2);
    if (!win) return false;
    keypad(win, TRUE);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " %s ", title);
    mvwaddnstr(win, 2, 2, label, width - 4);
    mvwaddstr(win, 5, 2, "Enter: confirm   Esc: cancel");

    size_t len = strlen(value);
    curs_set(1);
    while (true) {
        mvwhline(win, 3, 2, ' ', width - 4);
        int visible = width - 6;
        size_t start = len > (size_t)visible ? len - (size_t)visible : 0;
        if (password) {
            wmove(win, 3, 2);
            for (size_t i = start; i < len; i++) waddch(win, '*');
        } else {
            mvwaddnstr(win, 3, 2, value + start, visible);
        }
        wmove(win, 3, 2 + (int)(len - start));
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == '\n' || ch == KEY_ENTER) {
            curs_set(0);
            delwin(win);
            return allow_empty || len > 0;
        }
        if (ch == 27) {
            if (password) explicit_bzero(value, capacity);
            curs_set(0);
            delwin(win);
            return false;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) value[--len] = '\0';
        } else if (ch >= 32 && ch <= 255 && len + 1 < capacity) {
            value[len++] = (char)ch;
            value[len] = '\0';
        }
    }
}

/* ------------------------------------------------------------------ *
 * Connecting
 * ------------------------------------------------------------------ */

static int find_saved_network(App *app, const char *ssid)
{
    char output[MAX_OUTPUT];
    if (wpa_cmd(app, output, sizeof(output), "list_networks", (char *)NULL) != 0) {
        return -1;
    }

    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (!strchr(line, '\t')) continue;
        if (strncmp(line, "network id", 10) == 0) continue;
        char fields[4][MAX_FIELD];
        if (split_tabs(line, fields, ARRAY_LEN(fields)) < 2) continue;
        if (!isdigit((unsigned char)fields[0][0])) continue;
        if (strcmp(fields[1], ssid) == 0) return atoi(fields[0]);
    }
    return -1;
}

/* Create and enable a wpa_supplicant network, returning its id or -1. */
static int create_network(App *app, const ConnectRequest *req, const char *secret,
                          char *error, size_t error_size)
{
    char output[MAX_OUTPUT];
    if (wpa_cmd(app, output, sizeof(output), "add_network", (char *)NULL) != 0) {
        copy_text(error, error_size, output[0] ? output : "add_network failed");
        return -1;
    }
    trim_newlines(output);
    const char *tail = last_line(output);
    if (!isdigit((unsigned char)tail[0])) {
        copy_text(error, error_size, output[0] ? output : "add_network returned no id");
        return -1;
    }

    char id[16];
    snprintf(id, sizeof(id), "%d", atoi(tail));

    char quoted[MAX_FIELD + 8];
    if (!wpa_quote_encoded(quoted, sizeof(quoted), req->ssid)) {
        copy_text(error, error_size, "SSID cannot be expressed in wpa_supplicant syntax");
        goto fail;
    }
    if (!wpa_set(app, id, "ssid", quoted)) {
        copy_text(error, error_size, "wpa_supplicant rejected the SSID");
        goto fail;
    }
    if (req->hidden && !wpa_set(app, id, "scan_ssid", "1")) {
        copy_text(error, error_size, "Could not enable hidden-SSID probing");
        goto fail;
    }

    if (req->open) {
        if (!wpa_set(app, id, "key_mgmt", "NONE")) {
            copy_text(error, error_size, "Could not configure an open network");
            goto fail;
        }
    } else if (req->wep) {
        if (!wpa_set(app, id, "key_mgmt", "NONE") ||
            !wpa_set_secret(app, id, "wep_key0", secret) ||
            !wpa_set(app, id, "wep_tx_keyidx", "0")) {
            copy_text(error, error_size, "Could not configure the WEP key");
            goto fail;
        }
    } else if (req->sae_only) {
        if (!wpa_set(app, id, "key_mgmt", "SAE")) {
            copy_text(error, error_size,
                      "This wpa_supplicant build has no WPA3/SAE support");
            goto fail;
        }
        if (!wpa_set(app, id, "ieee80211w", "2") ||
            !wpa_set_secret(app, id, "sae_password", secret)) {
            copy_text(error, error_size, "Could not configure the WPA3 passphrase");
            goto fail;
        }
    } else if (!wpa_set_secret(app, id, "psk", secret)) {
        copy_text(error, error_size, "Rejected passphrase (WPA needs 8-63 characters)");
        goto fail;
    }

    if (wpa_cmd(app, output, sizeof(output), "enable_network", id, (char *)NULL) != 0 ||
        !output_has_word(output, "OK")) {
        copy_text(error, error_size, "Could not enable the network");
        goto fail;
    }
    return atoi(id);

fail:
    wpa_cmd(app, output, sizeof(output), "remove_network", id, (char *)NULL);
    return -1;
}

static bool wait_for_association(App *app)
{
    char output[MAX_OUTPUT];
    for (int waited = 0; waited < ASSOC_TIMEOUT_MS; waited += POLL_INTERVAL_MS) {
        if (wpa_cmd(app, output, sizeof(output), "status", (char *)NULL) == 0 &&
            strstr(output, "wpa_state=COMPLETED")) {
            return true;
        }
        char note[128];
        snprintf(note, sizeof(note), "Associating... %ds", waited / 1000);
        progress(app, note);
        nap(POLL_INTERVAL_MS);
    }
    return false;
}

static bool has_ipv4(App *app)
{
    char output[MAX_OUTPUT];
    char *argv[] = {"ifconfig", app->status.iface, NULL};
    if (run_command(argv, output, sizeof(output)) != 0) return false;
    /* The trailing space keeps this from matching an "inet6" line. */
    return strstr(output, "\tinet ") != NULL || strstr(output, " inet ") != NULL;
}

/* devd(8) normally starts dhclient on association. Only step in when it has
   not, and only when we actually have the privileges to do so. */
static void ensure_dhcp(App *app, char *note, size_t note_size)
{
    for (int waited = 0; waited < DHCP_TIMEOUT_MS; waited += POLL_INTERVAL_MS) {
        if (has_ipv4(app)) {
            note[0] = '\0';
            return;
        }
        char text[128];
        snprintf(text, sizeof(text), "Waiting for DHCP... %ds", waited / 1000);
        progress(app, text);
        nap(POLL_INTERVAL_MS);
    }

    if (geteuid() != 0) {
        snprintf(note, note_size, "; no IPv4 lease yet, try: sudo dhclient %s",
                 app->status.iface);
        return;
    }

    char output[MAX_OUTPUT];
    char *argv[] = {"dhclient", "-b", app->status.iface, NULL};
    if (run_command(argv, output, sizeof(output)) != 0) {
        snprintf(note, note_size, "; dhclient failed");
    } else {
        snprintf(note, note_size, "; dhclient started");
    }
}

static void finish_connection(App *app, const char *ssid, bool created)
{
    char note[160] = "";
    ensure_dhcp(app, note, sizeof(note));

    const char *persistence = "";
    if (created) {
        char output[MAX_OUTPUT];
        bool saved = wpa_cmd(app, output, sizeof(output), "save_config",
                             (char *)NULL) == 0 &&
                     output_has_word(output, "OK");
        persistence = saved ? " (profile saved)"
                            : " (not saved: set update_config=1 in wpa_supplicant.conf)";
    }

    /* Refresh first: scan_networks() posts its own message, and the connection
       result is the one worth leaving on screen. */
    refresh_status(app);
    scan_networks(app, false);

    char text[512];
    snprintf(text, sizeof(text), "Connected to %s%s%s", ssid, persistence, note);
    set_message(app, false, text);
}

/* select_network disables every other profile, so a failed attempt has to put
   the supplicant back into roaming mode or the machine stays offline. */
static void restore_roaming(App *app)
{
    char output[MAX_OUTPUT];
    wpa_cmd(app, output, sizeof(output), "enable_network", "all", (char *)NULL);
    wpa_cmd(app, output, sizeof(output), "reconnect", (char *)NULL);
}

static void activate_network(App *app, int id, const char *ssid, bool created)
{
    char output[MAX_OUTPUT];
    char id_text[16];
    snprintf(id_text, sizeof(id_text), "%d", id);

    if (wpa_cmd(app, output, sizeof(output), "select_network", id_text,
                (char *)NULL) != 0 ||
        !output_has_word(output, "OK")) {
        set_message(app, true, output[0] ? output : "select_network failed");
        return;
    }

    if (!wait_for_association(app)) {
        if (created) {
            wpa_cmd(app, output, sizeof(output), "remove_network", id_text, (char *)NULL);
        }
        restore_roaming(app);
        refresh_status(app);
        set_message(app, true,
                    created ? "Association timed out; the passphrase is probably wrong"
                            : "Association timed out");
        return;
    }
    finish_connection(app, ssid, created);
}

static void connect_request(App *app, const ConnectRequest *req)
{
    if (app->backend != BACKEND_WPA) {
        set_message(app, true,
                    "Read-only mode: start wpa_supplicant on this interface to connect");
        return;
    }

    int existing = find_saved_network(app, req->ssid);
    if (existing >= 0) {
        progress(app, "Activating saved profile...");
        activate_network(app, existing, req->ssid, false);
        return;
    }

    char secret[MAX_FIELD] = "";
    if (!req->open) {
        const char *label = req->wep ? "WEP key:" : "Passphrase:";
        if (!text_dialog("Wi-Fi password", label, secret, sizeof(secret), true, false)) {
            explicit_bzero(secret, sizeof(secret));
            set_message(app, false, "Connection cancelled");
            return;
        }
    }

    char error[256] = "";
    progress(app, "Configuring network...");
    int id = create_network(app, req, secret, error, sizeof(error));
    explicit_bzero(secret, sizeof(secret));
    if (id < 0) {
        set_message(app, true, error[0] ? error : "Could not create the network profile");
        return;
    }
    activate_network(app, id, req->ssid, true);
}

static void connect_selected(App *app)
{
    if (app->ap_count == 0 || app->selected >= (int)app->ap_count) return;
    AccessPoint *ap = &app->aps[app->selected];

    if (strcmp(ap->ssid, "<hidden>") == 0) {
        set_message(app, true, "Hidden network: press 'a' and enter its SSID");
        return;
    }
    if (ap->enterprise) {
        set_message(app, true,
                    "WPA Enterprise needs an 802.1X block in wpa_supplicant.conf");
        return;
    }

    ConnectRequest req;
    memset(&req, 0, sizeof(req));
    copy_text(req.ssid, sizeof(req.ssid), ap->ssid);
    req.hidden = false;
    req.open = ap->open;
    req.wep = ap->wep;
    req.sae_only = ap->sae_only;
    connect_request(app, &req);
}

static void add_network(App *app)
{
    if (app->backend != BACKEND_WPA) {
        set_message(app, true,
                    "Read-only mode: start wpa_supplicant on this interface to connect");
        return;
    }

    char ssid[MAX_FIELD] = "";
    if (!text_dialog("Add network", "SSID (hidden networks are supported):", ssid,
                     sizeof(ssid), false, false)) {
        set_message(app, false, "Add network cancelled");
        return;
    }

    ConnectRequest req;
    memset(&req, 0, sizeof(req));
    req.hidden = true;

    char quoted[MAX_FIELD + 8];
    if (!wpa_quote_raw(quoted, sizeof(quoted), ssid)) {
        set_message(app, true, "SSID contains characters wpa_supplicant cannot accept");
        return;
    }
    /* Strip the quotes wpa_quote_raw added: connect_request re-quotes, and the
       escaped body is exactly the encoding wpa_supplicant reports back. */
    size_t body = strlen(quoted);
    if (body >= 2) {
        memmove(quoted, quoted + 1, body - 2);
        quoted[body - 2] = '\0';
    }
    copy_text(req.ssid, sizeof(req.ssid), quoted);

    if (find_saved_network(app, req.ssid) >= 0) {
        connect_request(app, &req);
        return;
    }

    char secret[MAX_FIELD] = "";
    if (!text_dialog("Wi-Fi password", "Passphrase (empty for an open network):", secret,
                     sizeof(secret), true, true)) {
        explicit_bzero(secret, sizeof(secret));
        set_message(app, false, "Add network cancelled");
        return;
    }
    req.open = secret[0] == '\0';

    char error[256] = "";
    progress(app, "Configuring network...");
    int id = create_network(app, &req, secret, error, sizeof(error));
    explicit_bzero(secret, sizeof(secret));
    if (id < 0) {
        set_message(app, true, error[0] ? error : "Could not create the network profile");
        return;
    }
    activate_network(app, id, req.ssid, true);
}

static void disconnect_wifi(App *app)
{
    if (!app->status.iface[0]) return;
    if (app->backend != BACKEND_WPA) {
        set_message(app, true, "Read-only mode: cannot disconnect without wpa_supplicant");
        return;
    }

    char output[MAX_OUTPUT];
    int rc = wpa_cmd(app, output, sizeof(output), "disconnect", (char *)NULL);
    bool ok = rc == 0 && output_has_word(output, "OK");
    char detail[512];
    copy_text(detail, sizeof(detail), output);

    refresh_status(app);
    scan_networks(app, false);
    set_message(app, !ok,
                ok ? "Disconnected; press Enter on a network to reconnect" : detail);
}

/* ------------------------------------------------------------------ *
 * Startup
 * ------------------------------------------------------------------ */

static void probe_backend(App *app)
{
    char output[MAX_OUTPUT];
    int rc = wpa_cmd(app, output, sizeof(output), "status", (char *)NULL);
    if (rc == 0 && strstr(output, "wpa_state")) {
        app->backend = BACKEND_WPA;
        return;
    }

    app->backend = BACKEND_IFCONFIG;
    copy_text(app->notice, sizeof(app->notice),
              rc == 127
                  ? "wpa_cli not found; listing the scan cache only"
                  : "wpa_supplicant not reachable; listing the scan cache only. "
                    "Check ctrl_interface_group in wpa_supplicant.conf");
    set_message(app, true, app->notice);
}

int main(void)
{
    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    App app;
    memset(&app, 0, sizeof(app));
    snprintf(app.message, sizeof(app.message), "Starting...");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);
        init_pair(2, COLOR_GREEN, -1);
        init_pair(3, COLOR_RED, -1);
    }
    signal(SIGWINCH, on_resize);

    if (find_wifi_interface(&app)) {
        probe_backend(&app);
        refresh_status(&app);
        scan_networks(&app, app.backend == BACKEND_WPA);
        /* The scan posts its own result; why we are degraded matters more. */
        if (app.notice[0]) set_message(&app, true, app.notice);
    }

    bool running = true;
    while (running) {
        if (resized) {
            resized = 0;
            resize_term(0, 0);
            clearok(stdscr, TRUE);
        }
        draw(&app);
        int ch = getch();
        switch (ch) {
        case 'q': case 'Q': running = false; break;
        case KEY_LEFT: app.focus = 0; break;
        case KEY_RIGHT: app.focus = 1; break;
        case KEY_UP:
            if (app.focus == 0 && app.selected > 0) app.selected--;
            break;
        case KEY_DOWN:
            if (app.focus == 0 && app.selected + 1 < (int)app.ap_count) app.selected++;
            break;
        case KEY_HOME: app.selected = 0; break;
        case KEY_END:
            if (app.ap_count) app.selected = (int)app.ap_count - 1;
            break;
        case KEY_PPAGE:
            app.selected -= 10;
            if (app.selected < 0) app.selected = 0;
            break;
        case KEY_NPAGE:
            app.selected += 10;
            if (app.selected >= (int)app.ap_count)
                app.selected = app.ap_count ? (int)app.ap_count - 1 : 0;
            break;
        case '\n': case KEY_ENTER:
            if (app.focus == 0) connect_selected(&app);
            break;
        case 'r': case 'R':
            progress(&app, "Scanning...");
            scan_networks(&app, true);
            break;
        case 'a': case 'A': add_network(&app); break;
        case 'd': case 'D': disconnect_wifi(&app); break;
        case KEY_RESIZE: break;
        default: break;
        }
    }
    endwin();
    return 0;
}
