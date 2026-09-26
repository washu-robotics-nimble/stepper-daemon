#include "status.h"
#include <stdio.h>
#include <string.h>

static const char *cmd_type_name(command_type_t type)
{
    switch (type) {
        case CMD_TYPE_REL:   return "rel";
        case CMD_TYPE_ABS:   return "abs";
        case CMD_TYPE_STOP:  return "stop";
        case CMD_TYPE_SPEED: return "speed";
        case CMD_TYPE_ACCEL: return "accel";
        case CMD_TYPE_MICRO: return "micro";
        case CMD_TYPE_POS:   return "pos";
        case CMD_TYPE_STATUS:return "status";
        case CMD_TYPE_ZERO:  return "zero";
        case CMD_TYPE_HOME:  return "home";
        case CMD_TYPE_LOCK:  return "lock";
        case CMD_TYPE_UNLOCK:return "unlock";
        case CMD_TYPE_HELP:  return "help";
        case CMD_TYPE_QUIT:  return "quit";
        default:             return "?";
    }
}

// Construct command descriptor, e.g. "rel -200"
static void cmd_describe(const command_t *cmd, char *buf, size_t buf_size)
{
    switch (cmd->type) {
        case CMD_TYPE_UNKNOWN:
            snprintf(buf, buf_size, "none");
            break;
        case CMD_TYPE_REL:
            snprintf(buf, buf_size, "rel %d", cmd->args.rel_steps);
            break;
        case CMD_TYPE_ABS:
            snprintf(buf, buf_size, "abs %d", cmd->args.abs_pos);
            break;
        case CMD_TYPE_SPEED:
            snprintf(buf, buf_size, "speed %d", cmd->args.speed);
            break;
        case CMD_TYPE_ACCEL:
            snprintf(buf, buf_size, "accel %d", cmd->args.accel);
            break;
        case CMD_TYPE_MICRO:
            snprintf(buf, buf_size, "micro %d", cmd->args.microsteps);
            break;
        default:
            snprintf(buf, buf_size, "%s", cmd_type_name(cmd->type));
            break;
    }
}

void status_capture(motor_t *motor, status_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));

    command_status_t cs;
    command_get_status(&cs);
    st->current_cmd = cs.current_cmd;
    st->last_result = cs.last_result;
    snprintf(st->last_error, sizeof(st->last_error), "%s", cs.last_error);

    if (motor) {
        int32_t pos = 0;
        st->pos_valid = (motor_get_position(motor, &pos) == MOTOR_OK);
        st->position  = st->pos_valid ? pos : 0;
        st->moving    = motor_is_moving(motor) ? 1 : 0;
        st->locked    = motor_is_locked(motor) ? 1 : 0;
        st->accel     = motor->accel;
    }
}

status_state_t status_state(const status_t *st)
{
    if (st->moving) return STATUS_RUNNING;
    if (st->last_result != CMD_OK) return STATUS_ERROR;
    return STATUS_IDLE;
}

const char *status_state_name(status_state_t state)
{
    switch (state) {
        case STATUS_RUNNING: return "RUNNING";
        case STATUS_ERROR:   return "ERROR";
        default:             return "IDLE";
    }
}

int status_equal(const status_t *a, const status_t *b)
{
    // The argument union holds only 32-bit scalars, so comparing its bytes
    // is equivalent to comparing whichever member the type selects.
    return a->moving      == b->moving
        && a->locked      == b->locked
        && a->pos_valid   == b->pos_valid
        && a->position    == b->position
        && a->accel       == b->accel
        && a->last_result == b->last_result
        && a->current_cmd.type == b->current_cmd.type
        && memcmp(&a->current_cmd.args, &b->current_cmd.args,
                  sizeof(a->current_cmd.args)) == 0
        && strcmp(a->last_error, b->last_error) == 0;
}

int status_format(const status_t *st, char *buf, size_t buf_size)
{
    if (!st || !buf || buf_size == 0) return -1;

    char cmd_desc[64];
    cmd_describe(&st->current_cmd, cmd_desc, sizeof(cmd_desc));

    // Current accel limit, so it's visible whether ramping is active
    char accel_desc[32];
    if (st->accel > 0)
        snprintf(accel_desc, sizeof(accel_desc), "%d", st->accel);
    else
        snprintf(accel_desc, sizeof(accel_desc), "off");

    int n = snprintf(buf, buf_size,
                     "%s | %s | Pos: %s%d | Accel: %s | Cmd: %s | Last: %s",
                     status_state_name(status_state(st)),
                     st->locked ? "Locked" : "Unlocked",
                     st->pos_valid ? "" : "INVALID ",
                     st->pos_valid ? st->position : 0,
                     accel_desc,
                     cmd_desc,
                     st->last_result == CMD_OK ? "OK" : st->last_error);
    return (n < 0) ? -1 : n;
}

int status_get_string(motor_t *motor, char *buf, size_t buf_size)
{
    if (!motor || !buf || buf_size == 0) return -1;

    status_t st;
    status_capture(motor, &st);
    return status_format(&st, buf, buf_size);
}

void status_print(motor_t *motor)
{
    char buf[256];
    if (status_get_string(motor, buf, sizeof(buf)) >= 0) {
        printf("%s\n", buf);
    }
}

void status_watch_init(status_watch_t *w)
{
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->primed = 0;
}

int status_watch_poll(status_watch_t *w, motor_t *motor, status_t *st)
{
    status_t cur;
    status_capture(motor, &cur);

    int changed = 1;
    if (w) {
        changed = !w->primed || !status_equal(&cur, &w->last);
        w->last = cur;
        w->primed = 1;
    }
    if (st) *st = cur;
    return changed;
}
