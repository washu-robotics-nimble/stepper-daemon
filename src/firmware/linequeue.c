#include "linequeue.h"

#include <assert.h>
#include <string.h>

void linequeue_reset(linequeue_t *q)
{
    q->head = 0;
    q->count = 0;
}

static size_t tail(const linequeue_t *q)
{
    return (q->head + q->count) % LINEQUEUE_SLOTS;
}

void linequeue_push(linequeue_t *q, const char *line, size_t len)
{
    assert(!linequeue_full(q) && len < LINEBUF_MAX);
    size_t t = tail(q);
    memcpy(q->buf[t], line, len);
    q->buf[t][len] = '\0';
    q->len[t] = len;
    q->too_long[t] = false;
    q->count++;
}

void linequeue_push_too_long(linequeue_t *q)
{
    assert(!linequeue_full(q));
    q->too_long[tail(q)] = true;
    q->count++;
}

const char *linequeue_front(const linequeue_t *q, size_t *len)
{
    assert(!linequeue_empty(q));
    if (q->too_long[q->head])
        return NULL;
    *len = q->len[q->head];
    return q->buf[q->head];
}

void linequeue_drop(linequeue_t *q)
{
    assert(!linequeue_empty(q));
    q->head = (q->head + 1) % LINEQUEUE_SLOTS;
    q->count--;
}
