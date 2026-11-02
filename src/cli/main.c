// nimhub-cli: the command-line client for nimhubd (D-R15, build plan step 10)
//
// Interactive on a terminal: a prompt with line editing (linenoise), and a
// status bar below it, following nimhubd's status events (watch). With a command on
// the command line, it sends that one; with stdin not a terminal, one per
// line. Replies are printed as nimhubd sends them.

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "cJSON.h"
#include "conn.h"
#include "linenoise.h"
#include "parse.h"

#define SOCKET_DEFAULT   "/run/nimhubd/control.sock"   // D-R12
#define RETRY_MS         250    // devices, until nimhubd has the MCU
#define RECONNECT_MS     1000
#define REPLY_TIMEOUT_MS 3000   // nimhubd answers within its own 500 ms (D-R30)
#define MAX_DEVS         32
#define PROMPT           "nimhub> "

// ANSI: ESC[2K erases the line, ESC7 / ESC8 save / restore the cursor
#define CLEAR_LINE  "\r\033[2K"
#define CUR_UP      "\033[1A"
#define CUR_DOWN    "\033[1B"   // unlike \n, never scrolls
#define CUR_SAVE    "\0337"
#define CUR_RESTORE "\0338"
#define SGR_RESET   "\033[0m"
#define STYLE_IDLE   "\033[48;5;238m\033[38;5;252m"    // grey
#define STYLE_MOVING "\033[48;5;23m\033[38;5;159m"     // teal
#define STYLE_ERROR  "\033[48;5;52m\033[38;5;217m"     // red

typedef struct {
    char dev[32];
    bool motor;
    bool known;             // a status event arrived
    int32_t pos;
    bool pos_valid, moving, locked;
} bar_dev_t;

static struct {
    const char *path;
    conn_t conn;
    uint32_t seq;
    bool quit;
    bool failed;                // a request failed: exit status 1

    // The user's request, waiting for its reply
    bool waiting;
    double wait_seq;            // -1: a raw request without a valid seq, which
                                // only a bad_request event answers
    uint64_t wait_until;
    enum {
        STEP_PLAIN,             // the user's request; its reply is printed
        STEP_LEASE,             // automatic lease for a write: lease sent
        STEP_WRITE,             // ...the write sent; its reply is printed
        STEP_RELEASE,           // ...release sent; its reply isn't printed
        STEP_RENEW,             // renew of the user's own lease, without a terminal
    } step;
    cJSON *held;                // STEP_LEASE: the write, sent once the lease is ours
    char user_cmd[16];          // STEP_PLAIN: its cmd, to follow lease / release

    // The lease, taken by the user with "lease" (D-R18): kept, and renewed
    // in the background until "release"
    bool lease_mine;
    uint32_t lease_timeout_ms;
    uint64_t next_renew;
    uint32_t renew_seq;         // renew in flight; 0: none

    // Status bar (interactive only)
    bool bar;
    size_t ndev;
    bar_dev_t devs[MAX_DEVS];
    bool have_devices;
    uint32_t devices_seq;       // devices request in flight; 0: none
    uint32_t watch_seq;         // watch request in flight; 0: none
    bool link_down;
    uint64_t next_devices;      // retry devices (link down) at; 0: now
    uint64_t next_connect;

    // Line editing
    bool editing;
    struct linenoiseState ls;
    char buf[1024];
} S = {.conn = {.fd = -1}};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t next_seq(void)
{
    if (++S.seq == 0)
        S.seq = 1;
    return S.seq;
}

static bool connected(void)
{
    return S.conn.fd >= 0;
}

// ---- Status bar -----

static int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

