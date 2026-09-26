#ifndef RAMP_H
#define RAMP_H

#include <stdint.h>

// A single constant-frequency segment of a move. Maps directly onto one
// lgTxPulse(handle, gpio, pulse_on_us, pulse_off_us, 0, cycles) call.
typedef struct {
    int pulse_on_us;
    int pulse_off_us;
    int cycles;
} ramp_segment_t;

// Max number of segments a plan can hold.
//
// NOTE: lgTxPulse's software queue (LG_TX_BUF in lib/lg/lgPthTx.h) only
// holds 10 entries per GPIO at once. Keeping this <= 10 means motor.c can
// fire a whole plan off non-blocking without polling lgTxRoom() to top the
// queue up. Raising it trades that simplicity for a smoother ramp curve.
#define RAMP_MAX_SEGMENTS 10

// lgpio refuses a pulse unless (pulse_on_us + pulse_off_us) is strictly
// greater than this -- see lgMinTxDelay in lib/lg/lgPthTx.c, enforced at
// lib/lg/lgGpio.c. This is a hard driver constraint, independent of the
// caller's own min_pulse_off_us policy.
#define RAMP_MIN_TX_PERIOD_US 10

typedef struct {
    ramp_segment_t segments[RAMP_MAX_SEGMENTS];
    int num_segments;
} ramp_plan_t;

// Error codes
#define RAMP_OK             0
#define RAMP_ERR_PARAM     -1  // bad input (speed/accel/microsteps <= 0, etc.)
#define RAMP_ERR_TOO_FAST  -2  // a resulting segment violated a timing floor

// Build a ramp-up / cruise / ramp-down plan for a move of `abs_steps` full
// steps, honouring a maximum acceleration.
//
// The profile is trapezoidal when the move is long enough to reach the
// target speed, and degrades automatically to a triangular profile (ramp
// turns around at the midpoint, never reaching target speed) when it is
// not. Very short moves fall back to a single constant-frequency segment
// at whatever speed the acceleration limit allows over the distance.
//
//  speed_full_steps_per_sec:  target cruise speed (motor->speed)
// accel_full_steps_per_sec2:  max acceleration limit (motor->accel).
//                             Caller should treat <= 0 as "ramping
//                             disabled" and skip calling this function
//                             entirely (keep today's single constant-
//                             frequency lgTxPulse call in that case).
//                microsteps:  current microstep resolution (motor->microsteps)
//            pulse_width_us:  STEP pulse HIGH time (motor->pulse_width_us)
//                 abs_steps:  move distance in full steps (> 0)
//          min_pulse_off_us:  caller's minimum allowed LOW time between
//                             pulses (matches the existing `pulse_off < 10`
//                             check in motor.c). Applied on top of the
//                             driver's own RAMP_MIN_TX_PERIOD_US floor.
//                  out_plan:  filled in on success; left untouched on error
//
// Returns RAMP_OK, or one of the RAMP_ERR_* codes above.
//
// GUARANTEE: on success, the sum of `cycles` over all returned segments is
// exactly abs_steps * microsteps. motor.c's no-feedback position tracking
// assumes the commanded step count is delivered in full, so this must hold
// or the recorded position silently drifts from the physical one.
int ramp_build_plan(int speed_full_steps_per_sec,
                     int accel_full_steps_per_sec2,
                     int microsteps,
                     int pulse_width_us,
                     int32_t abs_steps,
                     int min_pulse_off_us,
                     ramp_plan_t *out_plan);

#endif
