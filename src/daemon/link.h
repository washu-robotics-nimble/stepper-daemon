#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

// Host link to the MCU (F-R31): newline-delimited JSON frames, seq numbers,
// matching replies to requests, the reply timeout (D-R30) and keepalive
// pings (F-R22). Single-threaded: the caller polls the fd and calls
// link_handle() and link_tick().

#define LINK_FRAME_MAX   1024   // the firmware's line cap, including '\n'
#define LINK_RX_MAX      4096   // longest line accepted from the MCU
#define LINK_TX_MAX      16384  // frames waiting for the device to take them
#define LINK_MAX_PENDING 32     // requests waiting for their reply

// Called with the reply to a request (ack true or false)
typedef void (*link_reply_fn)(void *ctx, const cJSON *reply);
// Called with each event from the MCU
typedef void (*link_event_fn)(void *ctx, const cJSON *evt);

typedef struct {
    uint32_t seq;
    uint64_t deadline;      // ms (monotonic_ms())
    link_reply_fn fn;       // NULL: an error reply is only logged
    void *ctx;
    char cmd[32];           // for messages
} link_pending_t;

typedef struct {
    int fd;
    bool up;
    char down_reason[160];
    uint32_t seq;               // of the last request
    uint32_t reply_timeout_ms;
    uint32_t keepalive_ms;      // ping when nothing was sent for this long; 0: off
    uint64_t last_tx;
    link_event_fn on_event;
    void *event_ctx;
    size_t pending_count;
    link_pending_t pending[LINK_MAX_PENDING];
    size_t rx_len;
    bool rx_overflow;           // dropping an over-length line, up to its '\n'
    char rx[LINK_RX_MAX];
    size_t tx_len;
    char tx[LINK_TX_MAX];
} link_t;

// Monotonic time in ms, for all link timing
uint64_t monotonic_ms(void);

// Start on an open device; seq starts over (F-R31)
void link_init(link_t *l, int fd, uint32_t reply_timeout_ms,
               link_event_fn on_event, void *event_ctx);

// Send a request: req (an object without seq) gets the next seq. fn gets the
// reply. False if it can't be sent (logged): the link is down, or the frame
// is too long, or too much is waiting.
bool link_request(link_t *l, const cJSON *req, link_reply_fn fn, void *ctx);

// A request with only cmd and, unless NULL, oid
cJSON *link_new_request(const char *cmd, const char *oid);

void link_set_keepalive(link_t *l, uint32_t ms);

// For poll(): the events to wait for, and the time until link_tick() has
// something to do (ms, -1: nothing)
short link_poll_events(const link_t *l);
int link_poll_timeout(const link_t *l);

// After poll() returned revents for the fd
void link_handle(link_t *l, short revents);

// Reply timeouts and keepalive pings
void link_tick(link_t *l);

// Down after a read or write error, a hangup, or a missing reply (D-R30).
// The reason is logged once.
static inline bool link_up(const link_t *l) { return l->up; }

#endif