static const char *bar_text(char *text, size_t size)
{
    if (!connected()) {
        snprintf(text, size, "nimhubd not running (%s)", S.path);
        return STYLE_ERROR;
    }
    if (S.link_down) {
        snprintf(text, size, "link down: nimhubd has no MCU");
        return STYLE_ERROR;
    }
    if (!S.have_devices) {
        snprintf(text, size, "connecting");
        return STYLE_IDLE;
    }

    bool moving = false;
    size_t len = 0;
    text[0] = '\0';
    for (size_t i = 0; i < S.ndev && len < size; i++) {
        const bar_dev_t *d = &S.devs[i];
        if (!d->motor)
            continue;
        if (!d->known)
            len += snprintf(text + len, size - len, "%s%s ?", len ? "   " : "", d->dev);
        else
            len += snprintf(text + len, size - len, "%s%s %ld%s %s %s", len ? "   " : "",
                            d->dev, (long)d->pos, d->pos_valid ? "" : " (invalid)",
                            d->moving ? "moving" : "idle", d->locked ? "locked" : "free");
        moving |= d->known && d->moving;
    }
    if (len == 0)
        len = snprintf(text, size, "no motors");
    if (S.lease_mine && len < size)
        snprintf(text + len, size - len, "   lease: yours");
    return moving ? STYLE_MOVING : STYLE_IDLE;
}

// Paint the bar over the line the cursor is on
static void put_bar(void)
{
    char text[512];
    const char *style = bar_text(text, sizeof text);

    // One column short of the edge, so it never wraps
    int avail = term_width() - 2;
    if (avail < 0)
        avail = 0;
    int len = (int)strlen(text);
    if (len > avail)
        len = avail;
    printf(CLEAR_LINE "%s %.*s%*s" SGR_RESET, style, len, text, avail - len, "");
}

// Repaint the bar below the prompt, leaving the line being edited alone
static void redraw_bar(void)
{
    if (!S.editing)
        return;
    printf(CUR_SAVE CUR_DOWN);
    put_bar();
    printf(CUR_RESTORE);
    fflush(stdout);
}

// ---- Line editing -----

static void begin_edit(void)
{
    // A fresh bar line below, then back up to the prompt
    printf("\n");
    put_bar();
    printf(CUR_UP CLEAR_LINE);
    fflush(stdout);
    linenoiseEditStart(&S.ls, -1, -1, S.buf, sizeof S.buf, PROMPT);
    S.editing = true;
}

static void end_edit(void)
{
    linenoiseEditStop(&S.ls);   // leaves the cursor on the bar line
    printf(CLEAR_LINE);
    fflush(stdout);
    S.editing = false;
}

// Print one line of output; while editing, above the prompt
static void print_line(FILE *f, const char *text)
{
    if (!S.editing) {
        fprintf(f, "%s\n", text);
        return;
    }
    // Raw mode: "\n" doesn't return the carriage, hence the "\r"s
    linenoiseHide(&S.ls);
    printf(CLEAR_LINE "%s\r\n\n", text);
    put_bar();
    printf(CUR_UP CLEAR_LINE);
    fflush(stdout);
    linenoiseShow(&S.ls);
}

static void print_error(const char *fmt, const char *arg)
{
    char msg[512];
    snprintf(msg, sizeof msg, fmt, arg);
    print_line(S.bar ? stdout : stderr, msg);
    S.failed = true;
}

// Tab completion: commands and device names for the first word, device
// commands for the second
static void complete(const char *buf, linenoiseCompletions *lc)
{
    const char *word = strrchr(buf, ' ');
    word = word != NULL ? word + 1 : buf;
    size_t prefix_len = (size_t)(word - buf);
    size_t index = 0;
    for (const char *p = buf; p < word; p++)
        if (*p == ' ' && (p == buf || p[-1] != ' '))
            index++;

    const char *cands[64];
    size_t n = 0;
    if (index == 0) {
        cands[n++] = "help";
        cands[n++] = "quit";
        for (const char *const *c = cli_daemon_commands; *c != NULL; c++)
            cands[n++] = *c;
        for (size_t i = 0; i < S.ndev && n < 64; i++)
            cands[n++] = S.devs[i].dev;
    } else if (index == 1) {
        for (const char *const *c = cli_motor_commands; *c != NULL && n < 64; c++)
            cands[n++] = *c;
    }

    char line[1024];
    for (size_t i = 0; i < n; i++) {
        if (strncmp(cands[i], word, strlen(word)) != 0)
            continue;
        snprintf(line, sizeof line, "%.*s%s ", (int)prefix_len, buf, cands[i]);
        linenoiseAddCompletion(lc, line);
    }
}

