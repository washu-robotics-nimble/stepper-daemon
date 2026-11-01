#include "stepper.h"

#include <stdbool.h>
#include <stdint.h>
#include "board.h"
#include "planner.h"
#include "stepgen.h"

typedef struct {
    uint32_t speed;           // full steps/s
    uint32_t accel;           // full steps/s^2
    uint32_t pulse_width_us;  // STEP high time
    uint32_t microsteps;      // MS pins are held at full step until step 6
    bool     dir_forward_high; // DIR level for positive moves
} motion_t;

typedef struct {
    motion_t motion;
    int32_t  pos;             // full steps; during a move, where it started
    bool     moving;          // until move_end is sent
    bool     start_pending;   // move_start not sent yet
    const char *end_reason;   // move_end pending, with this reason
    int32_t  target;          // pos once the move is done
    bool     stepping;        // the step generator runs this move (not a
                              // move of 0 steps)
    bool     forward;         // direction of the move
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
                .microsteps       = 1,
                .dir_forward_high = true,
            },
        };
    }
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

static void send_move_event(size_t i, const char *evt, const char *reason)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "evt", evt);
    cJSON_AddStringToObject(msg, "oid", board_steppers[i].oid);
    cJSON_AddNumberToObject(msg, "pos", axes[i].pos);
    if (reason != NULL)
        cJSON_AddStringToObject(msg, "reason", reason);
    hostlink_send(msg);
    cJSON_Delete(msg);
}

void stepper_poll(void)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        axis_t *a = &axes[i];

        // Sent here rather than in the handler, so it follows the reply
        if (a->start_pending) {
            a->start_pending = false;
            send_move_event(i, "move_start", NULL);
        }

        // A late step (F-R3): the move goes on, with no step lost
        uint32_t underruns = stepgen_underruns(i);
        if (underruns != a->underruns) {
            a->underruns = underruns;
            send_fault(i, "step_underrun");
        }

        if (a->moving && a->end_reason == NULL && stepgen_idle(i)) {
            a->pos = a->target;
            a->end_reason = "done";
        }

        // Also sent from here after stop and estop, so it follows the reply
        if (a->end_reason != NULL) {
            send_move_event(i, "move_end", a->end_reason);
            a->end_reason = NULL;
            a->moving = false;
        }
    }
}

// Cut the move short now (F-R21, F-R24): the position becomes the steps
// actually emitted (F-R1), and move_end follows with reason
static void halt(size_t i, const char *reason)
{
    axis_t *a = &axes[i];

    if (a->stepping) {
        uint32_t pulses = stepgen_stop(i);
        int64_t steps = pulses / a->motion.microsteps;
        a->pos = (int32_t)(a->forward ? a->pos + steps : a->pos - steps);
        a->stepping = false;
    }
    a->end_reason = reason;
}

void stepper_estop(void)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        if (axes[i].moving && axes[i].end_reason == NULL)
            halt(i, "estop");
        board_set_enable(i, false);
    }
}

// ---- Commands -----

// Start a trapezoid move of steps full steps (F-R5)
static const char *start_move(size_t i, int32_t steps)
{
    axis_t *a = &axes[i];
    const motion_t *m = &a->motion;

    if (a->moving)
        return "busy";

    int64_t target = (int64_t)a->pos + steps;
    if (target < INT32_MIN || target > INT32_MAX)
        return "bad_param";
    if (steps == 0) {
        // Nothing to queue: the axis is idle, so the next poll sends
        // move_start and move_end together, as for any finished move
        a->target = a->pos;
        a->moving = true;
        a->stepping = false;
        a->start_pending = true;
        return NULL;
    }

    uint64_t pulses = (uint64_t)(steps < 0 ? -(int64_t)steps : steps) * m->microsteps;
    if (pulses > STEPGEN_MAX_REPEAT)
        return "bad_param";

    // Timing in state machine cycles. F-R2: high time >= pulse_width_us,
    // low time >= 1 us. Ramp steps are never faster than the cruise, so
    // checking the cruise period covers them.
    uint64_t clk = stepgen_clock_hz();
    uint64_t rate = (uint64_t)m->speed * m->microsteps;   // pulses/s
    uint64_t accel = (uint64_t)m->accel * m->microsteps;  // pulses/s^2
    if (rate == 0 || accel == 0 || accel > UINT32_MAX)
        return "bad_param";
    uint64_t period = (clk + rate / 2) / rate;
    uint64_t high = m->pulse_width_us * clk / 1000000;
    uint64_t one_us = clk / 1000000;
    if (high < one_us || period < high + one_us)
        return "bad_param";
    // What one single word can encode, and the planner's period limit
    if (period < high + STEPGEN_SINGLE_OVERHEAD || period >= (1u << 30))
        return "bad_param";

    planner_t plan;
    planner_init(&plan, (uint32_t)pulses, (uint32_t)period, (uint32_t)high,
                 (uint32_t)accel, (uint32_t)clk);

    stepgen_set_high(i, (uint32_t)high);
    stepgen_set_dir(i, (steps > 0) == m->dir_forward_high);
    stepgen_start(i, &plan);

    a->target = (int32_t)target;
    a->moving = true;
    a->stepping = true;
    a->forward = steps > 0;
    a->start_pending = true;
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

static const char *cmd_stop(const cJSON *req, cJSON *reply, size_t index)
{
    (void)req;
    axis_t *a = &axes[index];

    if (!a->moving || a->end_reason != NULL)
        return "not_moving";
    halt(index, "stop");
    cJSON_AddNumberToObject(reply, "pos", a->pos);
    return NULL;
}

static const char *cmd_set_enable(const cJSON *req, cJSON *reply, size_t index)
{
    (void)reply;
    int32_t value;

    if (!hostlink_get_int32(req, "value", &value) || (value != 0 && value != 1))
        return "bad_param";
    board_set_enable(index, value == 1);
    return NULL;
}

const hostlink_cmd_t stepper_cmds[] = {
    {"move",       cmd_move},
    {"stop",       cmd_stop},
    {"set_enable", cmd_set_enable},
    {NULL, NULL},
};
