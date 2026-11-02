#ifndef CONN_H
#define CONN_H

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

// The connection to nimhubd's Unix socket (D-R12, D-R40)

#define CONN_LINE_MAX 65536     // longest line accepted from nimhubd

// Called for each line received: the line as received, and parsed (NULL if
// it isn't JSON)
typedef void (*conn_line_fn)(void *ctx, const char *line, const cJSON *msg);

typedef struct {
    int fd;                 // -1: not connected
    size_t rx_len;
    char rx[CONN_LINE_MAX];
} conn_t;

// Connect to the socket at path; false (errno set) if that fails
bool conn_open(conn_t *c, const char *path);

void conn_close(conn_t *c);

// Send msg as one line; false if the connection failed (then closed)
bool conn_send(conn_t *c, const cJSON *msg);

// Read what has arrived (call when the fd is readable), and pass on every
// complete line. False if nimhubd closed the connection (then closed).
bool conn_read(conn_t *c, conn_line_fn fn, void *ctx);

#endif