// ---- Messages from nimhubd -----

static bool acked(const cJSON *msg)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "ack"));
}

static bool is_link_down(const cJSON *msg)
{
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(msg, "err");
    return cJSON_IsString(err) && strcmp(err->valuestring, "link_down") == 0;
}

static uint32_t bar_request(const char *cmd, const char *dev);

static void got_devices(const cJSON *msg)
{
    S.link_down = is_link_down(msg);
    if (!acked(msg))
        return;

    const cJSON *list = cJSON_GetObjectItemCaseSensitive(msg, "devices");
    const cJSON *item;
    S.ndev = 0;
    cJSON_ArrayForEach(item, list) {
        const cJSON *dev = cJSON_GetObjectItemCaseSensitive(item, "dev");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(dev) || S.ndev == MAX_DEVS)
            continue;
        bar_dev_t *d = &S.devs[S.ndev++];
        memset(d, 0, sizeof *d);
        snprintf(d->dev, sizeof d->dev, "%s", dev->valuestring);
        d->motor = cJSON_IsString(type) && strcmp(type->valuestring, "stepper") == 0;
    }
    S.have_devices = true;
    // Their state comes with watch, and every change after it (D-R21)
    S.watch_seq = bar_request("watch", NULL);
}

// A status event: one device's state (D-R22)
static void got_status(const cJSON *msg)
{
    const cJSON *dev = cJSON_GetObjectItemCaseSensitive(msg, "dev");
    bar_dev_t *d = NULL;
    for (size_t i = 0; i < S.ndev && cJSON_IsString(dev); i++)
        if (strcmp(S.devs[i].dev, dev->valuestring) == 0)
            d = &S.devs[i];
    if (d == NULL)
        return;
    const cJSON *pos = cJSON_GetObjectItemCaseSensitive(msg, "pos");
    if (cJSON_IsNumber(pos))
        d->pos = (int32_t)pos->valuedouble;
    d->pos_valid = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "pos_valid"));
    d->moving = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "moving"));
    d->locked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "locked"));
    d->known = true;
}

static bool send_step(cJSON *req, int step);

// The user's request is over (answered, failed, or given up on)
static void end_wait(void)
{
    S.waiting = false;
    cJSON_Delete(S.held);
    S.held = NULL;
}

static void send_simple(const char *cmd, int step)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "cmd", cmd);
    send_step(req, step);
    cJSON_Delete(req);
}

// The reply to the user's request, or to a step of the automatic lease
// around a write: lease, the write, release (D-R18)
static void got_reply(const char *line, const cJSON *msg)
{
    bool ok = acked(msg);

    S.waiting = false;
    switch (S.step) {
    case STEP_LEASE:
        if (ok) {
            cJSON *write = S.held;
            S.held = NULL;
            send_step(write, STEP_WRITE);
            cJSON_Delete(write);
        } else {
            // Held by another client: this reply is the answer
            S.failed = true;
            print_line(stdout, line);
            end_wait();
        }
        break;
    case STEP_WRITE:
        if (!ok)
            S.failed = true;
        print_line(stdout, line);
        send_simple("release", STEP_RELEASE);
        break;
    case STEP_RELEASE:
        break;
    case STEP_RENEW:
        if (!ok) {
            S.lease_mine = false;
            print_line(stderr, "lease lost: it expired, or another client took it");
        }
        break;
    case STEP_PLAIN:
        if (!ok)
            S.failed = true;
        print_line(stdout, line);
        // The user's own lease: kept and renewed until released
        if (ok && strcmp(S.user_cmd, "lease") == 0) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(msg, "timeout_ms");
            S.lease_mine = true;
            S.lease_timeout_ms = cJSON_IsNumber(t) && t->valuedouble >= 2 ? (uint32_t)t->valuedouble : 5000;
            S.next_renew = now_ms() + S.lease_timeout_ms / 2;
        } else if (strcmp(S.user_cmd, "release") == 0) {
            S.lease_mine = false;
        }
        break;
    }
}

