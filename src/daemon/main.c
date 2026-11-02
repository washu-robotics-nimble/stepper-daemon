// nimhubd: the daemon that owns the MCU's host link (docs/daemon/spec.md)

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "commands.h"
#include "config.h"
#include "handshake.h"
#include "link.h"
#include "log.h"
#include "model.h"
#include "serial.h"
#include "server.h"

#define CONFIG_NAME "nimhub.ini"
#define CONFIG_DIR  "/etc/nimhubd"

static volatile sig_atomic_t stop_signal;

static void on_signal(int sig)
{
    stop_signal = sig;
}

static void usage(FILE *f)
{
    fprintf(f,
            "usage: nimhubd [-d] [CONFIG]\n"
            "  -d      debug: also log every frame sent to and received from the MCU\n"
            "  CONFIG  INI file; default " CONFIG_DIR "/" CONFIG_NAME ", or if that\n"
            "          doesn't exist, $XDG_CONFIG_HOME/nimhubd/" CONFIG_NAME "\n"
            "          ($XDG_CONFIG_HOME defaults to ~/.config)\n");
}

// The default config: the system one, else the user's (for testing without
// write access to /etc)
static const char *default_config(char *buf, size_t size)
{
    const char *system = CONFIG_DIR "/" CONFIG_NAME;
    if (access(system, F_OK) == 0)
        return system;

    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg != NULL && xdg[0] != '\0')
        snprintf(buf, size, "%s/nimhubd/" CONFIG_NAME, xdg);
    else if (home != NULL && home[0] != '\0')
        snprintf(buf, size, "%s/.config/nimhubd/" CONFIG_NAME, home);
    else
        return system;
    if (access(buf, F_OK) == 0)
        return buf;
    log_error("no config: neither %s nor %s exists", system, buf);
    return NULL;
}

#define BACKOFF_MIN_MS 250      // D-R31
#define BACKOFF_MAX_MS 2000

static model_t model;
static link_t mcu;
static handshake_t hs;

// The connection to the MCU: closed (fd -1, retried at next_attempt), or
// open with a handshake running, done (ready) or failed (D-R30, D-R31)
static struct {
    int fd;
    bool ready;                 // handshake done; clients were told link_up
    uint32_t backoff_ms;        // the wait before the next attempt
    uint64_t next_attempt;
    char problem[300];          // the last failure logged; "" after success
} conn;

static void on_event(void *ctx, const cJSON *evt);

// D-R31: a failure is logged once per change of state, not on every retry
static void report(const char *msg)
{
    if (strcmp(msg, conn.problem) == 0) {
        log_debug("%s (again)", msg);
        return;
    }
    log_error("%s; retrying", msg);
    snprintf(conn.problem, sizeof conn.problem, "%s", msg);
}

static void schedule_retry(void)
{
    conn.next_attempt = monotonic_ms() + conn.backoff_ms;
    conn.backoff_ms = conn.backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : conn.backoff_ms * 2;
}

// Open the device and start a handshake. False only at startup, if another
// process holds the device (D-R1); any other failure is retried.
static bool attempt(const config_t *cfg, commands_t *c, bool startup)
{
    char err[300];
    bool held;

    conn.fd = serial_open(cfg->device, &held, err, sizeof err);
    if (conn.fd < 0) {
        if (startup && held) {
            log_error("%s", err);
            return false;
        }
        report(err);
        schedule_retry();
        return true;
    }
    link_init(&mcu, conn.fd, (uint32_t)cfg->reply_timeout_ms, on_event, c);
    // After a failure, the handshake's progress is logged at debug level
    handshake_start(&hs, &mcu, cfg, &model, conn.problem[0] != '\0');
    return true;
}

// The link is gone, or the handshake failed: close, tell clients (D-R30),
// and retry with backoff (D-R31). Requests still waiting get link_down.
static void drop(commands_t *c, const char *what, const char *why)
{
    char msg[300];

    snprintf(msg, sizeof msg, "%s: %s", what, why);
    link_close(&mcu, msg);
    serial_close(conn.fd);
    conn.fd = -1;
    if (conn.ready) {
        conn.ready = false;
        commands_link_event(c, false, msg);
    }
    report(msg);
    schedule_retry();
}

