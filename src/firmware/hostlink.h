#ifndef HOSTLINK_H
#define HOSTLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

// Host link protocol: frame decoding, dispatch and replies (F-R31, F-R33,
// F-R35). Pure C, no SDK dependency.

#define HOSTLINK_PROTO 1

// Writes one complete line, including its '\n'
typedef void (*hostlink_write_fn)(const char *line, size_t len);

// A command handler adds its result fields to reply, and returns NULL on
// success or an error code (message catalogue) on failure. index is the
// target object's position in its board table; global commands ignore it.
typedef const char *(*hostlink_cmd_fn)(const cJSON *req, cJSON *reply, size_t index);

// Command table entry; tables end with {NULL, NULL}
typedef struct {
    const char *name;
    hostlink_cmd_fn fn;
} hostlink_cmd_t;

void hostlink_init(hostlink_write_fn write);

// Handle one received line (without '\n'). len is the number of bytes
// received, so an embedded NUL can be detected.
void hostlink_handle_line(const char *line, size_t len);

// F-R33: true once the host has sent settings_done; false from boot
bool hostlink_settings_applied(void);

// F-R32: period of position events while moving, ms; 0 turns them off
uint32_t hostlink_report_period_ms(void);

// Read an optional integer field: true if absent, or present and in
// [min, max] (stored in *out); false if present but not such an integer
bool hostlink_get_opt_int32(const cJSON *req, const char *key, int32_t min,
                            int32_t max, int32_t *out);

// Link loss (F-R22). up: the USB device is mounted and not suspended, and
// the host has the port open (DTR). Losing the link stops all motion
// (stepper_link_lost()); when it returns, a link_recovered event is sent
// once. Call on every change, or every main loop pass.
void hostlink_set_connected(bool up);

// The time, for the F-R22 timeout (set_link timeout_ms). Call every main
// loop pass, before handling lines.
void hostlink_tick(uint32_t now_ms);

// True if the line is an estop command, which the receive path then handles
// ahead of the lines already queued (F-R24). Handle it as any other line.
bool hostlink_is_estop(const char *line, size_t len);

// Report a frame dropped before it could be decoded (rx_error event)
void hostlink_rx_error(const char *err);

// Send one message (reply or event) as a line
void hostlink_send(const cJSON *msg);

// Read an integer argument in int32 range. False if it is missing, not a
// number, or not an integer in range.
bool hostlink_get_int32(const cJSON *req, const char *key, int32_t *out);

#endif
