#ifndef DUMMY_BOARD_H
#define DUMMY_BOARD_H

#include <stdbool.h>
#include "board.h"

// Driver ENABLE as last set by board_set_enable() (true: enabled)
extern bool dummy_enabled[BOARD_MAX_STEPPERS];

// Microsteps as last set by board_set_microsteps()
extern uint32_t dummy_microsteps[BOARD_MAX_STEPPERS];

#endif