static void on_line(void *ctx, const char *line, const cJSON *msg)
{
    (void)ctx;
    const cJSON *evt = cJSON_GetObjectItemCaseSensitive(msg, "evt");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(msg, "seq");

    if (cJSON_IsString(evt)) {
        // In interactive mode, status events drive the bar, and aren't printed
        if (S.bar && strcmp(evt->valuestring, "status") == 0) {
            got_status(msg);
            redraw_bar();
            return;
        }
        // The link to the MCU (D-R30): red bar while down; afterwards the
        // devices may differ, so they are asked for again, then watch
        if (strcmp(evt->valuestring, "link_down") == 0) {
            S.link_down = true;
        } else if (strcmp(evt->valuestring, "link_up") == 0) {
            S.link_down = false;
            S.have_devices = false;
            S.next_devices = 0;
        }
        // A bad_request event answers a request whose seq couldn't be read
        if (S.waiting && strcmp(evt->valuestring, "bad_request") == 0) {
            end_wait();
            S.failed = true;
        }
        print_line(stdout, line);
        return;
    }
    if (!cJSON_IsNumber(seq)) {
        print_line(stdout, line);
        return;
    }
    if (S.waiting && seq->valuedouble == S.wait_seq) {
        got_reply(line, msg);
        redraw_bar();
        return;
    }

    if (S.renew_seq != 0 && seq->valuedouble == S.renew_seq) {
        S.renew_seq = 0;
        if (!acked(msg) && S.lease_mine) {
            S.lease_mine = false;
            print_line(stdout, "lease lost: it expired, or another client took it");
        }
    } else if (S.devices_seq != 0 && seq->valuedouble == S.devices_seq) {
        S.devices_seq = 0;
        got_devices(msg);
    } else if (S.watch_seq != 0 && seq->valuedouble == S.watch_seq) {
        S.watch_seq = 0;
    }
    redraw_bar();
}

static void lost_connection(void)
{
    S.have_devices = false;
    S.devices_seq = 0;
    S.watch_seq = 0;
    S.next_devices = 0;
    S.renew_seq = 0;
    S.lease_mine = false;       // freed by the disconnect (D-R18)
    if (S.waiting) {
        end_wait();
        print_error("error: nimhubd closed the connection", NULL);
    }
    S.next_connect = now_ms() + RECONNECT_MS;
    redraw_bar();
}

// ---- Requests -----

// A request of the bar; no reply is waited for
static uint32_t bar_request(const char *cmd, const char *dev)
{
    cJSON *req = cJSON_CreateObject();
    uint32_t seq = next_seq();
    cJSON_AddNumberToObject(req, "seq", seq);
    cJSON_AddStringToObject(req, "cmd", cmd);
    if (dev != NULL)
        cJSON_AddStringToObject(req, "dev", dev);
    bool ok = conn_send(&S.conn, req);
    cJSON_Delete(req);
    if (!ok) {
        lost_connection();
        return 0;
    }
    return seq;
}

// The bar's devices: asked for once connected, and again every RETRY_MS
// while nimhubd has no MCU (link_down); watch follows the reply
static void bar_devices(void)
{
    uint64_t now = now_ms();
    if (!connected() || S.have_devices || S.devices_seq != 0 || now < S.next_devices)
        return;
    S.next_devices = now + RETRY_MS;
    S.devices_seq = bar_request("devices", NULL);
}

// The renewal of the user's own lease, at half its timeout
static void renew_lease(void)
{
    uint64_t now = now_ms();
    if (!S.lease_mine || !connected() || now < S.next_renew)
        return;
    S.next_renew = now + S.lease_timeout_ms / 2;
    S.renew_seq = bar_request("renew", NULL);
}

