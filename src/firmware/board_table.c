// Board table data. Pure C, so host tests can link it.
#include "board.h"

// Dev board: bare RPi Pico on the bench (pin map in docs/build-plan.md)
const board_stepper_t board_steppers[] = {
    {
        .oid        = "m0",
        .step_pin   = 2,
        .dir_pin    = 3,
        .enable_pin = 4,
        .ms_pin     = {5, 6, 7},
        .sleep_pin  = 8,
        .reset_pin  = 9,
    },
};
const size_t board_stepper_count = sizeof board_steppers / sizeof board_steppers[0];
