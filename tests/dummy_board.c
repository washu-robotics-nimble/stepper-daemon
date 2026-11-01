// Dummy driver pins for host tests: records ENABLE instead of driving it
// (src/firmware/board.h). The board table itself is the real one.
#include "dummy_board.h"

bool dummy_enabled[BOARD_MAX_STEPPERS];

void board_set_enable(size_t i, bool on)
{
    dummy_enabled[i] = on;
}
