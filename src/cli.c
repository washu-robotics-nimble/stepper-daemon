#define _DEFAULT_SOURCE // Make struct winsize visible

#include "cli.h"
#include "command.h"
#include "status.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>

// ANSI: ESC[2K erase line, ESC[1A cursor up, ESC[0m reset
#define CLEAR_LINE "\r\033[2K"
#define BAR_STYLE  "\033[48;5;238m\033[38;5;252m"
#define BAR_LABEL  "\033[1m MOTOR \033[22m"
#define SGR_RESET  "\033[0m"

static motor_t *g_motor = NULL;
static volatile int g_running = 1;
static int g_show_status = 1;

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

// Draw the status bar on the line below the prompt, 
// then come back up and print the prompt.
static void draw_bar_and_prompt(void)
{
    char status[256];
    if (status_get_string(g_motor, status, sizeof(status)) < 0) {
        snprintf(status, sizeof(status), "status unavailable");
    }

    // Stop one column short of the edge so the bar never wraps
    int width = term_width() - 1;
    int avail = width - 8;               // " MOTOR "
    if (avail < 0) avail = 0;
    if ((int)strlen(status) > avail) status[avail] = '\0';
    int pad = avail - (int)strlen(status);

    printf("\n" CLEAR_LINE);             // step down onto the bar line
    printf(BAR_STYLE BAR_LABEL " %s%*s" SGR_RESET, status, pad, "");
    printf("\033[1A" CLEAR_LINE "> ");   // back up to the prompt line
    fflush(stdout);
}

void cli_run(void)
{
    char line[256];
    char response[512];
    int use_bar = g_show_status && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    printf("Motor CLI. Type 'help' for commands.\n");

    while (g_running) {
        if (use_bar) {
            draw_bar_and_prompt();
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
            // EOF: erase the prompt and the bar below it
            if (use_bar) printf(CLEAR_LINE "\n" CLEAR_LINE "\033[1A");
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