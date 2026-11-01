#ifndef STEPGEN_H
#define STEPGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "planner.h"

// Step pulse output, one PIO state machine per board stepper (stepgen.pio),
// fed by DMA from words the planner writes on core 1. This header is pure
// C; stepgen.c implements it on the RP2040, and the host tests use a dummy
// (tests/dummy_stepgen.c).

// Cycles the PIO program adds to the delays it is given (stepgen.pio)
#define STEPGEN_HIGH_OVERHEAD    2   // high time = high delay + 2
#define STEPGEN_REPEAT_OVERHEAD  3   // low time in a repeat word = low delay + 3
#define STEPGEN_SINGLE_OVERHEAD  6   // low time of a single word = low delay + 6

// Largest step count of one repeat word
#define STEPGEN_MAX_REPEAT (1u << 31)

// First word of a repeat pair: count steps (1 .. STEPGEN_MAX_REPEAT),
// followed by a word holding the raw low delay
static inline uint32_t stepgen_repeat_word(uint32_t count)
{
    return ((count - 1) << 1) | 1u;
}

// A single step with the given low delay (below 2^31)
static inline uint32_t stepgen_single_word(uint32_t low_delay)
{
    return low_delay << 1;
}

// Set up every board stepper's state machine, STEP low and idle, and start
// core 1
void stepgen_init(void);

// State machine clock, in Hz
uint32_t stepgen_clock_hz(void);

// Set the STEP high time, in cycles (>= STEPGEN_HIGH_OVERHEAD). Only while
// the axis is idle.
void stepgen_set_high(size_t axis, uint32_t cycles);

// Set the DIR pin level. Returns after the A4988 setup time (F-R4), so a
// step pushed afterwards is safe. Only while the axis is idle.
void stepgen_set_dir(size_t axis, bool level);

// Start a move: core 1 runs the planned move (planner_init() done) and feeds
// its words to the state machine. Only while the axis is idle.
void stepgen_start(size_t axis, const planner_t *plan);

// True when the last move has been fully emitted, including the low time of
// its last step
bool stepgen_idle(size_t axis);

// Stop the move now (F-R21, F-R24): no step starts after this, and a pulse
// already high is held for the full high time, so the driver sees it. The
// axis is idle on return. Returns the number of steps the move emitted,
// counted on the STEP pin (F-R1). On an idle axis it changes nothing and
// returns the last move's count.
uint32_t stepgen_stop(size_t axis);

// Steps the current (or last) move has emitted so far, counted on the STEP
// pin (F-R1)
uint32_t stepgen_emitted(size_t axis);

// Number of times the state machine has run out of words in the middle of a
// move (F-R3): each is a late step. Counts up from 0 at boot and never
// resets; the caller keeps the last value it has seen.
uint32_t stepgen_underruns(size_t axis);

#endif
