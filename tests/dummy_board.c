// Dummy driver pins for host tests: records ENABLE and the microstep
// setting instead of driving them
// (src/firmware/board.h). The board table itself is the real one.
#include "dummy_board.h"

bool dummy_enabled[BOARD_MAX_STEPPERS];
uint32_t dummy_microsteps[BOARD_MAX_STEPPERS];

void board_set_enable(size_t i, bool on)
{
    dummy_enabled[i] = on;
}

void board_set_microsteps(size_t i, uint32_t microsteps)
{
    dummy_microsteps[i] = microsteps;
}
