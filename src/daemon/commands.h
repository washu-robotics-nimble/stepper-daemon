#ifndef COMMANDS_H
#define COMMANDS_H

#include <stddef.h>
#include "handshake.h"
#include "lease.h"
#include "link.h"
#include "model.h"
#include "server.h"

// The client protocol (D-R40–D-R43) and its commands: the one dispatch path
// for every client. Commands that need the MCU send a host-link request and
// reply from its reply; reads are answered from the machine model (D-R20).

typedef struct {
    link_t *link;
    model_t *model;
    const handshake_t *hs;
    server_t *server;
    lease_t lease;
} commands_t;

void commands_init(commands_t *c, link_t *link, model_t *model,
                   const handshake_t *hs, server_t *server, uint32_t lease_timeout_ms);

// A server_line_fn: ctx is the commands_t
void commands_line(void *ctx, server_client_t client, char *line, size_t len);

// A server_close_fn: a client is gone, and its lease with it (D-R18)
void commands_client_gone(void *ctx, server_client_t client);

#endif