// The handshake is done: clients get link_up, then the devices' state
static void went_up(commands_t *c)
{
    conn.ready = true;
    conn.backoff_ms = BACKOFF_MIN_MS;
    if (conn.problem[0] != '\0')
        log_info("link up again");
    conn.problem[0] = '\0';
    commands_link_event(c, true, NULL);
    commands_broadcast(c);
}

// Stopping on SIGTERM / SIGINT (D-R2): stop sent, waiting for move_end; 0:
// not stopping
#define STOP_WAIT_MS 2000
static uint64_t stopping_until;

static bool any_moving(void)
{
    for (size_t i = 0; i < model.count; i++)
        if (model_is_stepper(&model.devices[i]) && model.devices[i].moving)
            return true;
    return false;
}

// stop for every moving axis, as the firmware last reported it; returns how
// many were sent. Their move_end events end the wait.
static size_t stop_motion(void)
{
    size_t n = 0;

    if (!conn.ready || !link_up(&mcu))
        return 0;
    for (size_t i = 0; i < model.count; i++) {
        const model_device_t *d = &model.devices[i];
        if (!model_is_stepper(d) || !d->moving)
            continue;
        cJSON *req = link_new_request("stop", d->oid);
        if (link_request(&mcu, req, NULL, NULL))
            n++;
        cJSON_Delete(req);
    }
    return n;
}

// Follow the link and the handshake: drop, or tell clients it is up
static void update(commands_t *c)
{
    if (conn.fd < 0)
        return;
    if (!link_up(&mcu))
        drop(c, "link down", mcu.down_reason);
    else if (hs.status == HANDSHAKE_FAILED)
        drop(c, "handshake failed", hs.error);
    else if (hs.status == HANDSHAKE_READY && !conn.ready)
        went_up(c);
}

// Events from the MCU: into the model, and on to watching clients (D-R21).
// Every frame is logged at debug level anyway; position events are too
// frequent for more. ctx is the commands_t.
static void on_event(void *ctx, const cJSON *evt)
{
    commands_t *c = ctx;

    // A state report from a firmware that lost its settings: it was reset
    // (D-R30, F-R33), so reconnect
    const cJSON *applied = cJSON_GetObjectItemCaseSensitive(evt, "settings_applied");
    if (hs.status == HANDSHAKE_READY && cJSON_IsFalse(applied)) {
        link_close(&mcu, "firmware reset (settings_applied false)");
        return;
    }
    model_event(c->model, evt);
    commands_broadcast(c);

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(evt, "evt");
    if (cJSON_IsString(name) && strcmp(name->valuestring, "position") == 0)
        return;

    char *text = cJSON_PrintUnformatted(evt);
    if (cJSON_IsString(name) && strcmp(name->valuestring, "rx_error") == 0)
        log_warn("the MCU dropped a frame: %s", text != NULL ? text : "?");
    else if (conn.problem[0] != '\0')
        log_debug("event: %s", text != NULL ? text : "?");  // retrying (D-R31)
    else
        log_info("event: %s", text != NULL ? text : "?");
    cJSON_free(text);
}

