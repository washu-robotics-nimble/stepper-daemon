#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Hardware layout of this board build (F-R34, F-R35). Fixed at build time.
//
// NEVER rename or reuse an existing oid: clients and scripts address
// objects by it. Retire an object by deleting its entry; a new object gets
// a new oid.

// One state machine per stepper in each of the two PIO blocks: the step
// generator, and its step counter (stepgen.c)
#define BOARD_MAX_STEPPERS 4

// Safe built-in settings, used until the host overrides them (F-R34)
typedef struct {
    uint32_t speed;           // full steps/s
    uint32_t accel;           // full steps/s^2
    uint32_t pulse_width_us;  // STEP high time
    uint32_t microsteps;      // 1, 2, 4, 8 or 16
    bool     dir_forward_high; // dir_polarity "H": DIR high for positive moves
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

// Driver ENABLE of stepper i, on host command (F-R10). Enabling first wakes
// the driver if it is asleep or in reset: RESET and SLEEP high, then the
// A4988's 1 ms wake-up time, then ENABLE low. Disabling only drives ENABLE
// high; SLEEP and RESET stay high.
void board_set_enable(size_t i, bool on);

// MS1..MS3 of stepper i for 1, 2, 4, 8 or 16 microsteps (F-R10). Only while
// the axis is idle.
void board_set_microsteps(size_t i, uint32_t microsteps);

#endif
