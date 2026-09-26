#include "ramp.h"
#include <stddef.h>
#include <limits.h>
#include <math.h>

// Pulses needed to accelerate from stop to freq at constant accel.
// Rounded up, so it is only used to decide trapezoid-vs-triangle -- the
// authoritative pulse count comes back out of build_ramp_up().
static int64_t pulses_to_reach_speed(int64_t freq, int64_t accel)
{
    return (int64_t)ceil((double)freq * freq / accel / 2.0);
}

// Builds one constant-frequency segment at `freq` pulses/sec carrying
// `cycles` pulses. Returns RAMP_OK or a RAMP_ERR_* code.
static int make_segment(double freq, int pulse_width_us, int64_t cycles,
                        ramp_segment_t *out)
{
    if (!out || pulse_width_us <= 0) {return RAMP_ERR_PARAM;}
    // lgTxPulse takes cycles as an int, so anything past INT_MAX cannot be
    // expressed as a single segment.
    if (cycles < 1 || cycles > (int64_t)INT_MAX) {return RAMP_ERR_PARAM;}

    if (freq < 1.0) {freq = 1.0;}

    int64_t period_us = (int64_t)round(1e6 / freq);
    if (period_us <= RAMP_MIN_TX_PERIOD_US) {return RAMP_ERR_TOO_FAST;}

    int64_t off_us = period_us - pulse_width_us;
    if (off_us < 0) {return RAMP_ERR_TOO_FAST;}

    out->pulse_on_us  = pulse_width_us;
    out->pulse_off_us = (int)off_us;
    out->cycles       = (int)cycles;
    return RAMP_OK;
}

// Fills out[0..returned-1] with a ramp-up from ~0 to freq_target, spending
// at most ramp_pulses pulses and at most n segments.
//
// Returns the number of segments written, or a RAMP_ERR_* code. On success
// *out_pulses gets the exact number of pulses the ramp covers, and
// *out_v_final the frequency it ends at (both needed by the caller to size
// the cruise segment). Either out-param may be NULL.
static int build_ramp_up(int64_t freq_target, int64_t accel,
                          int64_t ramp_pulses, int n,
                          int pulse_width_us, ramp_segment_t *out,
                          int64_t *out_pulses, double *out_v_final)
{
    if (!out || n < 1) {return RAMP_ERR_PARAM;}
    if (freq_target <= 0 || accel <= 0 || ramp_pulses <= 0){
        return RAMP_ERR_PARAM;
    }

    double v_peak = sqrt(2.0 * accel * ramp_pulses);
    double v_target = (double)freq_target;
    if (v_peak < v_target) {v_target = v_peak;}

    // Eval segment count. Every segment must carry at least one pulse;
    // the narrowest is the first, spanning dv^2/(2*accel) pulses, so the
    // count is capped at v_target / sqrt(2*accel).
    int seg_cnt = n;
    double max_seg_d = floor(v_target / sqrt(2.0 * accel));
    int max_seg;
    if (max_seg_d < 1.0) {max_seg = 1;}
    else if (max_seg_d > (double)RAMP_MAX_SEGMENTS) {max_seg = RAMP_MAX_SEGMENTS;}
    else {max_seg = (int)max_seg_d;}
    if (seg_cnt > max_seg) {seg_cnt = max_seg;}

    double dv = v_target / seg_cnt;
    int64_t total = 0;

    for (int i = 0; i < seg_cnt; i++) {
        double v0 = i * dv;
        double v1 = (i + 1) * dv;

        // Step count. Closed form, so the per-segment counts telescope to
        // the ramp total exactly instead of accumulating rounding drift.
        int64_t s0 = (int64_t)round(v0 * v0 / (2.0 * accel));
        int64_t s1 = (int64_t)round(v1 * v1 / (2.0 * accel));
        int64_t cycles = s1 - s0;
        if (cycles < 1) {cycles = 1;}

        // Mean pulse rate over a constant-accel segment is exactly (v0+v1)/2
        int err = make_segment((v0 + v1) / 2.0, pulse_width_us, cycles, &out[i]);
        if (err != RAMP_OK) {return err;}

        total += cycles;
    }

    if (out_pulses)  {*out_pulses = total;}
    if (out_v_final) {*out_v_final = v_target;}
    return seg_cnt;
}

// Mirrors a ramp-up segment array into a ramp-down (descending frequency).
// Returns the number of segments written, or a RAMP_ERR_* code.
static int mirror_ramp_down(const ramp_segment_t *ramp_up, int n,
                             ramp_segment_t *out)
{
    if (!ramp_up || !out || n < 1) {return RAMP_ERR_PARAM;}
    for (int i = 0; i < n; i++) {
        out[i] = ramp_up[n-1-i];
    }
    return n;
}

