#include "linebuf.h"

void linebuf_reset(linebuf_t *lb)
{
    lb->len = 0;
    lb->overflow = false;
    lb->done = false;
}

linebuf_result_t linebuf_put(linebuf_t *lb, char c)
{
    if (lb->done)
        linebuf_reset(lb);

    if (c == '\n') {
        if (lb->overflow) {
            linebuf_reset(lb);
            return LINEBUF_TOO_LONG;
        }
        lb->buf[lb->len] = '\0';
        lb->done = true;
        return LINEBUF_LINE;
    }

    if (lb->overflow)
        return LINEBUF_NONE;

    // Keep one byte for the '\n' (counted in LINEBUF_MAX), stored as NUL
    if (lb->len >= LINEBUF_MAX - 1) {
        lb->overflow = true;
        return LINEBUF_NONE;
    }

    lb->buf[lb->len++] = c;
    return LINEBUF_NONE;
}
