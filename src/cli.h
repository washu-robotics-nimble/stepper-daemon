#ifndef CLI_H
#define CLI_H

#include "motor.h"

int cli_init(motor_t *motor);
void cli_run(void);
void cli_stop(void);
void cli_set_show_status(int enable);

#endif