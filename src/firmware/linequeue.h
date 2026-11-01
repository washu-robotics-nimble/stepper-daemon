#ifndef LINEQUEUE_H
#define LINEQUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include "linebuf.h"

// Received lines waiting for dispatch, in arrival order. Lines are queued
// between assembly and dispatch so that an estop can run ahead of them
// (F-R24). Pure C, no SDK dependency.

#define LINEQUEUE_SLOTS 8

typedef struct {
    char   buf[LINEQUEUE_SLOTS][LINEBUF_MAX];
    size_t len[LINEQUEUE_SLOTS];
    bool   too_long[LINEQUEUE_SLOTS];   // an over-length line, dropped
    size_t head;                        // oldest entry
    size_t count;
} linequeue_t;

void linequeue_reset(linequeue_t *q);

static inline bool linequeue_full(const linequeue_t *q)
{
    return q->count == LINEQUEUE_SLOTS;
}

static inline bool linequeue_empty(const linequeue_t *q)
{
    return q->count == 0;
}

// Append a complete line (len bytes, without '\n'), or the marker of an
// over-length line. Only when not full.
void linequeue_push(linequeue_t *q, const char *line, size_t len);
void linequeue_push_too_long(linequeue_t *q);

// The oldest entry, NUL-terminated; valid until it is dropped. Returns NULL
// for an over-length marker. Only when not empty.
const char *linequeue_front(const linequeue_t *q, size_t *len);

// Remove the oldest entry. Only when not empty.
void linequeue_drop(linequeue_t *q);

#endif
