#include "dummy_stepgen.h"

#include <assert.h>
#include <string.h>
#include "board.h"

dummy_axis_t dummy_axes[BOARD_MAX_STEPPERS];

void dummy_stepgen_reset(void)
{
    memset(dummy_axes, 0, sizeof dummy_axes);
}

void stepgen_init(void)
{
    dummy_stepgen_reset();
}

uint32_t stepgen_clock_hz(void)
{
    return DUMMY_CLOCK_HZ;
}

void stepgen_set_high(size_t axis, uint32_t cycles)
{
    assert(!dummy_axes[axis].busy);
    assert(cycles >= STEPGEN_HIGH_OVERHEAD);
    dummy_axes[axis].high = cycles;
}

void stepgen_set_dir(size_t axis, bool level)
{
    // DIR must not change during a move (F-R4 hold time)
    assert(!dummy_axes[axis].busy);
    dummy_axes[axis].dir = level;
}

bool stepgen_push(size_t axis, uint32_t word)
{
    dummy_axis_t *a = &dummy_axes[axis];
    assert(a->nwords < DUMMY_MAX_WORDS);
    a->words[a->nwords++] = word;
    a->busy = true;
    return true;
}

bool stepgen_idle(size_t axis)
{
    return !dummy_axes[axis].busy;
}
