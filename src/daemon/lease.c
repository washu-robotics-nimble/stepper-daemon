#include "lease.h"

void lease_init(lease_t *l, uint32_t timeout_ms)
{
    l->holder = 0;
    l->expires = 0;
    l->timeout_ms = timeout_ms;
}

uint64_t lease_holder(lease_t *l, uint64_t now)
{
    // Expired without renewal: free (a move it started goes on, D-R18)
    if (l->holder != 0 && now >= l->expires)
        l->holder = 0;
    return l->holder;
}

bool lease_take(lease_t *l, uint64_t client, uint64_t now)
{
    uint64_t holder = lease_holder(l, now);
    if (holder != 0 && holder != client)
        return false;
    l->holder = client;
    l->expires = now + l->timeout_ms;
    return true;
}

bool lease_renew(lease_t *l, uint64_t client, uint64_t now)
{
    if (lease_holder(l, now) != client)
        return false;
    l->expires = now + l->timeout_ms;
    return true;
}

void lease_release(lease_t *l, uint64_t client)
{
    if (l->holder == client)
        l->holder = 0;
}

bool lease_check(lease_t *l, command_class_t cls, uint64_t client, uint64_t now)
{
    if (cls != CLASS_WRITE)
        return true;
    return lease_holder(l, now) == client;
}
