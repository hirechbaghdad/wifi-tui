#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_APS 512
#define MAX_FIELD 256
#define MAX_OUTPUT 65536
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    bool in_use;
    char ssid[MAX_FIELD];
    char channel[16];
    char security[96];
    int signal;
} AccessPoint;

typedef struct {
    char iface[64];
    char state[64];
    char connection[MAX_FIELD];
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
    NetStatus status;
} App;

static volatile sig_atomic_t resized = 0;

static void on_resize(int sig)
{
    (void)sig;
    resized = 1;
}

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

static size_t split_escaped(const char *line, char delimiter, char fields[][MAX_FIELD],
                            size_t field_count)
{
    size_t item = 0;
    size_t pos = 0;
    bool escaped = false;
    memset(fields, 0, field_count * MAX_FIELD);

    for (const char *p = line; *p && item < field_count; p++) {
        if (escaped) {
            if (pos + 1 < MAX_FIELD) {
                fields[item][pos++] = *p;
            }
            escaped = false;
        } else if (*p == '\\') {
            escaped = true;
        } else if (*p == delimiter) {
            fields[item][pos] = '\0';
            item++;
            pos = 0;
        } else if (pos + 1 < MAX_FIELD) {
            fields[item][pos++] = *p;
        }
    }
    if (item < field_count) {
        fields[item][pos] = '\0';
        item++;
    }
    return item;
}

static void set_message(App *app, bool error, const char *text)
{
    snprintf(app->message, sizeof(app->message), "%s", text && *text ? text : "Done");
    trim_newlines(app->message);
    app->message_error = error;
}

static bool find_wifi_interface(App *app)
{
    char output[MAX_OUTPUT];
    char *argv[] = {"nmcli", "-t", "--escape", "yes", "-f", "DEVICE,TYPE,STATE",
                    "device", "status", NULL};
    int rc = run_command(argv, output, sizeof(output));
    if (rc != 0) {
        set_message(app, true, output[0] ? output : "Unable to run nmcli");
        return false;
    }

    char fallback[sizeof(app->status.iface)] = "";
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char fields[3][MAX_FIELD];
        if (split_escaped(line, ':', fields, ARRAY_LEN(fields)) < 3 ||
            strcmp(fields[1], "wifi") != 0) {
            continue;
        }
        if (!fallback[0]) {
            copy_text(fallback, sizeof(fallback), fields[0]);
        }
        if (strcmp(fields[2], "connected") == 0) {
            copy_text(app->status.iface, sizeof(app->status.iface), fields[0]);
            return true;
        }
    }
    if (fallback[0]) {
        snprintf(app->status.iface, sizeof(app->status.iface), "%s", fallback);
        return true;
    }
    set_message(app, true, "No Wi-Fi interface found by NetworkManager");
    return false;
}

