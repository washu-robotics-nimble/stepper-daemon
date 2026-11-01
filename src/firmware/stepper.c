#include "stepper.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "board.h"
#include "planner.h"
#include "stepgen.h"

typedef struct {
    uint32_t speed;           // full steps/s
    uint32_t accel;           // full steps/s^2
    uint32_t pulse_width_us;  // STEP high time
    uint32_t microsteps;      // 1, 2, 4, 8 or 16
    bool     dir_forward_high; // dir_polarity: DIR level for positive moves
    bool     has_min, has_max; // soft limits set (F-R25)
    int32_t  limit_min, limit_max;
} motion_t;

// A move's timing, from its motion settings
typedef struct {
    uint32_t cruise;          // cruise period, cycles
    uint32_t high;            // STEP high time, cycles
    uint32_t accel;           // pulses/s^2
} timing_t;

typedef struct {
    motion_t motion;
    int32_t  pos;             // full steps; during a move, where it started
    bool     pos_valid;       // set by set_position; voided by disabling
                              // the driver (set_enable 0, estop)
    bool     enabled;         // driver ENABLE, as last set
    bool     moving;          // until move_end is sent
    bool     start_pending;   // move_start not sent yet
    const char *end_reason;   // set by stop or estop; NULL: ends "done"
    int32_t  target;          // pos once the move ends
    bool     forward;         // direction of the move
    bool     stepping;        // a step generator run belongs to this move
    uint32_t pulses;          // steps of this move's earlier runs (a stop
                              // that finishes the full step is a second run)
    uint32_t last_report;     // time of move_start or the last position, ms
    uint32_t underruns;       // stepgen_underruns() already reported
} axis_t;

static axis_t axes[BOARD_MAX_STEPPERS];

void stepper_init(void)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        const board_motion_t *d = &board_steppers[i].defaults;
        axes[i] = (axis_t){
            .motion = {
                .speed            = d->speed,
                .accel            = d->accel,
                .pulse_width_us   = d->pulse_width_us,
                .microsteps       = d->microsteps,
                .dir_forward_high = d->dir_forward_high,
            },
        };
        board_set_microsteps(i, d->microsteps);
    }
}

// ---- Motion -----

// Timing for motion m, or false if no move could run with it (F-R2). Ramp
// steps are never faster than the cruise, so checking the cruise period
// covers them.
static bool get_timing(const motion_t *m, timing_t *t)
{
    uint64_t clk = stepgen_clock_hz();
    uint64_t rate = (uint64_t)m->speed * m->microsteps;   // pulses/s
    uint64_t accel = (uint64_t)m->accel * m->microsteps;  // pulses/s^2
    if (rate == 0 || accel == 0 || accel > UINT32_MAX)
        return false;

    uint64_t period = (clk + rate / 2) / rate;
    uint64_t high = (uint64_t)m->pulse_width_us * clk / 1000000;
    uint64_t one_us = clk / 1000000;
    if (high < one_us || period < high + one_us)
        return false;
    // What one single word can encode, and the planner's period limit
    if (period < high + STEPGEN_SINGLE_OVERHEAD || period >= (1u << 30))
        return false;

    t->cruise = (uint32_t)period;
    t->high = (uint32_t)high;
    t->accel = (uint32_t)accel;
    return true;
}

// Run the step generator for pulses (>= 1) steps, from rest, in the move's
// direction
static void run(size_t i, uint32_t pulses, const timing_t *t)
{
    axis_t *a = &axes[i];
    planner_t plan;

    planner_init(&plan, pulses, t->cruise, t->high, t->accel, stepgen_clock_hz());
    stepgen_set_high(i, t->high);
    stepgen_set_dir(i, a->forward == a->motion.dir_forward_high);
    stepgen_start(i, &plan);
    a->stepping = true;
}

// pos plus pulses steps in the move's direction, in whole full steps
static int32_t pos_after(const axis_t *a, uint32_t pulses)
{
    int64_t steps = pulses / a->motion.microsteps;
    return (int32_t)(a->forward ? a->pos + steps : a->pos - steps);
}

// Current position: during a move, the steps emitted so far, rounded
// toward the move's start
static int32_t live_pos(size_t i)
{
    const axis_t *a = &axes[i];

    if (!a->moving)
        return a->pos;
    return pos_after(a, a->pulses + (a->stepping ? stepgen_emitted(i) : 0));
}

// Cut the move short (F-R1, F-R21, F-R24). With finish_step, the move then
// ends on the next full step: up to microsteps - 1 more steps, from rest, so
// the position stays a whole number of full steps. move_end follows from
// the poll once the axis is idle.
static void halt(size_t i, const char *reason, bool finish_step)
{
    axis_t *a = &axes[i];

    if (a->stepping) {
        a->pulses += stepgen_stop(i);
        a->stepping = false;
    }

    uint32_t micro = a->motion.microsteps;
    uint32_t rest = finish_step ? (micro - a->pulses % micro) % micro : 0;
    // The settings can't change during a move, so they are still valid
    timing_t t;
    if (rest > 0 && get_timing(&a->motion, &t))
        run(i, rest, &t);
    else
        rest = 0;
    a->target = pos_after(a, a->pulses + rest);
    if (a->end_reason == NULL)
        a->end_reason = reason;
}