// Final safety pass over an assembled plan.
static int validate_plan(const ramp_plan_t *plan, int min_pulse_off_us)
{
    if (plan->num_segments < 1 || plan->num_segments > RAMP_MAX_SEGMENTS) {
        return RAMP_ERR_PARAM;
    }
    for (int i = 0; i < plan->num_segments; i++) {
        const ramp_segment_t *s = &plan->segments[i];
        if (s->cycles < 1 || s->pulse_on_us < 1) {return RAMP_ERR_PARAM;}
        if (s->pulse_off_us < min_pulse_off_us) {return RAMP_ERR_TOO_FAST;}
        // Driver floor: lgpio rejects (on + off) <= lgMinTxDelay
        if (s->pulse_on_us + s->pulse_off_us <= RAMP_MIN_TX_PERIOD_US) {
            return RAMP_ERR_TOO_FAST;
        }
    }
    return RAMP_OK;
}

// -----------------------------------------------------------------------

int ramp_build_plan(int speed_full_steps_per_sec,
                     int accel_full_steps_per_sec2,
                     int microsteps,
                     int pulse_width_us,
                     int32_t abs_steps,
                     int min_pulse_off_us,
                     ramp_plan_t *out_plan)
{
    if (!out_plan) return RAMP_ERR_PARAM;
    if (speed_full_steps_per_sec <= 0) return RAMP_ERR_PARAM;
    if (accel_full_steps_per_sec2 <= 0) return RAMP_ERR_PARAM;
    if (microsteps <= 0) return RAMP_ERR_PARAM;
    if (pulse_width_us <= 0) return RAMP_ERR_PARAM;
    if (abs_steps <= 0) return RAMP_ERR_PARAM;
    if (min_pulse_off_us < 0) return RAMP_ERR_PARAM;

    // Work in the pulse domain throughout
    int64_t freq         = (int64_t)speed_full_steps_per_sec * microsteps;
    int64_t accel        = (int64_t)accel_full_steps_per_sec2 * microsteps;
    int64_t total_pulses = (int64_t)abs_steps * microsteps;

    // Assembled locally so out_plan is left untouched if anything fails
    ramp_plan_t plan;
    int err;

    // Pulses one ramp needs to reach full speed
    int64_t d_accel = pulses_to_reach_speed(freq, accel);

    int64_t ramp_pulses;
    if (2 * d_accel <= total_pulses) {
        // Trapezoid: full speed is reachable
        ramp_pulses = d_accel;
    } else {
        // Triangle: too short to reach full speed, so the ramp turns
        // around at the midpoint. floor() here leaves 0 or 1 pulse over,
        // which the remainder segment below picks up.
        ramp_pulses = total_pulses / 2;
    }

    if (ramp_pulses < 1) {
        // Move is too short to ramp at all -- emit one constant-frequency
        // segment, capped at the speed the accel limit allows over the
        // whole distance.
        double v = (double)freq;
        double v_lim = sqrt(2.0 * accel * total_pulses);
        if (v_lim < v) {v = v_lim;}

        err = make_segment(v, pulse_width_us, total_pulses, &plan.segments[0]);
        if (err != RAMP_OK) return err;
        plan.num_segments = 1;

        err = validate_plan(&plan, min_pulse_off_us);
        if (err != RAMP_OK) return err;
        *out_plan = plan;
        return RAMP_OK;
    }

    // Segment budget per ramp. Always reserve one slot for cruise/remainder
    // so the triangular case cannot overflow the plan on a leftover pulse.
    int budget = (RAMP_MAX_SEGMENTS - 1) / 2;
    if (budget < 1) {budget = 1;}

    ramp_segment_t up[RAMP_MAX_SEGMENTS];
    int64_t up_pulses = 0;
    double  v_cruise  = (double)freq;

    int up_cnt = build_ramp_up(freq, accel, ramp_pulses, budget,
                               pulse_width_us, up, &up_pulses, &v_cruise);
    if (up_cnt < 0) return up_cnt;

    // Whatever the two ramps don't cover is cruised at the peak frequency.
    // This is what makes the total come out exactly equal to total_pulses.
    int64_t cruise_pulses = total_pulses - 2 * up_pulses;
    if (cruise_pulses < 0) return RAMP_ERR_PARAM;  // ramps overshot; shouldn't happen

    int need_cruise = (cruise_pulses > 0) ? 1 : 0;
    if (2 * up_cnt + need_cruise > RAMP_MAX_SEGMENTS) return RAMP_ERR_PARAM;

    int k = 0;
    for (int i = 0; i < up_cnt; i++) {
        plan.segments[k++] = up[i];
    }

    if (need_cruise) {
        err = make_segment(v_cruise, pulse_width_us, cruise_pulses,
                           &plan.segments[k]);
        if (err != RAMP_OK) return err;
        k++;
    }

    int down_cnt = mirror_ramp_down(up, up_cnt, &plan.segments[k]);
    if (down_cnt < 0) return down_cnt;
    k += down_cnt;

    plan.num_segments = k;

    err = validate_plan(&plan, min_pulse_off_us);
    if (err != RAMP_OK) return err;

    *out_plan = plan;
    return RAMP_OK;
}
