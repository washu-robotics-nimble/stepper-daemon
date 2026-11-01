#ifndef LINEBUF_H
#define LINEBUF_H

#include <stdbool.h>
#include <stddef.h>

// Line assembler for the host link (F-R31). Pure C, no SDK dependency.

// Maximum line length on the wire, including the terminating '\n'
#define LINEBUF_MAX 1024

typedef enum {
    LINEBUF_NONE,       // no complete line yet
    LINEBUF_LINE,       // buf holds a complete line, NUL-terminated, without '\n'
    LINEBUF_TOO_LONG,   // an over-length line ended and was dropped
} linebuf_result_t;

typedef struct {
    char   buf[LINEBUF_MAX];
    size_t len;         // length of the line in buf, valid after LINEBUF_LINE
    bool   overflow;    // current line is over-length; dropping until '\n'
    bool   done;        // buf holds a returned line; start over on next byte
} linebuf_t;

// Discard any partial line (startup, reconnect)
void linebuf_reset(linebuf_t *lb);

// Feed one received byte. After LINEBUF_LINE, buf stays valid until the
// next call.
linebuf_result_t linebuf_put(linebuf_t *lb, char c);

#endif
