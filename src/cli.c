#define _DEFAULT_SOURCE // Make struct winsize visible

#include "cli.h"
#include "command.h"
#include "status.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>

// ANSI: ESC[2K erase line, ESC[0m reset, ESC7/ESC8 save/restore cursor
#define CLEAR_LINE "\r\033[2K"
#define CUR_UP     "\033[1A"
#define CUR_DOWN   "\033[1B"   // unlike \n this never scrolls
#define CUR_SAVE   "\0337"
#define CUR_RESTORE "\0338"
#define BAR_INDENT 1           // Leading space before the status text
#define SGR_RESET  "\033[0m"

// How often to repaint the bar while idle at the prompt
#define REFRESH_MS 500

static motor_t *g_motor = NULL;
static volatile int g_running = 1;
static int g_show_status = 1;
static status_watch_t g_watch;   // Tracks what the bar currently shows

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

int cli_init(motor_t *motor)
{
    if (!motor) return -1;
    g_motor = motor;
    g_running = 1;
    signal(SIGINT, sigint_handler);
    return 0;
}

void cli_stop(void)
{
    g_running = 0;
}

void cli_set_show_status(int enable)
{
    g_show_status = enable;
}

static int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
}

// Bar colours follow the motor state
static const char *bar_style(status_state_t state)
{
    switch (state) {
        case STATUS_RUNNING: return "\033[48;5;23m\033[38;5;159m";  // teal
        case STATUS_ERROR:   return "\033[48;5;52m\033[38;5;217m";  // red
        default:             return "\033[48;5;238m\033[38;5;252m"; // grey
    }
}

// Paint the bar over the line the cursor is currently on
static void put_bar(const status_t *st)
{
    char text[256];
    if (status_format(st, text, sizeof(text)) < 0) {
        snprintf(text, sizeof(text), "status unavailable");
    }

    // Stop one column short of the edge so the bar never wraps
    int avail = term_width() - 1 - BAR_INDENT;
    if (avail < 0) avail = 0;
    int len = (int)strlen(text);
    if (len > avail) len = avail;

    printf("%s %.*s%*s" SGR_RESET,
           bar_style(status_state(st)), avail, text, avail - len, "");
}

// Open a fresh bar line below the cursor,
// then come back up and print the prompt.
static void draw_bar_and_prompt(void)
{
    status_t st;
    status_watch_poll(&g_watch, g_motor, &st);   // repaint regardless of change

    printf("\n" CLEAR_LINE);             // step down onto the bar line
    put_bar(&st);
    printf(CUR_UP CLEAR_LINE "> ");      // back up to the prompt line
    fflush(stdout);
}

// Repaint the bar while sitting at the prompt. The bar line already exists
// below us, so nothing here scrolls and the saved cursor stays valid:
// whatever the user has typed so far is left untouched.
static void refresh_bar(void)
{
    status_t st;
    if (!status_watch_poll(&g_watch, g_motor, &st)) return;

    printf(CUR_SAVE CUR_DOWN CLEAR_LINE);
    put_bar(&st);
    printf(CUR_RESTORE);
    fflush(stdout);
}

// Wait until stdin holds a line, repainting the bar meanwhile. This is what
// keeps the bar live: the motor thread advances position/moving on its own,
// so an idle prompt still has to poll. Returns 1 when stdin is readable,
// 0 if we should stop.
static int wait_for_input(void)
{
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = { 0, REFRESH_MS * 1000 };

        int rv = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (rv > 0) return 1;
        if (rv < 0 && errno != EINTR) return 0;  // select is not restarted on EINTR
        refresh_bar();
    }
    return 0;
}

// Wipe the prompt line and the bar below it
static void erase_prompt_and_bar(void)
{
    printf(CLEAR_LINE "\n" CLEAR_LINE CUR_UP);
}

void cli_run(void)
{
    char line[256];
    char response[512];
    int use_bar = g_show_status && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    status_watch_init(&g_watch);

    // Unbuffered stdin: select() only sees the file descriptor, so stdio
    // must not hold a read-ahead line that would never wake the poll loop.
    if (use_bar) setvbuf(stdin, NULL, _IONBF, 0);

    printf("Motor CLI. Type 'help' for commands.\n");

    while (g_running) {
        if (use_bar) {
            draw_bar_and_prompt();
            if (!wait_for_input()) {     // interrupted or cli_stop()
                erase_prompt_and_bar();
                break;
            }
        } else {
            if (g_show_status) {
                char status[256];
                if (status_get_string(g_motor, status, sizeof(status)) >= 0) {
                    printf("[%s]\n", status);
                }
            }
            printf("> ");
            fflush(stdout);
        }

        if (!fgets(line, sizeof(line), stdin)) {
            if (use_bar) erase_prompt_and_bar();   // EOF
            break;
        }

        // Enter moved the cursor onto the bar line; wipe it before printing
        if (use_bar) printf(CLEAR_LINE);

        // Remove line breaks
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }

        if (strlen(line) == 0) continue;

        command_t cmd;
        char errbuf[128];
        command_result_t parse_ret = command_parse(line, &cmd, errbuf, sizeof(errbuf));
        if (parse_ret != CMD_OK) {
            printf("Error: %s\n", errbuf);
            continue;
        }

        if (cmd.type == CMD_TYPE_QUIT) {
            printf("Exiting.\n");
            break;
        }

        response[0] = '\0';
        command_result_t run_ret = command_run(g_motor, &cmd, response, sizeof(response));
        if (response[0]) {
            printf("%s\n", response);
        }
        if (run_ret != CMD_OK) {
            // Error message is recorded in command_run
            // TODO: Optional print here
        }
    }

    printf("CLI stopped.\n");
}