#include "stepper.h"

#include <stdbool.h>
#include <stdint.h>
#include "board.h"
#include "stepgen.h"

typedef struct {
    uint32_t speed;           // full steps/s
    uint32_t pulse_width_us;  // STEP high time
    uint32_t microsteps;      // MS pins are held at full step until step 6
    bool     dir_forward_high; // DIR level for positive moves
} motion_t;

typedef struct {
    motion_t motion;
    int32_t  pos;             // full steps
    bool     moving;
    bool     start_pending;   // move_start not sent yet
    int32_t  target;          // pos once the move is done
} axis_t;

static axis_t axes[BOARD_MAX_STEPPERS];

void stepper_init(void)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        const board_motion_t *d = &board_steppers[i].defaults;
        axes[i] = (axis_t){
            .motion = {
                .speed            = d->speed,
                .pulse_width_us   = d->pulse_width_us,
                .microsteps       = 1,
                .dir_forward_high = true,
            },
        };
    }
}

// ---- Events -----

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

        if (a->moving && stepgen_idle(i)) {
            a->moving = false;
            a->pos = a->target;
            send_move_event(i, "move_end", "done");
        }
    }
}

// ---- Commands -----

// Start a constant-speed move of steps full steps (build plan step 3; ramps
// come in step 4)
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
        a->start_pending = true;
        return NULL;
    }

    uint64_t pulses = (uint64_t)(steps < 0 ? -(int64_t)steps : steps) * m->microsteps;
    if (pulses > STEPGEN_MAX_REPEAT)
        return "bad_param";

    // Timing in state machine cycles. F-R2: high time >= pulse_width_us,
    // low time >= 1 us.
    uint64_t clk = stepgen_clock_hz();
    uint64_t rate = (uint64_t)m->speed * m->microsteps;   // pulses/s
    if (rate == 0)
        return "bad_param";
    uint64_t period = (clk + rate / 2) / rate;
    uint64_t high = m->pulse_width_us * clk / 1000000;
    uint64_t low_min = clk / 1000000;
    if (high < low_min || period < high + low_min || period > UINT32_MAX)
        return "bad_param";

    stepgen_set_high(i, (uint32_t)high);
    stepgen_set_dir(i, (steps > 0) == m->dir_forward_high);

    // The FIFO is empty while idle, so both words of the pair fit
    stepgen_push(i, stepgen_repeat_word((uint32_t)pulses));
    stepgen_push(i, (uint32_t)(period - high - STEPGEN_REPEAT_OVERHEAD));

    a->target = (int32_t)target;
    a->moving = true;
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

const hostlink_cmd_t stepper_cmds[] = {
    {"move", cmd_move},
    {NULL, NULL},
};