// Start a move of steps full steps (F-R5)
static const char *start_move(size_t i, int64_t steps)
{
    axis_t *a = &axes[i];

    if (!hostlink_settings_applied())
        return "not_configured";
    if (a->moving)
        return "busy";

    // Soft limits (F-R25): with any set, they need a valid position
    const motion_t *m = &a->motion;
    if ((m->has_min || m->has_max) && !a->pos_valid)
        return "pos_invalid";

    int64_t target = (int64_t)a->pos + steps;
    if (target < INT32_MIN || target > INT32_MAX)
        return "bad_param";
    if ((m->has_min && target < m->limit_min) || (m->has_max && target > m->limit_max))
        return "bad_param";

    if (steps != 0) {
        uint64_t pulses = (uint64_t)(steps < 0 ? -steps : steps) * a->motion.microsteps;
        timing_t t;
        if (pulses > STEPGEN_MAX_REPEAT || !get_timing(&a->motion, &t))
            return "bad_param";
        a->forward = steps > 0;
        run(i, (uint32_t)pulses, &t);
    } else {
        // Nothing to run: the axis is idle, so the next poll sends
        // move_start and move_end together, as for any finished move
        a->stepping = false;
    }

    a->target = (int32_t)target;
    a->pulses = 0;
    a->end_reason = NULL;
    a->moving = true;
    a->start_pending = true;
    return NULL;
}

void stepper_estop(void)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        axis_t *a = &axes[i];

        // At once, even off a full step; the driver is disabled, so the
        // position is void either way
        if (a->moving)
            halt(i, "estop", false);
        a->pos_valid = false;
        board_set_enable(i, false);
        a->enabled = false;
    }
}

void stepper_link_lost(void)
{
    for (size_t i = 0; i < board_stepper_count; i++)
        if (axes[i].moving && axes[i].end_reason == NULL)
            halt(i, "link_lost", true);
}

// ---- Events -----

static void send_fault(size_t i, const char *code)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "evt", "fault");
    cJSON_AddStringToObject(msg, "oid", board_steppers[i].oid);
    cJSON_AddStringToObject(msg, "code", code);
    hostlink_send(msg);
    cJSON_Delete(msg);
}

// move_start, move_end and position: state reports, so they carry
// settings_applied (F-R33)
static void send_pos_event(size_t i, const char *evt, int32_t pos, const char *reason)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "evt", evt);
    cJSON_AddStringToObject(msg, "oid", board_steppers[i].oid);
    cJSON_AddNumberToObject(msg, "pos", pos);
    if (reason != NULL)
        cJSON_AddStringToObject(msg, "reason", reason);
    cJSON_AddBoolToObject(msg, "settings_applied", hostlink_settings_applied());
    hostlink_send(msg);
    cJSON_Delete(msg);
}

void stepper_poll(uint32_t now_ms)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        axis_t *a = &axes[i];

        // Sent here rather than in the handler, so it follows the reply
        if (a->start_pending) {
            a->start_pending = false;
            a->last_report = now_ms;
            send_pos_event(i, "move_start", a->pos, NULL);
        }

        // A late step (F-R3): the move goes on, with no step lost
        uint32_t underruns = stepgen_underruns(i);
        if (underruns != a->underruns) {
            a->underruns = underruns;
            send_fault(i, "step_underrun");
        }

        if (!a->moving)
            continue;

        // Done, or stopped and idle; also after stop and estop, so it
        // follows the reply
        if (!a->stepping || stepgen_idle(i)) {
            a->pos = a->target;
            a->moving = false;
            a->stepping = false;
            send_pos_event(i, "move_end", a->pos,
                           a->end_reason != NULL ? a->end_reason : "done");
            a->end_reason = NULL;
            continue;
        }

        // Periodic position (F-R32)
        uint32_t period = hostlink_report_period_ms();
        if (period != 0 && now_ms - a->last_report >= period) {
            a->last_report = now_ms;
            send_pos_event(i, "position", live_pos(i), NULL);
        }
    }
}

// ---- Commands -----

// A soft limit field: absent leaves it, null clears it, an int32 sets it.
// False if it is anything else.
static bool get_limit(const cJSON *req, const char *key, bool *has, int32_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(req, key);

    if (item == NULL)
        return true;
    if (cJSON_IsNull(item)) {
        *has = false;
        return true;
    }
    if (!hostlink_get_int32(req, key, value))
        return false;
    *has = true;
    return true;
}