static void prefix_to_netmask(const char *address, char *ip, size_t ip_size,
                              char *mask, size_t mask_size)
{
    char copy[64];
    copy_text(copy, sizeof(copy), address);
    char *slash = strchr(copy, '/');
    int prefix = -1;
    if (slash) {
        *slash = '\0';
        prefix = atoi(slash + 1);
    }
    snprintf(ip, ip_size, "%s", copy);
    if (prefix >= 0 && prefix <= 32) {
        uint32_t value = prefix == 0 ? 0 : htonl(0xffffffffu << (32 - prefix));
        struct in_addr addr = {.s_addr = value};
        if (!inet_ntop(AF_INET, &addr, mask, mask_size)) {
            snprintf(mask, mask_size, "-");
        }
    } else {
        snprintf(mask, mask_size, "-");
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
    char *argv[] = {"nmcli", "-t", "--escape", "yes", "-f",
                    "GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS",
                    "device", "show", app->status.iface, NULL};
    if (run_command(argv, output, sizeof(output)) != 0) {
        return;
    }

    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char fields[2][MAX_FIELD];
        if (split_escaped(line, ':', fields, ARRAY_LEN(fields)) < 2) {
            continue;
        }
        if (strcmp(fields[0], "GENERAL.STATE") == 0) {
            char *start = strchr(fields[1], '(');
            char *end = start ? strchr(start, ')') : NULL;
            if (start && end) {
                *end = '\0';
                snprintf(app->status.state, sizeof(app->status.state), "%s", start + 1);
            } else {
                copy_text(app->status.state, sizeof(app->status.state), fields[1]);
            }
        } else if (strcmp(fields[0], "GENERAL.CONNECTION") == 0) {
            copy_text(app->status.connection, sizeof(app->status.connection), fields[1]);
        } else if (strncmp(fields[0], "IP4.ADDRESS", 11) == 0 && app->status.ip[0] == '-') {
            prefix_to_netmask(fields[1], app->status.ip, sizeof(app->status.ip),
                              app->status.netmask, sizeof(app->status.netmask));
        } else if (strcmp(fields[0], "IP4.GATEWAY") == 0) {
            copy_text(app->status.gateway, sizeof(app->status.gateway), fields[1]);
        } else if (strncmp(fields[0], "IP4.DNS", 7) == 0 &&
                   app->status.dns_count < ARRAY_LEN(app->status.dns)) {
            copy_text(app->status.dns[app->status.dns_count++],
                      sizeof(app->status.dns[0]), fields[1]);
        }
    }
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

static bool scan_networks(App *app, bool request_rescan)
{
    if (!app->status.iface[0] && !find_wifi_interface(app)) {
        return false;
    }

    char output[MAX_OUTPUT];
    if (request_rescan) {
        char *rescan[] = {"nmcli", "--wait", "15", "device", "wifi", "rescan", "ifname",
                          app->status.iface, NULL};
        int rc = run_command(rescan, output, sizeof(output));
        if (rc != 0) {
            set_message(app, true, output[0] ? output : "Wi-Fi scan failed");
        }
    }

    char *list[] = {"nmcli", "-t", "--escape", "yes", "-f",
                    "IN-USE,SSID,CHAN,SECURITY,SIGNAL", "device", "wifi", "list",
                    "ifname", app->status.iface, "--rescan", "no", NULL};
    int rc = run_command(list, output, sizeof(output));
    if (rc != 0) {
        set_message(app, true, output[0] ? output : "Unable to list Wi-Fi networks");
        return false;
    }

    app->ap_count = 0;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line && app->ap_count < MAX_APS;
         line = strtok_r(NULL, "\n", &save)) {
        char fields[5][MAX_FIELD];
        if (split_escaped(line, ':', fields, ARRAY_LEN(fields)) < 5) {
            continue;
        }
        AccessPoint *ap = &app->aps[app->ap_count++];
        memset(ap, 0, sizeof(*ap));
        ap->in_use = strcmp(fields[0], "*") == 0 || strcmp(fields[0], "yes") == 0;
        copy_text(ap->ssid, sizeof(ap->ssid), fields[1][0] ? fields[1] : "<hidden>");
        copy_text(ap->channel, sizeof(ap->channel), fields[2]);
        copy_text(ap->security, sizeof(ap->security), fields[3][0] ? fields[3] : "Open");
        ap->signal = atoi(fields[4]);
        if (ap->signal < 0) ap->signal = 0;
        if (ap->signal > 100) ap->signal = 100;
    }
    qsort(app->aps, app->ap_count, sizeof(app->aps[0]), compare_aps);
    if (app->selected >= (int)app->ap_count) {
        app->selected = app->ap_count ? (int)app->ap_count - 1 : 0;
    }
    if (app->ap_count == 0) {
        set_message(app, false, "Scan completed; no networks found");
    } else {
        snprintf(app->message, sizeof(app->message), "Scan completed: %zu access points", app->ap_count);
        app->message_error = false;
    }
    refresh_status(app);
    return true;
}

static void clipped(WINDOW *win, int y, int x, int width, const char *text)
{
    if (width > 0) {
        mvwaddnstr(win, y, x, text ? text : "", width);
    }
}

