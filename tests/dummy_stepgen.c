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

void stepgen_start(size_t axis, const planner_t *plan)
{
    dummy_axis_t *a = &dummy_axes[axis];
    assert(!a->busy);
    assert(plan->high == a->high);
    a->plan = *plan;
    a->starts++;
    a->busy = true;
}

bool stepgen_idle(size_t axis)
{
    return !dummy_axes[axis].busy;
}

uint32_t stepgen_stop(size_t axis)
{
    dummy_axis_t *a = &dummy_axes[axis];
    if (a->busy) {
        a->busy = false;
        a->stops++;
    }
    return a->emitted;
}

uint32_t stepgen_underruns(size_t axis)
{
    return dummy_axes[axis].underruns;
}

uint64_t dummy_plan_steps(size_t axis)
{
    planner_t p = dummy_axes[axis].plan;
    uint32_t w[64];
    uint64_t steps = 0;
    bool repeat = false;

    while (!planner_done(&p)) {
        size_t n = planner_fill(&p, w, 64);
        for (size_t i = 0; i < n; i++) {
            if (repeat)
                repeat = false;
            else if (w[i] & 1) {
                steps += (w[i] >> 1) + 1;
                repeat = true;
            } else
                steps++;
        }
    }
    return steps;
}
