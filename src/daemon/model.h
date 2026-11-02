#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cJSON.h"
#include "config.h"

// Machine model: the devices the firmware reported (F-R35, D-R43) and the
// latest state the firmware reported for each (D-R20). It changes only on
// firmware reports: get_state replies (at the end of each handshake, and
// after every accepted command that changes the state) and events. That is
// enough since nimhubd is the MCU's only host (D-R1). Reads (pos, status) are
// answered from here, without asking the firmware. Nothing here is the
// daemon's own guess: while the link is down, reads get link_down instead
// (D-R20, D-R30).

#define MODEL_MAX_DEVICES 32

// Settings clients can change at runtime (speed, accel, micro)
typedef enum {
    MODEL_SPEED,
    MODEL_ACCEL,
    MODEL_MICRO,
    MODEL_SETTINGS,
} model_setting_t;

typedef struct {
    char oid[CONFIG_OID_MAX];
    char type[32];
    bool known;             // stepper state filled by get_state
    // Stepper state
    int32_t pos;            // full steps
    bool pos_valid;
    bool moving;
    bool locked;            // driver enabled
    // The state last sent to watching clients (D-R21)
    bool sent;
    int32_t sent_pos;
    bool sent_pos_valid, sent_moving, sent_locked;
} model_device_t;

// Settings changed by clients and accepted by the firmware, by oid. A
// reconnect re-sends them instead of the INI's (D-R3); they are lost when
// nimhubd stops.
typedef struct {
    char oid[CONFIG_OID_MAX];
    bool has[MODEL_SETTINGS];
    int32_t value[MODEL_SETTINGS];
} model_runtime_t;

typedef struct {
    size_t count;
    model_device_t devices[MODEL_MAX_DEVICES];
    size_t runtime_count;           // kept across model_reset()
    model_runtime_t runtime[MODEL_MAX_DEVICES];
} model_t;

// Forget the devices and their state, for a new handshake; the runtime
// settings stay
void model_reset(model_t *m);

// Add a device from list_objects; NULL if there are too many
model_device_t *model_add(model_t *m, const char *oid, const char *type);

model_device_t *model_find(model_t *m, const char *oid);

static inline bool model_is_stepper(const model_device_t *d)
{
    return strcmp(d->type, "stepper") == 0;
}

// A get_state reply for d
void model_set_state(model_device_t *d, const cJSON *reply);

// An event from the MCU (move_start, position, move_end)
void model_event(model_t *m, const cJSON *evt);

// The MCU accepted a setting for d (set_motion)
void model_set_setting(model_t *m, const model_device_t *d, model_setting_t s, int32_t value);

// The runtime settings for oid, or NULL if none was set
const model_runtime_t *model_runtime(const model_t *m, const char *oid);

// True if d's reported state (pos, pos_valid, moving, locked) has changed
// since the last call that returned true, which is then the new reference.
// False until get_state has filled it.
bool model_take_change(model_device_t *d);

#endif
