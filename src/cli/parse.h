#ifndef PARSE_H
#define PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

// Typed lines to client requests (D-R14, D-R40). Pure C.
//
//   <cmd>                 daemon command, e.g. estop, devices
//   <dev> <cmd> [arg]     device command; arg goes into the field the
//                         command takes (rel: steps, abs: pos, else value)
//   {...}                 raw JSON request, sent as it is
//   help, quit            local

typedef enum {
    CLI_EMPTY,      // blank line
    CLI_HELP,
    CLI_QUIT,
    CLI_REQUEST,    // *req is the request, without seq
    CLI_RAW,        // *req is a raw JSON request, sent as it is (seq kept)
    CLI_ERROR,      // err says why
} cli_kind_t;

cli_kind_t cli_parse(const char *line, cJSON **req, char *err, size_t errlen);

// A write command (D-R13), which needs the lease (D-R18)
bool cli_is_write(const cJSON *req);

const char *cli_help(void);

// For completion: NULL-terminated lists
extern const char *const cli_daemon_commands[];
extern const char *const cli_motor_commands[];

#endif
