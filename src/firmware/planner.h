#ifndef PLANNER_H
#define PLANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Per-step trapezoid planner (F-R5): turns one move into step generator
// words (stepgen.h), a chunk at a time. Ramp steps are one single word
// each, with periods from David Austin's integer recurrence; the cruise is
// one repeat pair per STEPGEN_MAX_REPEAT steps. Pure C, no SDK.
//
// Periods are kept in fixed point, c / 2^frac cycles. Accelerating step n
// (0-based) has period c[n], with
//     c[0] = 0.676 * clk * sqrt(2 / accel)
//     c[n] = c[n-1] - 2 * c[n-1] / (4n + 1)
// Acceleration lasts until c[n] reaches the cruise period, or until half the
// move is done (a triangle). Deceleration then replays the same periods
// backwards, by the inverse
//     c[n-1] = c[n] + 2 * c[n] / (4n - 1)
// so the two ramps have the same length.

typedef enum {
    PLANNER_ACCEL,
    PLANNER_CRUISE,
    PLANNER_DECEL,
    PLANNER_DONE,
} planner_phase_t;

typedef struct {
    // The move, fixed by planner_init()
    uint32_t pulses;        // total steps
    uint32_t cruise;        // cruise period, cycles
    uint32_t high;          // STEP high time, cycles

    // Progress
    planner_phase_t phase;
    uint32_t n;             // ramp step index (see above); once cruising,
                            // the length of each ramp
    uint32_t left;          // steps between the ramps not yet planned
    uint32_t c;             // period of ramp step n, fixed point
    uint8_t  frac;          // fractional bits of c
} planner_t;

// Plan a move of pulses (>= 1) steps from rest to rest. cruise is the
// cruise period (below 2^30) and high the STEP high time, in cycles of a
// clk_hz clock; accel is in steps/s^2 (>= 1). The caller checks that cruise
// is long enough for one single word (stepgen.h) plus the F-R2 low time.
void planner_init(planner_t *p, uint32_t pulses, uint32_t cruise, uint32_t high,
                  uint32_t accel, uint32_t clk_hz);

// Write up to max (>= 2) words; returns how many. 0 once the move is fully
// planned.
size_t planner_fill(planner_t *p, uint32_t *words, size_t max);

static inline bool planner_done(const planner_t *p)
{
    return p->phase == PLANNER_DONE;
}

#endif
