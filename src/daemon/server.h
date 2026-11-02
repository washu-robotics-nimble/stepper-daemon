#ifndef SERVER_H
#define SERVER_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

// Client server: the Unix socket (D-R12), client connections, and their
// newline-delimited framing (D-R40). What a line means is up to the line
// callback (commands.c).

#define SERVER_MAX_CLIENTS 8
#define SERVER_LINE_MAX    4096     // including the '\n' (D-R40)
#define SERVER_TX_MAX      65536    // unread output; a client over it is dropped

// A client, as the line callback gets it. Stays valid to send to after the
// client is gone: the message is then dropped.
typedef struct {
    int index;
    uint32_t gen;
} server_client_t;

// One line from a client, without its '\n' and NUL-terminated; line is NULL
// for an over-length line, which was dropped
typedef void (*server_line_fn)(void *ctx, server_client_t client, char *line, size_t len);

typedef struct {
    int fd;                     // -1: free slot
    uint32_t gen;               // changes with every new client in the slot
    unsigned id;                // for messages
    unsigned pending;           // requests still to be answered (server_hold())
    bool closing;               // the client shut down its sending side; closed
                                // once nothing is pending or unsent
    size_t rx_len;
    bool rx_overflow;           // dropping an over-length line, up to its '\n'
    char rx[SERVER_LINE_MAX];
    size_t tx_len;
    char tx[SERVER_TX_MAX];
} server_conn_t;

typedef struct {
    int listen_fd;
    char path[108];
    unsigned next_id;
    server_line_fn on_line;
    void *ctx;
    int poll_map[SERVER_MAX_CLIENTS];   // pollfd order -> slot, from server_poll_fill()
    size_t poll_count;
    server_conn_t conns[SERVER_MAX_CLIENTS];
} server_t;

// Create the socket at path, mode 0660, owned by group (a warning if that
// group doesn't exist or can't be set). An old socket file at path is
// removed: holding the MCU's device already ensures that no other nimhubd
// runs (D-R1). The directory isn't created; systemd creates it (D-R12).
// Returns 0, or -1 (logged).
int server_open(server_t *s, const char *path, const char *group,
                server_line_fn on_line, void *ctx);

// Close every client and the socket, and remove the socket file
void server_close(server_t *s);

// For poll(): fill pfd with the listening socket and every client (at most
// 1 + SERVER_MAX_CLIENTS entries); returns how many
size_t server_poll_fill(server_t *s, struct pollfd *pfd);

// After poll(), with the same entries
void server_poll_handle(server_t *s, const struct pollfd *pfd);

// Send msg to a client as one line; dropped if the client is gone
void server_send(server_t *s, server_client_t client, const cJSON *msg);

// A request of the client gets its reply later (D-R41). A client that shuts
// down its sending side (e.g. socat at the end of its input) is kept until
// every request it holds is released and its output is sent.
void server_hold(server_t *s, server_client_t client);
void server_release(server_t *s, server_client_t client);

// The client's id, for messages; 0 if it is gone
unsigned server_client_id(const server_t *s, server_client_t client);

#endif