int main(int argc, char *argv[])
{
    int opt;
    bool debug = false;

    while ((opt = getopt(argc, argv, "dh")) != -1) {
        switch (opt) {
        case 'd':
            debug = true;
            break;
        case 'h':
            usage(stdout);
            return EXIT_SUCCESS;
        default:
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    if (argc - optind > 1) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    log_set_level(debug ? LOG_DEBUG : LOG_INFO);

    char buf[512];
    const char *config_path = optind < argc ? argv[optind] : default_config(buf, sizeof buf);
    static config_t cfg;
    if (config_path == NULL || config_load(config_path, &cfg) != 0)
        return EXIT_FAILURE;
    log_info("config %s, device %s", config_path, cfg.device);

    // SIGINT / SIGTERM are only taken in ppoll(), so none is missed between
    // checking the flag and waiting
    sigset_t block, wait_mask;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &wait_mask);
    struct sigaction sa = {.sa_handler = on_signal};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // A write to a client that has gone fails with EPIPE instead
    signal(SIGPIPE, SIG_IGN);

    static server_t server;
    static commands_t commands;
    commands_init(&commands, &mcu, &model, &server, (uint32_t)cfg.lease_timeout_ms);
    conn.fd = -1;
    conn.backoff_ms = BACKOFF_MIN_MS;

    // The device first: if another process holds it, this nimhubd refuses to
    // start (D-R1), before touching the socket of one that may be running.
    // A device that isn't there yet is retried (D-R31).
    if (!attempt(&cfg, &commands, true))
        return EXIT_FAILURE;
    if (server_open(&server, cfg.socket, cfg.group, commands_line, commands_client_gone,
                    &commands) != 0) {
        if (conn.fd >= 0)
            serial_close(conn.fd);
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;
    for (;;) {
        uint64_t now = monotonic_ms();
        if (conn.fd < 0 && now >= conn.next_attempt)
            attempt(&cfg, &commands, false);
        update(&commands);

        int timeout;
        if (conn.fd >= 0) {
            timeout = link_poll_timeout(&mcu);
            int retry = handshake_poll_timeout(&hs);
            if (retry >= 0 && (timeout < 0 || retry < timeout))
                timeout = retry;
        } else {
            now = monotonic_ms();
            timeout = conn.next_attempt <= now ? 0 : (int)(conn.next_attempt - now);
        }
        if (stopping_until != 0) {
            now = monotonic_ms();
            int left = stopping_until <= now ? 0 : (int)(stopping_until - now);
            if (timeout < 0 || left < timeout)
                timeout = left;
        }
        struct timespec ts = {.tv_sec = timeout / 1000, .tv_nsec = (timeout % 1000) * 1000000L};
        struct pollfd pfds[2 + SERVER_MAX_CLIENTS];
        // A closed device is fd -1, which poll() skips
        pfds[0] = (struct pollfd){.fd = conn.fd,
                                  .events = conn.fd >= 0 ? link_poll_events(&mcu) : 0};
        size_t count = 1 + server_poll_fill(&server, &pfds[1]);

        int n = ppoll(pfds, count, timeout < 0 ? NULL : &ts, &wait_mask);
        if (n < 0 && errno != EINTR) {
            log_error("poll: %s", strerror(errno));
            status = EXIT_FAILURE;
            break;
        }
        if (stop_signal != 0) {
            int sig = stop_signal;
            stop_signal = 0;
            // A second signal while stopping: out at once
            if (stopping_until != 0) {
                log_info("%s again: closing the link now", strsignal(sig));
                break;
            }
            // D-R2: stop motion first, then close the link
            size_t n_stops = stop_motion();
            if (n_stops == 0) {
                log_info("%s: closing the link", strsignal(sig));
                break;
            }
            log_info("%s: stopping %zu axis(es), then closing the link", strsignal(sig), n_stops);
            stopping_until = monotonic_ms() + STOP_WAIT_MS;
        }
        if (n > 0) {
            if (conn.fd >= 0)
                link_handle(&mcu, pfds[0].revents);
            server_poll_handle(&server, &pfds[1]);
        }
        if (conn.fd >= 0) {
            link_tick(&mcu);
            handshake_tick(&hs);
        }
        // Before the broadcast, so link_up comes ahead of the state
        update(&commands);
        commands_broadcast(&commands);

        // Stopping: done once no axis moves, the link is gone, or the wait
        // is over
        if (stopping_until != 0) {
            if (!any_moving()) {
                log_info("motion stopped: closing the link");
                break;
            }
            if (!conn.ready || monotonic_ms() >= stopping_until) {
                log_warn("motion not confirmed stopped: closing the link anyway");
                break;
            }
        }
    }

    server_close(&server);
    if (conn.fd >= 0)
        serial_close(conn.fd);
    return status;
}
