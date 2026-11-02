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
    server_t *server;
    lease_t lease;
    bool up;            // clients were told link_up, and not link_down since
                        // (D-R30): only then do device commands run and
                        // status events go out
    // Clients that sent watch (D-R21), by server slot
    bool watching[SERVER_MAX_CLIENTS];
    uint32_t watching_gen[SERVER_MAX_CLIENTS];
} commands_t;

void commands_init(commands_t *c, link_t *link, model_t *model,
                   server_t *server, uint32_t lease_timeout_ms);

// A server_line_fn: ctx is the commands_t
void commands_line(void *ctx, server_client_t client, char *line, size_t len);

// A server_close_fn: a client is gone, and its lease and watch with it
// (D-R18)
void commands_client_gone(void *ctx, server_client_t client);

// Send one status event to every watching client for each device whose
// reported state changed (D-R21, D-R22). Call after every firmware report
// that may change the model. Nothing is sent until the handshake is done.
void commands_broadcast(commands_t *c);

// Tell every client that the link went down (D-R30), or is up again after
// a handshake (link_up)
void commands_link_event(commands_t *c, bool up, const char *reason);

#endif
