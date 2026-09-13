#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "config.h"
#include "motor.h"
#include "command.h"
#include "cli.h"

int main(int argc, char *argv[])
{
    const char *conf_file = "motor.ini";
    if (argc >= 2) conf_file = argv[1];

    motor_config_t cfg;
    if (config_load(conf_file, &cfg) != 0) {
        fprintf(stderr, "Failed to load config.\n");
        return EXIT_FAILURE;
    }

    motor_t motor;
    if (motor_init(&motor, &cfg) != MOTOR_OK) {
        fprintf(stderr, "Failed to init motor.\n");
        return EXIT_FAILURE;
    } else {
        printf("Motor initialized.\n");
    }

    if (command_init() != 0) {
        fprintf(stderr, "Failed to init command module.\n");
        motor_close(&motor);
        return EXIT_FAILURE;
    }

    if (cli_init(&motor) != 0) {
        fprintf(stderr, "Failed to init CLI.\n");
        motor_close(&motor);
        return EXIT_FAILURE;
    }

    cli_run();

    motor_close(&motor);
    return EXIT_SUCCESS;
}