// Send a request the user waits for, as this step; seq is added unless req
// has one. False if it couldn't be sent (then the wait is over).
static bool send_step(cJSON *req, int step)
{
    if (!connected()) {
        end_wait();
        print_error("error: not connected to nimhubd (%s)", S.path);
        return false;
    }
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(req, "seq");
    if (seq == NULL)
        cJSON_AddNumberToObject(req, "seq", next_seq());
    seq = cJSON_GetObjectItemCaseSensitive(req, "seq");
    S.wait_seq = cJSON_IsNumber(seq) ? seq->valuedouble : -1;

    if (!conn_send(&S.conn, req)) {
        lost_connection();
        end_wait();
        print_error("error: not connected to nimhubd (%s)", S.path);
        return false;
    }
    S.step = step;
    S.waiting = true;
    S.wait_until = now_ms() + REPLY_TIMEOUT_MS;
    return true;
}

// A typed request: a write gets the lease around it, unless the user holds
// it already (D-R18); raw JSON goes out as it is
static void send_user(cJSON *req, bool raw)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    snprintf(S.user_cmd, sizeof S.user_cmd, "%s",
             !raw && cJSON_IsString(cmd) ? cmd->valuestring : "");

    if (!raw && cli_is_write(req) && !S.lease_mine) {
        S.held = cJSON_Duplicate(req, true);
        send_simple("lease", STEP_LEASE);
    } else {
        send_step(req, STEP_PLAIN);
    }
}

static void wait_timed_out(void)
{
    // An automatic lease taken but not released expires on its own
    end_wait();
    print_error("error: no reply from nimhubd", NULL);
}

static void handle_line(const char *line)
{
    cJSON *req;
    char err[128];

    cli_kind_t kind = cli_parse(line, &req, err, sizeof err);
    switch (kind) {
    case CLI_EMPTY:
        break;
    case CLI_HELP:
        fputs(cli_help(), stdout);
        break;
    case CLI_QUIT:
        S.quit = true;
        break;
    case CLI_ERROR:
        print_error("error: %s", err);
        break;
    case CLI_REQUEST:
    case CLI_RAW:
        send_user(req, kind == CLI_RAW);
        cJSON_Delete(req);
        break;
    }
}

// ---- Modes -----

// Wait for the reply to the user's request, without a terminal
static void wait_reply(void)
{
    while (S.waiting) {
        int left = (int)(S.wait_until - now_ms());
        if (left <= 0) {
            wait_timed_out();
            return;
        }
        struct pollfd pfd = {.fd = S.conn.fd, .events = POLLIN};
        if (poll(&pfd, 1, left) > 0 && !conn_read(&S.conn, on_line, NULL))
            lost_connection();
    }
}

// Without a terminal there is no background loop: the user's own lease is
// renewed before the next command, once half its timeout has passed. Only
// lease and renew keep it (D-R18).
static void renew_before_command(void)
{
    uint64_t now = now_ms();
    if (!S.lease_mine || !connected() || now < S.next_renew)
        return;
    S.next_renew = now + S.lease_timeout_ms / 2;
    send_simple("renew", STEP_RENEW);
    wait_reply();
}

