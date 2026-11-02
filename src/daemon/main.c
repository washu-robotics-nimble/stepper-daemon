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

// Events from the MCU. Every frame is logged at debug level anyway; position
// events are too frequent for more. ctx is the machine model.
static void on_event(void *ctx, const cJSON *evt)
{
    model_event(ctx, evt);

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(evt, "evt");
    if (cJSON_IsString(name) && strcmp(name->valuestring, "position") == 0)
        return;

    char *text = cJSON_PrintUnformatted(evt);
    if (cJSON_IsString(name) && strcmp(name->valuestring, "rx_error") == 0)
        log_warn("the MCU dropped a frame: %s", text != NULL ? text : "?");
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

    // The device first: if another nimhubd holds it, this one stops before
    // touching that one's socket (D-R1)
    int fd = serial_open(cfg.device);
    if (fd < 0)
        return EXIT_FAILURE;

    static model_t model;
    static link_t link;
    static handshake_t hs;
    static server_t server;
    static commands_t commands;
    link_init(&link, fd, (uint32_t)cfg.reply_timeout_ms, on_event, &model);
    commands_init(&commands, &link, &model, &hs, &server, (uint32_t)cfg.lease_timeout_ms);
    if (server_open(&server, cfg.socket, cfg.group, commands_line, commands_client_gone,
                    &commands) != 0) {
        serial_close(fd);
        return EXIT_FAILURE;
    }
    handshake_start(&hs, &link, &cfg, &model);

    int status = EXIT_SUCCESS;
    for (;;) {
        // Until reconnecting exists (build plan step 13), a link drop ends
        // the daemon; a failed handshake always does, since retrying won't
        // change the outcome
        if (!link_up(&link) || hs.status == HANDSHAKE_FAILED) {
            model_link_down(&model);
            status = EXIT_FAILURE;
            break;
        }

        int timeout = link_poll_timeout(&link);
        int retry = handshake_poll_timeout(&hs);
        if (retry >= 0 && (timeout < 0 || retry < timeout))
            timeout = retry;
        struct timespec ts = {.tv_sec = timeout / 1000, .tv_nsec = (timeout % 1000) * 1000000L};
        struct pollfd pfds[2 + SERVER_MAX_CLIENTS];
        pfds[0] = (struct pollfd){.fd = fd, .events = link_poll_events(&link)};
        size_t count = 1 + server_poll_fill(&server, &pfds[1]);

        int n = ppoll(pfds, count, timeout < 0 ? NULL : &ts, &wait_mask);
        if (n < 0 && errno != EINTR) {
            log_error("poll: %s", strerror(errno));
            status = EXIT_FAILURE;
            break;
        }
        if (stop_signal != 0) {
            // Stopping motion first comes with build plan step 14 (D-R2);
            // closing the port stops it too, as a link loss (F-R22)
            log_info("%s: closing the link", strsignal(stop_signal));
            break;
        }
        if (n > 0) {
            link_handle(&link, pfds[0].revents);
            server_poll_handle(&server, &pfds[1]);
        }
        link_tick(&link);
        handshake_tick(&hs);
    }

    server_close(&server);
    serial_close(fd);
    return status;
}
