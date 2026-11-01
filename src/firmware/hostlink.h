#ifndef HOSTLINK_H
#define HOSTLINK_H

#include <stddef.h>

// Host link protocol: frame decoding, dispatch and replies (F-R31, F-R33,
// F-R35). Pure C, no SDK dependency.

#define HOSTLINK_PROTO 1

// Writes one complete line, including its '\n'
typedef void (*hostlink_write_fn)(const char *line, size_t len);

void hostlink_init(hostlink_write_fn write);

// Handle one received line (without '\n'). len is the number of bytes
// received, so an embedded NUL can be detected.
void hostlink_handle_line(const char *line, size_t len);

// Report a frame dropped before it could be decoded (rx_error event)
void hostlink_rx_error(const char *err);

#endif