static const char *cmd_set_motion(const cJSON *req, cJSON *reply, size_t index)
{
    (void)reply;
    axis_t *a = &axes[index];
    motion_t m = a->motion;
    int32_t v;

    if (a->moving)
        return "busy";

    // All fields are checked before any is applied
    v = (int32_t)m.microsteps;
    if (!hostlink_get_opt_int32(req, "microsteps", 1, 16, &v) || (v & (v - 1)) != 0)
        return "bad_param";
    m.microsteps = (uint32_t)v;

    v = (int32_t)m.speed;
    if (!hostlink_get_opt_int32(req, "speed", 1, INT32_MAX, &v))
        return "bad_param";
    m.speed = (uint32_t)v;

    v = (int32_t)m.accel;
    if (!hostlink_get_opt_int32(req, "accel", 1, INT32_MAX, &v))
        return "bad_param";
    m.accel = (uint32_t)v;

    v = (int32_t)m.pulse_width_us;
    if (!hostlink_get_opt_int32(req, "pulse_width_us", 1, INT32_MAX, &v))
        return "bad_param";
    m.pulse_width_us = (uint32_t)v;

    const cJSON *pol = cJSON_GetObjectItemCaseSensitive(req, "dir_polarity");
    if (pol != NULL) {
        if (cJSON_IsString(pol) && strcmp(pol->valuestring, "H") == 0)
            m.dir_forward_high = true;
        else if (cJSON_IsString(pol) && strcmp(pol->valuestring, "L") == 0)
            m.dir_forward_high = false;
        else
            return "bad_param";
    }

    if (!get_limit(req, "soft_limit_min", &m.has_min, &m.limit_min)
        || !get_limit(req, "soft_limit_max", &m.has_max, &m.limit_max))
        return "bad_param";
    if (m.has_min && m.has_max && m.limit_min > m.limit_max)
        return "bad_param";

    // The combination too: settings no move could run with are refused now
    timing_t t;
    if (!get_timing(&m, &t))
        return "bad_param";

    if (m.microsteps != a->motion.microsteps)
        board_set_microsteps(index, m.microsteps);   // idle (F-R10)
    a->motion = m;
    return NULL;
}

static const char *cmd_move(const cJSON *req, cJSON *reply, size_t index)
{
    (void)reply;
    int32_t steps;

    if (!hostlink_get_int32(req, "steps", &steps))
        return "bad_param";
    return start_move(index, steps);
}

static const char *cmd_move_to(const cJSON *req, cJSON *reply, size_t index)
{
    (void)reply;
    int32_t pos;

    if (!hostlink_get_int32(req, "pos", &pos))
        return "bad_param";
    return start_move(index, (int64_t)pos - axes[index].pos);
}

static const char *cmd_stop(const cJSON *req, cJSON *reply, size_t index)
{
    (void)req;
    axis_t *a = &axes[index];

    if (!a->moving || a->end_reason != NULL)
        return "not_moving";
    halt(index, "stop", true);
    cJSON_AddNumberToObject(reply, "pos", a->target);
    return NULL;
}

static const char *cmd_set_enable(const cJSON *req, cJSON *reply, size_t index)
{
    (void)reply;
    int32_t value;

    if (!hostlink_get_int32(req, "value", &value) || (value != 0 && value != 1))
        return "bad_param";
    board_set_enable(index, value == 1);
    axes[index].enabled = value == 1;
    // A disabled driver lets the motor turn freely
    if (value == 0)
        axes[index].pos_valid = false;
    return NULL;
}

static const char *cmd_set_position(const cJSON *req, cJSON *reply, size_t index)
{
    (void)reply;
    axis_t *a = &axes[index];
    int32_t pos;

    if (a->moving)
        return "busy";
    if (!hostlink_get_int32(req, "pos", &pos))
        return "bad_param";
    a->pos = pos;
    a->pos_valid = true;
    return NULL;
}

static const char *cmd_get_state(const cJSON *req, cJSON *reply, size_t index)
{
    (void)req;
    const axis_t *a = &axes[index];

    cJSON_AddNumberToObject(reply, "pos", live_pos(index));
    cJSON_AddBoolToObject(reply, "pos_valid", a->pos_valid);
    cJSON_AddBoolToObject(reply, "moving", a->moving);
    cJSON_AddBoolToObject(reply, "enabled", a->enabled);
    cJSON_AddBoolToObject(reply, "settings_applied", hostlink_settings_applied());
    return NULL;
}

const hostlink_cmd_t stepper_cmds[] = {
    {"set_motion",   cmd_set_motion},
    {"move",         cmd_move},
    {"move_to",      cmd_move_to},
    {"stop",         cmd_stop},
    {"set_enable",   cmd_set_enable},
    {"set_position", cmd_set_position},
    {"get_state",    cmd_get_state},
    {NULL, NULL},
};
