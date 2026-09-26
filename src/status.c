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

int status_get_string(motor_t *motor, char *buf, size_t buf_size)
{
    if (!motor || !buf || buf_size == 0) return -1;

    command_status_t cs;
    command_get_status(&cs);

    int32_t pos = 0;
    int pos_valid = (motor_get_position(motor, &pos) == MOTOR_OK);
    int moving = motor_is_moving(motor);

    const char *state_str = "IDLE";
    if (moving) {
        state_str = "RUNNING";
    } else if (cs.last_result != CMD_OK) {
        state_str = "ERROR";
    }

    // 构建命令描述
    char cmd_desc[64] = "none";
    if (cs.current_cmd.type != CMD_TYPE_UNKNOWN) {
        switch (cs.current_cmd.type) {
            case CMD_TYPE_REL:
                snprintf(cmd_desc, sizeof(cmd_desc), "rel %d", cs.current_cmd.args.rel_steps);
                break;
            case CMD_TYPE_ABS:
                snprintf(cmd_desc, sizeof(cmd_desc), "abs %d", cs.current_cmd.args.abs_pos);
                break;
            case CMD_TYPE_SPEED:
                snprintf(cmd_desc, sizeof(cmd_desc), "speed %d", cs.current_cmd.args.speed);
                break;
            case CMD_TYPE_ACCEL:
                snprintf(cmd_desc, sizeof(cmd_desc), "accel %d", cs.current_cmd.args.accel);
                break;
            case CMD_TYPE_MICRO:
                snprintf(cmd_desc, sizeof(cmd_desc), "micro %d", cs.current_cmd.args.microsteps);
                break;
            default:
                snprintf(cmd_desc, sizeof(cmd_desc), "%s", cmd_type_name(cs.current_cmd.type));
                break;
        }
    }

    // Current accel limit, so it's visible whether ramping is active
    char accel_desc[32];
    if (motor->accel > 0)
        snprintf(accel_desc, sizeof(accel_desc), "%d", motor->accel);
    else
        snprintf(accel_desc, sizeof(accel_desc), "off");

    int n = snprintf(buf, buf_size,
                     "State: %s | Pos: %s%d | Accel: %s | Cmd: %s | Last: %s%s",
                     state_str,
                     pos_valid ? "" : "INVALID ",
                     pos_valid ? pos : 0,
                     accel_desc,
                     cmd_desc,
                     cs.last_result == CMD_OK ? "OK" : cs.last_error,
                     cs.last_result == CMD_OK ? "" : "");
    return (n < 0) ? -1 : n;
}

void status_print(motor_t *motor)
{
    char buf[256];
    if (status_get_string(motor, buf, sizeof(buf)) >= 0) {
        printf("%s\n", buf);
    }
}