// One-shot (line != NULL) or stdin mode
static int run_batch(const char *line)
{
    if (!conn_open(&S.conn, S.path)) {
        fprintf(stderr, "nimhub-cli: cannot connect to %s: %s\n", S.path, strerror(errno));
        return EXIT_FAILURE;
    }
    if (line != NULL) {
        handle_line(line);
        wait_reply();
    } else {
        char *buf = NULL;
        size_t size = 0;
        while (!S.quit && getline(&buf, &size, stdin) >= 0) {
            buf[strcspn(buf, "\n")] = '\0';
            renew_before_command();
            handle_line(buf);
            wait_reply();
        }
        free(buf);
    }
    conn_close(&S.conn);
    return S.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int timeout_until(uint64_t when, uint64_t now, int current)
{
    int t = when <= now ? 0 : (int)(when - now);
    return current < 0 || t < current ? t : current;
}

static int run_interactive(void)
{
    S.bar = true;
    linenoiseSetCompletionCallback(complete);
    linenoiseHistorySetMaxLen(200);
    printf("nimhub-cli: nimhubd at %s. Type 'help' for commands.\n", S.path);
    if (!conn_open(&S.conn, S.path))
        S.next_connect = now_ms() + RECONNECT_MS;

    while (!S.quit) {
        uint64_t now = now_ms();
        if (!connected() && now >= S.next_connect) {
            S.next_connect = now + RECONNECT_MS;
            if (conn_open(&S.conn, S.path))
                redraw_bar();
        }
        bar_devices();
        renew_lease();
        if (S.waiting && now >= S.wait_until)
            wait_timed_out();
        if (!S.editing && !S.waiting)
            begin_edit();

        struct pollfd pfds[2];
        int n = 0, in_i = -1, conn_i = -1;
        if (S.editing) {
            in_i = n;
            pfds[n++] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        }
        if (connected()) {
            conn_i = n;
            pfds[n++] = (struct pollfd){.fd = S.conn.fd, .events = POLLIN};
        }
        int timeout = -1;
        now = now_ms();
        if (!connected())
            timeout = timeout_until(S.next_connect, now, timeout);
        else if (!S.have_devices && S.devices_seq == 0)
            timeout = timeout_until(S.next_devices, now, timeout);
        if (S.waiting)
            timeout = timeout_until(S.wait_until, now, timeout);
        if (S.lease_mine)
            timeout = timeout_until(S.next_renew, now, timeout);
        if (poll(pfds, (nfds_t)n, timeout) <= 0)
            continue;

        if (conn_i >= 0 && pfds[conn_i].revents && !conn_read(&S.conn, on_line, NULL))
            lost_connection();

        if (in_i >= 0 && S.editing && pfds[in_i].revents) {
            char *res = linenoiseEditFeed(&S.ls);
            if (res == linenoiseEditMore)
                continue;
            int e = errno;
            size_t typed = S.ls.len;
            end_edit();
            if (res == NULL) {
                // Ctrl-C with something typed drops the line; Ctrl-D, or
                // Ctrl-C on an empty line, quits
                if (!(e == EAGAIN && typed > 0))
                    S.quit = true;
                continue;
            }
            if (res[0] != '\0')
                linenoiseHistoryAdd(res);
            handle_line(res);
            linenoiseFree(res);
            fflush(stdout);
        }
    }

    if (S.editing)
        end_edit();
    conn_close(&S.conn);
    return EXIT_SUCCESS;
}

static void usage(FILE *f)
{
    fprintf(f,
            "usage: nimhub-cli [-s SOCKET] [COMMAND ...]\n"
            "  -s SOCKET  nimhubd's socket (default " SOCKET_DEFAULT ")\n"
            "  COMMAND    send this one command, print the reply and exit,\n"
            "             e.g. nimhub-cli m0 rel 400\n"
            "Without COMMAND: interactive on a terminal, else one command per\n"
            "line from stdin. The exit status is 1 if any command failed.\n\n");
    fputs(cli_help(), f);
}

int main(int argc, char *argv[])
{
    int opt;

    S.path = SOCKET_DEFAULT;
    // "+": options end at the first word, so "m0 rel -100" works
    while ((opt = getopt(argc, argv, "+s:h")) != -1) {
        switch (opt) {
        case 's':
            S.path = optarg;
            break;
        case 'h':
            usage(stdout);
            return EXIT_SUCCESS;
        default:
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        char line[1024];
        size_t len = 0;
        line[0] = '\0';
        for (int i = optind; i < argc && len < sizeof line; i++)
            len += snprintf(line + len, sizeof line - len, "%s%s", i > optind ? " " : "", argv[i]);
        return run_batch(line);
    }
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
        return run_interactive();
    return run_batch(NULL);
}
