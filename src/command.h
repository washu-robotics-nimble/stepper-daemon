#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>
#include <stdint.h>
#include "motor.h"

// Execution result code
typedef enum {
    CMD_OK = 0,
    CMD_ERR_SYNTAX,
    CMD_ERR_PARAM,
    CMD_ERR_MOTOR,
    CMD_ERR_UNKNOWN,
    CMD_ERR_NOT_IMPL,
    CMD_ERR_BUSY,
} command_result_t;

// Command type code
typedef enum {
    CMD_TYPE_UNKNOWN,
    CMD_TYPE_REL,
    CMD_TYPE_ABS,
    CMD_TYPE_STOP,
    CMD_TYPE_SPEED,
    CMD_TYPE_MICRO,
    CMD_TYPE_POS,
    CMD_TYPE_STATUS,
    CMD_TYPE_ZERO,
    CMD_TYPE_HOME,
    CMD_TYPE_LOCK,
    CMD_TYPE_UNLOCK,
    CMD_TYPE_HELP,
    CMD_TYPE_QUIT,
} command_type_t;

// Parsed command
typedef struct {
    command_type_t type;
    union {
        int32_t rel_steps;
        int32_t abs_pos;
        int speed;
        int microsteps;
    } args;
} command_t;

// Execution status (for status checking)
typedef struct {
    command_t current_cmd; // Last/executing command
    command_result_t last_result;
    char last_error[128];
} command_status_t;

// Initialize command parser
int command_init(void);

// Parse a command (thread safe)
command_result_t command_parse(const char *line, command_t *cmd,
                               char *errbuf, size_t errbuf_size);

// Issue command (nonblocking)
command_result_t command_run(motor_t *motor, const command_t *cmd,
                             char *response, size_t resp_size);

// Issue command (blocking)
command_result_t command_run_blocking(motor_t *motor, const command_t *cmd,
                                      char *response, size_t resp_size);

// Get command status (thread safe copy)
void command_get_status(command_status_t *status);

// Get help string
const char *command_help(void);

#endif