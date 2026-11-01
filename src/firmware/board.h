#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>
#include <stdint.h>

// Hardware layout of this board build (F-R34, F-R35). Fixed at build time.
//
// NEVER rename or reuse an existing oid: clients and scripts address
// objects by it. Retire an object by deleting its entry; a new object gets
// a new oid.

// One PIO block's state machines, one per stepper (stepgen.c)
#define BOARD_MAX_STEPPERS 4

// Safe built-in settings, used until the host overrides them (F-R34)
typedef struct {
    uint32_t speed;           // full steps/s
    uint32_t pulse_width_us;  // STEP high time
} board_motion_t;

typedef struct {
    const char *oid;
    uint8_t step_pin;
    uint8_t dir_pin;
    uint8_t enable_pin;     // A4988 ENABLE, active low
    uint8_t ms_pin[3];      // MS1..MS3
    uint8_t sleep_pin;      // A4988 SLEEP, active low
    uint8_t reset_pin;      // A4988 RESET, active low
    board_motion_t defaults;
} board_stepper_t;

extern const board_stepper_t board_steppers[];
extern const size_t board_stepper_count;

// Drive every driver output to its safe state (F-R23). Call first in main.
void board_safe_init(void);

#endif
