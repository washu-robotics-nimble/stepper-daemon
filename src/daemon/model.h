#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cJSON.h"
#include "config.h"

// Machine model: the devices the firmware reported (F-R35, D-R43) and the
// latest state the firmware reported for each (D-R20). It is filled from
// get_state once, at the end of the handshake, and kept up to date from
// events and from the replies to the commands nimhubd sends, which is enough
// since nimhubd is the MCU's only host (D-R1). Reads (pos, status) are
// answered from here, without asking the firmware.

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
    // Settings changed by clients and accepted by the firmware. They are
    // kept here so a reconnect re-sends them instead of the INI's (D-R3,
    // step 13), and are lost when nimhubd stops.
    bool has_setting[MODEL_SETTINGS];
    int32_t setting[MODEL_SETTINGS];
} model_device_t;

typedef struct {
    size_t count;
    model_device_t devices[MODEL_MAX_DEVICES];
} model_t;

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

// The MCU accepted a move for d: it is moving from now on, ahead of its
// move_start event
void model_move_accepted(model_device_t *d);

// The MCU accepted estop (F-R24): every driver disabled, every position void
void model_estop(model_t *m);

// The MCU accepted a setting for d (set_motion)
void model_set_setting(model_device_t *d, model_setting_t s, int32_t value);

// The MCU accepted set_position 0 for d: the position is valid
void model_zeroed(model_device_t *d);

// The MCU accepted set_enable for d. Disabling voids the position, since
// the motor can turn freely (firmware spec).
void model_enabled(model_device_t *d, bool enabled);

// The link is down (D-R30): every position void
void model_link_down(model_t *m);

#endif
