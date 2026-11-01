#ifndef STEPGEN_H
#define STEPGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Step pulse output, one PIO state machine per board stepper (stepgen.pio).
// This header is pure C; stepgen.c implements it on the RP2040, and the
// host tests use a dummy (tests/dummy_stepgen.c).

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

// Set up every board stepper's state machine, STEP low and idle
void stepgen_init(void);

// State machine clock, in Hz
uint32_t stepgen_clock_hz(void);

// Set the STEP high time, in cycles (>= STEPGEN_HIGH_OVERHEAD). Only while
// the axis is idle.
void stepgen_set_high(size_t axis, uint32_t cycles);

// Set the DIR pin level. Returns after the A4988 setup time (F-R4), so a
// step pushed afterwards is safe. Only while the axis is idle.
void stepgen_set_dir(size_t axis, bool level);

// Queue one word. Returns false if the FIFO is full.
bool stepgen_push(size_t axis, uint32_t word);

// True when every queued step has been emitted, including its low time.
// The caller must push whole words (a repeat pair together), or a gap
// between the two words of a pair could look idle.
bool stepgen_idle(size_t axis);

#endif
