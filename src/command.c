#include "command.h"
#include "status.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <stdlib.h>
#include <ctype.h>

static pthread_mutex_t g_status_mutex = PTHREAD_MUTEX_INITIALIZER;
static command_status_t g_status;

int command_init(void)
{
    pthread_mutex_lock(&g_status_mutex);
    memset(&g_status, 0, sizeof(g_status));
    g_status.last_result = CMD_OK;
    pthread_mutex_unlock(&g_status_mutex);
    return 0;
}

command_result_t command_parse(const char *line, command_t *cmd,
                               char *errbuf, size_t errbuf_size)
{
    if (!line || !cmd) return CMD_ERR_SYNTAX;

    // 跳过前导空白
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '\n') return CMD_ERR_SYNTAX;

    char name[32];
    if (sscanf(line, "%31s", name) != 1) {
        if (errbuf) snprintf(errbuf, errbuf_size, "Empty command");
        return CMD_ERR_SYNTAX;
    }

    // 转为小写以便不区分大小写
    for (char *p = name; *p; p++) *p = tolower(*p);

    cmd->type = CMD_TYPE_UNKNOWN;

    if (strcmp(name, "rel") == 0) {
        int32_t steps;
        if (sscanf(line, "%*s %d", &steps) != 1) {
            if (errbuf) snprintf(errbuf, errbuf_size, "Usage: rel <steps>");
            return CMD_ERR_PARAM;
        }
        cmd->type = CMD_TYPE_REL;
        cmd->args.rel_steps = steps;
    } else if (strcmp(name, "abs") == 0) {
        int32_t pos;
        if (sscanf(line, "%*s %d", &pos) != 1) {
            if (errbuf) snprintf(errbuf, errbuf_size, "Usage: abs <pos>");
            return CMD_ERR_PARAM;
        }
        cmd->type = CMD_TYPE_ABS;
        cmd->args.abs_pos = pos;
    } else if (strcmp(name, "stop") == 0) {
        cmd->type = CMD_TYPE_STOP;
    } else if (strcmp(name, "speed") == 0) {
        int speed;
        if (sscanf(line, "%*s %d", &speed) != 1 || speed <= 0) {
            if (errbuf) snprintf(errbuf, errbuf_size, "Usage: speed <positive>");
            return CMD_ERR_PARAM;
        }
        cmd->type = CMD_TYPE_SPEED;
        cmd->args.speed = speed;
    } else if (strcmp(name, "micro") == 0) {
        int ms;
        if (sscanf(line, "%*s %d", &ms) != 1) {
            if (errbuf) snprintf(errbuf, errbuf_size, "Usage: micro <1|2|4|8|16>");
            return CMD_ERR_PARAM;
        }
        if (ms != 1 && ms != 2 && ms != 4 && ms != 8 && ms != 16) {
            if (errbuf) snprintf(errbuf, errbuf_size, "Microstep must be 1,2,4,8,16");
            return CMD_ERR_PARAM;
        }
        cmd->type = CMD_TYPE_MICRO;
        cmd->args.microsteps = ms;
    } else if (strcmp(name, "pos") == 0) {
        cmd->type = CMD_TYPE_POS;
    } else if (strcmp(name, "status") == 0) {
        cmd->type = CMD_TYPE_STATUS;
    } else if (strcmp(name, "zero") == 0) {
        cmd->type = CMD_TYPE_ZERO;
    } else if (strcmp(name, "home") == 0) {
        cmd->type = CMD_TYPE_HOME;
    } else if (strcmp(name, "lock") == 0) {
        cmd->type = CMD_TYPE_LOCK;
    } else if (strcmp(name, "unlock") == 0) {
        cmd->type = CMD_TYPE_UNLOCK;
    } else if (strcmp(name, "help") == 0 || strcmp(name, "?") == 0) {
        cmd->type = CMD_TYPE_HELP;
    } else if (strcmp(name, "quit") == 0 || strcmp(name, "exit") == 0) {
        cmd->type = CMD_TYPE_QUIT;
    } else {
        if (errbuf) snprintf(errbuf, errbuf_size, "Unknown command: %s", name);
        return CMD_ERR_UNKNOWN;
    }

    return CMD_OK;
}

// Translates motor error to command error
static command_result_t motor_to_cmd_result(int motor_ret)
{
    if (motor_ret == MOTOR_OK) return CMD_OK;
    return CMD_ERR_MOTOR;
}

