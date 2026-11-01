// Trapezoid planner (F-R5): step totals, periods against the exact profile
// t[n] = sqrt(2n / a), acceleration limit, chunking
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "planner.h"
#include "stepgen.h"

#define CLK 125000000u

// The 0.676 correction makes the first period about 32% shorter than the
// exact profile, and the second about 2% shorter; from the third on, every
// period is within this tolerance
#define TOL 0.01
#define FIRST_CHECKED 3

typedef struct {
    // Move under test
    uint32_t pulses, cruise, high, accel;
    // Decoder state
    uint64_t steps;         // steps decoded so far
    uint32_t prev;          // previous period, cycles (0: none yet)
    uint64_t cruise_steps;  // steps in repeat pairs
    uint32_t ramp_steps;    // single words
    double   max_err;       // worst relative period error, steps checked
    double   max_accel;     // worst |acceleration| between steps, steps/s^2
} check_t;

// Exact period of ramp step k (1-based), s
static double exact(const check_t *ck, uint64_t k)
{
    double a = ck->accel;
    return sqrt(2.0 * k / a) - sqrt(2.0 * (k - 1) / a);
}

// Acceleration between two consecutive periods p0, p1 (cycles), in
// steps/s^2. Periods are whole cycles, so each may be up to half a cycle
// off the ideal one; this returns the smallest acceleration consistent with
// that. (At 20 kHz, consecutive periods differ by a fraction of a cycle, and
// a single cycle step would read as 160 times the limit.)
static double min_accel(double p0, double p1)
{
    const double h = 0.5;

    if (fabs(p1 - p0) <= 2 * h)
        return 0;
    if (p1 < p0) {
        p0 -= h;
        p1 += h;
    } else {
        p0 += h;
        p1 -= h;
    }
    return fabs((CLK / p1 - CLK / p0) / ((p0 + p1) / 2 / CLK));
}

// One step of the decoded stream, with its period in cycles
static void step(check_t *ck, uint32_t period_cycles)
{
    double p = (double)period_cycles / CLK;
    uint64_t k = ++ck->steps;
    uint64_t from_end = (uint64_t)ck->pulses + 1 - k;
    uint64_t ramp_k = k < from_end ? k : from_end;

    // Expected: the exact profile from the nearer end, capped at cruise speed
    double expect = exact(ck, ramp_k);
    double cruise = (double)ck->cruise / CLK;
    if (expect < cruise)
        expect = cruise;

    // No step is ever faster than cruise
    assert(period_cycles >= ck->cruise);

    if (ramp_k >= FIRST_CHECKED) {
        double err = fabs(p / expect - 1);
        if (err > ck->max_err)
            ck->max_err = err;
    }
    if (ck->prev > 0) {
        double accel = min_accel(ck->prev, period_cycles);
        if (accel > ck->max_accel)
            ck->max_accel = accel;
    }
    ck->prev = period_cycles;
}

static void decode(check_t *ck, const uint32_t *w, size_t n, uint32_t *pending_repeat)
{
    for (size_t i = 0; i < n; i++) {
        if (*pending_repeat) {
            uint32_t period = w[i] + ck->high + STEPGEN_REPEAT_OVERHEAD;
            assert(period == ck->cruise);
            ck->cruise_steps += *pending_repeat;
            // Only the first and last steps of a run touch prev
            step(ck, period);
            ck->steps += *pending_repeat - 1;
            *pending_repeat = 0;
        } else if (w[i] & 1) {
            *pending_repeat = (w[i] >> 1) + 1;
        } else {
            step(ck, (w[i] >> 1) + ck->high + STEPGEN_SINGLE_OVERHEAD);
            ck->ramp_steps++;
        }
    }
}

// Plan a whole move in chunks of the given size and check it
static check_t run(uint32_t pulses, uint32_t cruise, uint32_t high, uint32_t accel,
                   size_t chunk)
{
    check_t ck = {.pulses = pulses, .cruise = cruise, .high = high, .accel = accel};
    planner_t p;
    uint32_t words[64];
    uint32_t pending = 0;

    assert(chunk >= 2 && chunk <= 64);
    planner_init(&p, pulses, cruise, high, accel, CLK);
    while (!planner_done(&p)) {
        size_t n = planner_fill(&p, words, chunk);
        assert(n <= chunk);
        decode(&ck, words, n, &pending);
    }
    assert(planner_fill(&p, words, chunk) == 0);
    assert(pending == 0);   // no repeat word without its period
    assert(ck.steps == pulses);
    assert(ck.max_err <= TOL);
    assert(ck.max_accel <= accel * (1 + TOL));
    return ck;
}

// Board default: 200 full steps/s, 400 steps/s^2, 20 us pulses
#define CRUISE_200 625000u
#define HIGH_20US  2500u

// Ramp steps of a trapezoid at cruise speed v (steps/s): v^2 / (2a) by the
// exact profile. The 0.676 start runs the recurrence a little behind it in
// speed, which adds steps; a ramp also ends a little early once its period
// rounds to the cruise period. Each ramp is within this of v^2 / (2a).
#define RAMP_TOL 0.02

static void assert_ramps(const check_t *ck, double v)
{
    double ideal = v * v / (2.0 * ck->accel);
    assert(ck->ramp_steps % 2 == 0);
    assert(fabs(ck->ramp_steps / 2 - ideal) <= ideal * RAMP_TOL + 2);
}

