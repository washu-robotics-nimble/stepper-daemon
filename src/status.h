#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>
#include <stdint.h>
#include "motor.h"
#include "command.h"

// Coarse state, derived from a snapshot
typedef enum {
    STATUS_IDLE,
    STATUS_RUNNING,
    STATUS_ERROR,
} status_state_t;

// Consistent snapshot of everything a front-end reports. Taking a snapshot
// once keeps a single report self-consistent: the motor thread updates
// position/moving asynchronously, so re-reading per field can mix states.
typedef struct {
    int              moving;
    int              pos_valid;
    int32_t          position;        // Meaningful only when pos_valid
    int              accel;           // Accel limit, 0 = ramping off
    command_t        current_cmd;     // Last/executing command
    command_result_t last_result;
    char             last_error[128];
} status_t;

// Change tracker. One per front-end (CLI bar, serial poll, websocket push),
// each with its own idea of what it has already reported.
typedef struct {
    status_t last;
    int      primed;   // 0 until the first poll
} status_watch_t;

// Capture the current motor+command state
void status_capture(motor_t *motor, status_t *st);

// Derived state and its name
status_state_t status_state(const status_t *st);
const char *status_state_name(status_state_t state);

// True when two snapshots are identical in every reported field
int status_equal(const status_t *a, const status_t *b);

// Format a snapshot into a one-line string. Returns the length, or -1.
int status_format(const status_t *st, char *buf, size_t buf_size);

// Capture and format in one step (convenience wrapper)
int status_get_string(motor_t *motor, char *buf, size_t buf_size);

// Print status to stdout
void status_print(motor_t *motor);

// Begin watching; the first poll always reports a change
void status_watch_init(status_watch_t *w);

// Sample the state. Returns 1 if anything changed since the previous poll,
// 0 otherwise. When st is non-NULL it always receives the fresh snapshot,
// so a caller that must redraw unconditionally can ignore the return value.
int status_watch_poll(status_watch_t *w, motor_t *motor, status_t *st);

#endif
