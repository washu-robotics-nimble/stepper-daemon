#include "cli.h"
#include "command.h"
#include "status.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

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

void cli_run(void)
{
    char line[256];
    char response[512];

    printf("Motor CLI. Type 'help' for commands.\n");

    while (g_running) {
        if (g_show_status) {
            char status[256];
            if (status_get_string(g_motor, status, sizeof(status)) >= 0) {
                printf("[%s]\n", status);
            }
        }

        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;  // EOF
        }

        // Remove line return
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