static void test_trapezoid(void)
{
    // About v^2 / 2a = 50 steps per ramp
    check_t ck = run(400, CRUISE_200, HIGH_20US, 400, 64);
    assert_ramps(&ck, 200);
    assert(ck.cruise_steps == 400 - ck.ramp_steps);
    printf("  400 steps: %u ramp steps each way\n", ck.ramp_steps / 2);

    // Any chunk size gives the same stream
    for (size_t chunk = 2; chunk <= 64; chunk++) {
        check_t c2 = run(400, CRUISE_200, HIGH_20US, 400, chunk);
        assert(c2.ramp_steps == ck.ramp_steps && c2.max_err == ck.max_err
               && c2.max_accel == ck.max_accel);
    }

    // Shortest trapezoids: two full ramps, 1 and 2 cruise steps
    uint32_t r = ck.ramp_steps;
    ck = run(r + 1, CRUISE_200, HIGH_20US, 400, 64);
    assert(ck.ramp_steps == r && ck.cruise_steps == 1);
    ck = run(r + 2, CRUISE_200, HIGH_20US, 400, 64);
    assert(ck.ramp_steps == r && ck.cruise_steps == 2);
}

static void test_triangle(void)
{
    // Too short to reach cruise speed: even and odd step counts. An odd
    // triangle's middle step is a single word at the peak period.
    check_t ck = run(40, CRUISE_200, HIGH_20US, 400, 64);
    assert(ck.ramp_steps == 40 && ck.cruise_steps == 0);
    ck = run(41, CRUISE_200, HIGH_20US, 400, 64);
    assert(ck.ramp_steps == 41 && ck.cruise_steps == 0);

    // Shortest moves
    ck = run(2, CRUISE_200, HIGH_20US, 400, 64);
    assert(ck.ramp_steps == 2);
    ck = run(1, CRUISE_200, HIGH_20US, 400, 64);
    assert(ck.ramp_steps == 1);
}

static void test_single_step_period(void)
{
    // A one-step move runs at the first ramp period, 0.676 * sqrt(2 / a)
    planner_t p;
    uint32_t w[4];
    planner_init(&p, 1, CRUISE_200, HIGH_20US, 400, CLK);
    assert(planner_fill(&p, w, 4) == 1);
    uint32_t period = (w[0] >> 1) + HIGH_20US + STEPGEN_SINGLE_OVERHEAD;
    double expect = 0.676 * CLK * sqrt(2.0 / 400);
    assert(fabs(period - expect) <= 1);
}

static void test_ramping_off(void)
{
    // A huge accel: the first period is already below cruise, so there are
    // no ramp steps, only one repeat pair
    planner_t p;
    uint32_t w[4];
    planner_init(&p, 400, CRUISE_200, HIGH_20US, 4000000000u, CLK);
    assert(planner_fill(&p, w, 4) == 2);
    assert(w[0] == stepgen_repeat_word(400));
    assert(w[1] == CRUISE_200 - HIGH_20US - STEPGEN_REPEAT_OVERHEAD);
    assert(planner_fill(&p, w, 4) == 0);
    assert(planner_done(&p));

    // Same for a single step
    planner_init(&p, 1, CRUISE_200, HIGH_20US, 4000000000u, CLK);
    assert(planner_fill(&p, w, 4) == 2);
    assert(w[0] == stepgen_repeat_word(1));
}

static void test_long_ramps(void)
{
    // 20000 steps/s at 400 steps/s^2: about 500000 steps per ramp, to show
    // any drift of the integer recurrence
    check_t ck = run(1200000, 6250, 250, 400, 64);
    assert_ramps(&ck, 20000);
    printf("  20 kHz, 400/s^2:  %u ramp steps, worst period error %.4f%%, accel %.4f a\n",
           ck.ramp_steps / 2, 100 * ck.max_err, ck.max_accel / 400);

    // Slowest accel: long first period, few fractional bits
    ck = run(50000, CRUISE_200, HIGH_20US, 1, 64);
    assert_ramps(&ck, 200);
    printf("  200 Hz, 1/s^2:    %u ramp steps, worst period error %.4f%%, accel %.4f a\n",
           ck.ramp_steps / 2, 100 * ck.max_err, ck.max_accel / 1);

    // Fast and steep
    ck = run(100000, 1250, 250, 1000000, 64);
    assert_ramps(&ck, 100000);
    printf("  100 kHz, 1e6/s^2: %u ramp steps, worst period error %.4f%%, accel %.4f a\n",
           ck.ramp_steps / 2, 100 * ck.max_err, ck.max_accel / 1000000);
}

static void test_max_count(void)
{
    // The largest move: ramps plus one repeat pair
    check_t ck = run(STEPGEN_MAX_REPEAT, CRUISE_200, HIGH_20US, 400, 64);
    assert(ck.cruise_steps == STEPGEN_MAX_REPEAT - ck.ramp_steps);

    // Without ramps, a full repeat word
    planner_t p;
    uint32_t w[4];
    planner_init(&p, STEPGEN_MAX_REPEAT, CRUISE_200, HIGH_20US, 4000000000u, CLK);
    assert(planner_fill(&p, w, 4) == 2);
    assert(w[0] == stepgen_repeat_word(STEPGEN_MAX_REPEAT));
}

int main(void)
{
    test_trapezoid();
    test_triangle();
    test_single_step_period();
    test_ramping_off();
    test_long_ramps();
    test_max_count();
    printf("test_planner: ok\n");
    return 0;
}
