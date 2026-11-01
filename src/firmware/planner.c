#include "planner.h"

#include <math.h>
#include "stepgen.h"

// Austin's correction of the first period
#define AUSTIN_C0 0.676

// Fixed-point periods are kept in [C_LIMIT / 2, C_LIMIT): below it, so
// 2 * c fits 32 bits; above half of it, so each step's change 2c / (4n + 1)
// still has enough bits deep into a long ramp. The scale is adjusted as c
// shrinks and grows.
#define C_LIMIT  (1u << 30)
#define FRAC_MAX 24

// Ramp indices from here on would overflow 4n + 1. The recurrence's change
// is far below one cycle by then, so it stops.
#define N_LIMIT (1u << 30)

void planner_init(planner_t *p, uint32_t pulses, uint32_t cruise, uint32_t high,
                  uint32_t accel, uint32_t clk_hz)
{
    *p = (planner_t){
        .pulses = pulses,
        .cruise = cruise,
        .high   = high,
        .phase  = PLANNER_ACCEL,
    };

    // Once per move, so floating point is fine here; none per step
    double c0 = AUSTIN_C0 * clk_hz * sqrt(2.0 / accel);
    while (p->frac < FRAC_MAX && c0 * (double)(2u << p->frac) < C_LIMIT)
        p->frac++;
    p->c = (uint32_t)(c0 * (double)(1u << p->frac) + 0.5);
}

// Keep c in [C_LIMIT / 2, C_LIMIT), see above
static void normalise(planner_t *p)
{
    while (p->c < C_LIMIT / 2 && p->frac < FRAC_MAX) {
        p->c <<= 1;
        p->frac++;
    }
    while (p->c >= C_LIMIT && p->frac > 0) {
        p->c = (p->c + 1) >> 1;
        p->frac--;
    }
}

// 2c / d, rounded to nearest
static uint32_t ratio(uint32_t c, uint32_t d)
{
    uint32_t q = 2 * c / d;
    uint32_t r = 2 * c % d;
    return q + (r >= d - r);
}

// Period of the current ramp step, in cycles
static uint32_t ramp_period(const planner_t *p)
{
    return p->frac ? (p->c + (1u << (p->frac - 1))) >> p->frac : p->c;
}

static uint32_t single_word(const planner_t *p, uint32_t period)
{
    return stepgen_single_word(period - p->high - STEPGEN_SINGLE_OVERHEAD);
}

size_t planner_fill(planner_t *p, uint32_t *words, size_t max)
{
    size_t k = 0;

    while (k < max) {
        switch (p->phase) {
        case PLANNER_ACCEL:
            // Up to cruise speed, or half way (a triangle, F-R5)
            if (p->n == p->pulses / 2 || ramp_period(p) <= p->cruise) {
                p->left = p->pulses - 2 * p->n;
                p->phase = PLANNER_CRUISE;
                break;
            }
            words[k++] = single_word(p, ramp_period(p));
            p->n++;
            if (p->n < N_LIMIT) {
                p->c -= ratio(p->c, 4 * p->n + 1);
                normalise(p);
            }
            break;

        case PLANNER_CRUISE:
            if (p->left == 0) {
                p->phase = PLANNER_DECEL;
                break;
            }
            if (ramp_period(p) > p->cruise) {
                // Odd triangle: its one middle step, at the peak
                words[k++] = single_word(p, ramp_period(p));
                p->left--;
                break;
            }
            if (max - k < 2)
                return k;   // a repeat pair stays in one chunk
            {
                uint32_t count = p->left < STEPGEN_MAX_REPEAT ? p->left : STEPGEN_MAX_REPEAT;
                words[k++] = stepgen_repeat_word(count);
                words[k++] = p->cruise - p->high - STEPGEN_REPEAT_OVERHEAD;
                p->left -= count;
            }
            break;

        case PLANNER_DECEL:
            if (p->n == 0) {
                p->phase = PLANNER_DONE;
                break;
            }
            if (p->n < N_LIMIT) {
                p->c += ratio(p->c, 4 * p->n - 1);
                normalise(p);
            }
            p->n--;
            {
                // Rounding may land a replayed period a cycle below cruise
                uint32_t t = ramp_period(p);
                words[k++] = single_word(p, t > p->cruise ? t : p->cruise);
            }
            break;

        case PLANNER_DONE:
            return k;
        }
    }
    return k;
}
