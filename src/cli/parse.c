#include "parse.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The client protocol's commands (docs/daemon/spec.md); nimhubd decides
// what is valid, so a name missing here still goes out
const char *const cli_daemon_commands[] = {
    "devices", "estop", "lease", "renew", "release", "watch", "unwatch", NULL,
};

const char *const cli_motor_commands[] = {
    "rel", "abs", "stop", "pos", "status", "speed", "accel", "micro",
    "zero", "home", "lock", "unlock", NULL,
};

// The write commands (D-R13, docs/daemon/spec.md), which need the lease
static const char *const write_commands[] = {
    "rel", "abs", "stop", "speed", "accel", "micro", "zero", "home", "lock", "unlock", NULL,
};

bool cli_is_write(const cJSON *req)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    if (!cJSON_IsString(cmd) || cJSON_GetObjectItemCaseSensitive(req, "dev") == NULL)
        return false;
    for (const char *const *w = write_commands; *w != NULL; w++)
        if (strcmp(*w, cmd->valuestring) == 0)
            return true;
    return false;
}

// The argument field of each device command that takes one; any other
// command's argument goes into "value"
static const struct {
    const char *cmd;
    const char *field;
} arg_fields[] = {
    {"rel", "steps"},
    {"abs", "pos"},
};

#define MAX_WORDS 4

static const char *arg_field(const char *cmd)
{
    for (size_t i = 0; i < sizeof arg_fields / sizeof arg_fields[0]; i++)
        if (strcmp(arg_fields[i].cmd, cmd) == 0)
            return arg_fields[i].field;
    return "value";
}

// An argument: a number if it reads as one, else a string; nimhubd checks it
static cJSON *arg_value(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (end != s && *end == '\0' && isfinite(v))
        return cJSON_CreateNumber(v);
    return cJSON_CreateString(s);
}

cli_kind_t cli_parse(const char *line, cJSON **req, char *err, size_t errlen)
{
    *req = NULL;
    while (*line == ' ' || *line == '\t')
        line++;

    if (*line == '{') {
        cJSON *obj = cJSON_ParseWithOpts(line, NULL, true);
        if (!cJSON_IsObject(obj)) {
            cJSON_Delete(obj);
            snprintf(err, errlen, "not a JSON object");
            return CLI_ERROR;
        }
        *req = obj;
        return CLI_RAW;
    }

    // Split into words
    char copy[1024];
    if (strlen(line) >= sizeof copy) {
        snprintf(err, errlen, "line too long");
        return CLI_ERROR;
    }
    strcpy(copy, line);
    char *words[MAX_WORDS];
    size_t n = 0;
    for (char *w = strtok(copy, " \t\r\n"); w != NULL; w = strtok(NULL, " \t\r\n")) {
        if (n == MAX_WORDS) {
            snprintf(err, errlen, "too many words; usage: <dev> <cmd> [arg]");
            return CLI_ERROR;
        }
        words[n++] = w;
    }

    if (n == 0)
        return CLI_EMPTY;
    if (n == 1 && (strcmp(words[0], "help") == 0 || strcmp(words[0], "?") == 0))
        return CLI_HELP;
    if (n == 1 && (strcmp(words[0], "quit") == 0 || strcmp(words[0], "exit") == 0))
        return CLI_QUIT;
    if (n > 3) {
        snprintf(err, errlen, "too many words; usage: <dev> <cmd> [arg]");
        return CLI_ERROR;
    }

    cJSON *r = cJSON_CreateObject();
    if (n == 1) {
        cJSON_AddStringToObject(r, "cmd", words[0]);
    } else {
        cJSON_AddStringToObject(r, "dev", words[0]);
        cJSON_AddStringToObject(r, "cmd", words[1]);
        if (n == 3)
            cJSON_AddItemToObject(r, arg_field(words[1]), arg_value(words[2]));
    }
    *req = r;
    return CLI_REQUEST;
}

const char *cli_help(void)
{
    return "Commands (docs/daemon/spec.md):\n"
           "  <dev> rel <steps>     relative move, full steps\n"
           "  <dev> abs <pos>       absolute move (needs a valid position)\n"
           "  <dev> stop            stop the move; the reply has where it ends\n"
           "  <dev> pos             position\n"
           "  <dev> status          position, moving, locked\n"
           "  <dev> speed <value>   full steps/s\n"
           "  <dev> accel <value>   full steps/s^2\n"
           "  <dev> micro <value>   microsteps: 1, 2, 4, 8, 16\n"
           "  <dev> zero            set the position to 0 (makes it valid)\n"
           "  <dev> lock|unlock     enable / disable the driver (unlock voids the position)\n"
           "  devices               list the devices\n"
           "  estop                 emergency stop: every axis, drivers disabled\n"
           "  lease / release       hold the lease across commands (renewed in the\n"
           "                        background); otherwise each write command takes\n"
           "                        it and releases it by itself\n"
           "  {...}                 a raw JSON request, sent as it is (no automatic\n"
           "                        lease); seq is added if missing\n"
           "  help                  this help\n"
           "  quit                  leave (also Ctrl-D, or Ctrl-C on an empty line)\n"
           "Replies are shown as nimhubd sends them.\n";
}
