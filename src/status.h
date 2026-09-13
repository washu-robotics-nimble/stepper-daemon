#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>
#include "motor.h"
#include "command.h"

// Format current status (motor+command) into string
int status_get_string(motor_t *motor, char *buf, size_t buf_size);

// Print status to stdout
void status_print(motor_t *motor);

#endif