static void signal_bar(char *buffer, size_t size, int signal)
{
    int bars = (signal + 19) / 20;
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
            snprintf(line, sizeof(line), "%c %-20.20s %s", ap->in_use ? '*' : ' ', ap->ssid, bar);
        }
        clipped(win, y, 2, width - 4, line);
        if (index == app->selected && app->focus == 0) wattroff(win, A_REVERSE);
    }
    wnoutrefresh(win);
    delwin(win);
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
    mvaddstr(0, width - 17, "NetworkManager");

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
                        bool password)
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
            return len > 0;
        }
        if (ch == 27) {
            if (password) memset(value, 0, capacity);
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

static void connect_network(App *app, const char *ssid, bool hidden, bool known_attempt)
{
    char output[MAX_OUTPUT];
    char *plain[] = {"nmcli", "--wait", "20", "device", "wifi", "connect", (char *)ssid,
                     "ifname", app->status.iface, hidden ? "hidden" : NULL,
                     hidden ? "yes" : NULL, NULL};
    char *compact[16];
    size_t n = 0;
    for (size_t i = 0; i < ARRAY_LEN(plain) && plain[i]; i++) compact[n++] = plain[i];
    compact[n] = NULL;
    int rc = run_command(compact, output, sizeof(output));
    if (rc == 0) {
        set_message(app, false, output);
        refresh_status(app);
        scan_networks(app, false);
        return;
    }

    if (!known_attempt) {
        set_message(app, true, output);
        return;
    }
    char password[MAX_FIELD] = "";
    if (!text_dialog("Wi-Fi password", "Password:", password, sizeof(password), true)) {
        set_message(app, false, "Connection cancelled");
        return;
    }
    char *secured[] = {"nmcli", "--ask", "--wait", "20", "device", "wifi", "connect",
                       (char *)ssid, "ifname", app->status.iface,
                       hidden ? "hidden" : NULL, hidden ? "yes" : NULL, NULL};
    n = 0;
    for (size_t i = 0; i < ARRAY_LEN(secured) && secured[i]; i++) compact[n++] = secured[i];
    compact[n] = NULL;
    char password_input[MAX_FIELD + 2];
    snprintf(password_input, sizeof(password_input), "%s\n", password);
    rc = run_command_input(compact, password_input, output, sizeof(output));
    memset(password, 0, sizeof(password));
    memset(password_input, 0, sizeof(password_input));
    set_message(app, rc != 0, output);
    refresh_status(app);
    if (rc == 0) scan_networks(app, false);
}

static void connect_selected(App *app)
{
    if (app->ap_count == 0 || app->selected >= (int)app->ap_count) return;
    AccessPoint *ap = &app->aps[app->selected];
    if (strcmp(ap->ssid, "<hidden>") == 0) {
        set_message(app, true, "Hidden network: press 'a' and enter its SSID");
        return;
    }
    if (strstr(ap->security, "802.1X") || strstr(ap->security, "WPA-EAP")) {
        set_message(app, true, "Enterprise Wi-Fi requires a preconfigured NetworkManager profile");
        return;
    }
    bool secure = strcmp(ap->security, "Open") != 0 && strcmp(ap->security, "--") != 0;
    connect_network(app, ap->ssid, false, secure);
}

static void add_network(App *app)
{
    char ssid[MAX_FIELD] = "";
    if (!text_dialog("Add network", "SSID (hidden networks are supported):", ssid,
                     sizeof(ssid), false)) {
        set_message(app, false, "Add network cancelled");
        return;
    }
    connect_network(app, ssid, true, true);
}

static void disconnect_wifi(App *app)
{
    if (!app->status.iface[0]) return;
    char output[MAX_OUTPUT];
    char *argv[] = {"nmcli", "--wait", "15", "device", "disconnect",
                    app->status.iface, NULL};
    int rc = run_command(argv, output, sizeof(output));
    set_message(app, rc != 0, output);
    refresh_status(app);
    scan_networks(app, false);
}

static bool check_nmcli(App *app)
{
    char output[1024];
    char *argv[] = {"nmcli", "-t", "-f", "RUNNING", "general", NULL};
    int rc = run_command(argv, output, sizeof(output));
    trim_newlines(output);
    if (rc == 127) {
        set_message(app, true, "nmcli is not installed; install the network-manager package");
        return false;
    }
    if (rc != 0 || strcmp(output, "running") != 0) {
        set_message(app, true, output[0] ? output : "NetworkManager is not running");
        return false;
    }
    return true;
}

int main(void)
{
    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    App app = {0};
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

    if (check_nmcli(&app) && find_wifi_interface(&app)) {
        refresh_status(&app);
        scan_networks(&app, true);
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
            set_message(&app, false, "Scanning...");
            draw(&app);
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
