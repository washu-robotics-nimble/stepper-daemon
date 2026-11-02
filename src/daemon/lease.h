#ifndef LEASE_H
#define LEASE_H

#include <stdbool.h>
#include <stdint.h>

// The lease (D-R13, D-R16–D-R18): one for the whole daemon, held by at most
// one client. Pure C; the time comes from the caller. Clients are opaque
// non-zero keys.

typedef enum {
    CLASS_READ,         // any client, any time (D-R16)
    CLASS_SESSION,      // lease handling; any client, any time (D-R16)
    CLASS_EMERGENCY,    // any client, any time (D-R17)
    CLASS_WRITE,        // only the lease holder (D-R18)
} command_class_t;

typedef struct {
    uint64_t holder;        // 0: free
    uint64_t expires;       // ms
    uint32_t timeout_ms;
} lease_t;

void lease_init(lease_t *l, uint32_t timeout_ms);

// The holder, after expiry is applied; 0: free
uint64_t lease_holder(lease_t *l, uint64_t now);

// lease: take it, or renew it if client holds it already. False if another
// client holds it.
bool lease_take(lease_t *l, uint64_t client, uint64_t now);

// renew: false if client doesn't hold it (never taken, or expired)
bool lease_renew(lease_t *l, uint64_t client, uint64_t now);

// release, or the client disconnected: frees it if client holds it
void lease_release(lease_t *l, uint64_t client);

// May a command of this class from client run now? Only lease and renew
// renew the lease (D-R18); this doesn't.
bool lease_check(lease_t *l, command_class_t cls, uint64_t client, uint64_t now);

#endif