command_result_t command_run(motor_t *motor, const command_t *cmd,
                             char *response, size_t resp_size)
{
    if (!motor || !cmd) return CMD_ERR_PARAM;

    // Record current command
    pthread_mutex_lock(&g_status_mutex);
    g_status.current_cmd = *cmd;
    g_status.last_result = CMD_OK;
    g_status.last_error[0] = '\0';
    pthread_mutex_unlock(&g_status_mutex);

    command_result_t ret = CMD_OK;
    int motor_ret = MOTOR_OK;

    switch (cmd->type) {
        case CMD_TYPE_REL:
            motor_ret = motor_move_rel(motor, cmd->args.rel_steps);
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Moving rel %d", cmd->args.rel_steps);
            break;

        case CMD_TYPE_ABS:
            motor_ret = motor_move_abs(motor, cmd->args.abs_pos);
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Moving abs %d", cmd->args.abs_pos);
            break;

        case CMD_TYPE_STOP:
            motor_ret = motor_stop(motor);
            // If not moving, STOP is always successful
            if (motor_ret == MOTOR_ERR_NOT_MOVING) motor_ret = MOTOR_OK;
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Stop requested");
            break;

        case CMD_TYPE_SPEED:
            motor_ret = motor_set_speed(motor, cmd->args.speed);
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Speed set to %d", cmd->args.speed);
            break;

        case CMD_TYPE_MICRO:
            motor_ret = motor_set_microstep(motor, cmd->args.microsteps);
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Microstep set to %d", cmd->args.microsteps);
            break;

        case CMD_TYPE_POS: {
            int32_t pos;
            if (motor_get_position(motor, &pos) == MOTOR_OK) {
                if (response) snprintf(response, resp_size, "Position: %d", pos);
            } else {
                if (response) snprintf(response, resp_size, "Position invalid");
            }
            break;
        }

        case CMD_TYPE_STATUS:
            if (response) status_get_string(motor, response, resp_size);
            break;

        case CMD_TYPE_ZERO:
            motor_ret = motor_zero(motor);
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Position zeroed");
            break;

        case CMD_TYPE_HOME:
            motor_ret = motor_home(motor);
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Home not implemented");
            break;

        case CMD_TYPE_LOCK:
            motor_ret = motor_lock(motor);
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Motor locked (disabled)");
            break;

        case CMD_TYPE_UNLOCK:
            motor_ret = motor_unlock(motor);
            ret = motor_to_cmd_result(motor_ret);
            if (response) snprintf(response, resp_size, "Motor unlocked (enabled)");
            break;

        case CMD_TYPE_HELP:
            if (response) snprintf(response, resp_size, "%s", command_help());
            break;

        case CMD_TYPE_QUIT:
            // Hand over to CLI/consumer
            if (response) snprintf(response, resp_size, "Bye");
            break;

        default:
            ret = CMD_ERR_UNKNOWN;
            if (response) snprintf(response, resp_size, "Unknown command");
            break;
    }

    // Update last result
    pthread_mutex_lock(&g_status_mutex);
    g_status.last_result = ret;
    if (ret != CMD_OK) {
        snprintf(g_status.last_error, sizeof(g_status.last_error),
                 "%s", lguErrorText(motor_ret));
    }
    pthread_mutex_unlock(&g_status_mutex);

    return ret;
}

command_result_t command_run_blocking(motor_t *motor, const command_t *cmd,
                                      char *response, size_t resp_size)
{
    command_result_t ret = command_run(motor, cmd, response, resp_size);
    if (ret == CMD_OK &&
        (cmd->type == CMD_TYPE_REL || cmd->type == CMD_TYPE_ABS || cmd->type == CMD_TYPE_HOME)) {
        motor_wait(motor);
    }
    return ret;
}

void command_get_status(command_status_t *status)
{
    if (!status) return;
    pthread_mutex_lock(&g_status_mutex);
    *status = g_status;
    pthread_mutex_unlock(&g_status_mutex);
}

const char *command_help(void)
{
    return "Commands:\n"
           "  rel <steps>    - move relative steps\n"
           "  abs <pos>      - move absolute position\n"
           "  stop           - emergency stop\n"
           "  speed <val>    - set speed (full steps/sec)\n"
           "  micro <1|2|4|8|16> - set microstep\n"
           "  pos            - show current position\n"
           "  status         - show full status\n"
           "  zero           - set current position as zero\n"
           "  home           - home motor (not implemented)\n"
           "  lock           - disable outputs\n"
           "  unlock         - enable outputs\n"
           "  help           - this help\n"
           "  quit/exit      - exit program\n";
}