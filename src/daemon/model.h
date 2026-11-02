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

typedef struct {
    char oid[CONFIG_OID_MAX];
    char type[32];
    bool known;             // stepper state filled by get_state
    // Stepper state
    int32_t pos;            // full steps
    bool pos_valid;
    bool moving;
    bool locked;            // driver enabled
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

// The link is down (D-R30): every position void
void model_link_down(model_t *m);

#endif
