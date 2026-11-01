#include "hostlink.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"
#include "board.h"
#include "linebuf.h"
#include "stepper.h"
#include "version.h"

static hostlink_write_fn write_line;
static char out[LINEBUF_MAX];

// Link settings (set_link) and the F-R33 flag
#define REPORT_PERIOD_DEFAULT_MS 250
static bool settings_applied;
static uint32_t report_period_ms = REPORT_PERIOD_DEFAULT_MS;
static uint32_t timeout_ms;   // F-R22, 0 = infinity; used from step 7

void hostlink_init(hostlink_write_fn write)
{
    write_line = write;
}

bool hostlink_settings_applied(void)
{
    return settings_applied;
}

uint32_t hostlink_report_period_ms(void)
{
    return report_period_ms;
}

// Send msg as one line. cJSON needs a few spare bytes, and one more is kept
// for the '\n', so a frame never exceeds LINEBUF_MAX. A message too long to
// fit is not sent; the host then times out (D-R30). Only a firmware bug
// can cause that.
void hostlink_send(const cJSON *msg)
{
    if (!cJSON_PrintPreallocated((cJSON *)msg, out, sizeof out - 1, false))
        return;
    size_t n = strlen(out);
    out[n++] = '\n';
    write_line(out, n);
}

static void send_error_reply(double seq, const char *err)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "seq", seq);
    cJSON_AddBoolToObject(reply, "ack", false);
    cJSON_AddStringToObject(reply, "err", err);
    hostlink_send(reply);
    cJSON_Delete(reply);
}

void hostlink_rx_error(const char *err)
{
    cJSON *evt = cJSON_CreateObject();
    cJSON_AddStringToObject(evt, "evt", "rx_error");
    cJSON_AddStringToObject(evt, "err", err);
    hostlink_send(evt);
    cJSON_Delete(evt);
}

// ---- Global commands ----

static const char *cmd_identify(const cJSON *req, cJSON *reply, size_t index)
{
    (void)req;
    (void)index;
    cJSON_AddStringToObject(reply, "fw", FW_VERSION);
    cJSON_AddNumberToObject(reply, "proto", HOSTLINK_PROTO);
    cJSON_AddBoolToObject(reply, "settings_applied", settings_applied);
    return NULL;
}

static const char *cmd_settings_done(const cJSON *req, cJSON *reply, size_t index)
{
    (void)req;
    (void)reply;
    (void)index;
    settings_applied = true;
    return NULL;
}

static const char *cmd_set_link(const cJSON *req, cJSON *reply, size_t index)
{
    (void)reply;
    (void)index;
    int32_t period = (int32_t)report_period_ms;
    int32_t timeout = (int32_t)timeout_ms;

    // All or nothing
    if (!hostlink_get_opt_int32(req, "report_period", 0, INT32_MAX, &period)
        || !hostlink_get_opt_int32(req, "timeout_ms", 0, INT32_MAX, &timeout))
        return "bad_param";
    report_period_ms = (uint32_t)period;
    timeout_ms = (uint32_t)timeout;
    return NULL;
}

static const char *cmd_list_objects(const cJSON *req, cJSON *reply, size_t index);

static const char *cmd_ping(const cJSON *req, cJSON *reply, size_t index)
{
    (void)req;
    (void)reply;
    (void)index;
    return NULL;
}

// F-R24. The receive path runs it ahead of queued lines (hostlink_is_estop())
static const char *cmd_estop(const cJSON *req, cJSON *reply, size_t index)
{
    (void)req;
    (void)reply;
    (void)index;
    stepper_estop();
    return NULL;
}

static const hostlink_cmd_t global_cmds[] = {
    {"identify",      cmd_identify},
    {"list_objects",  cmd_list_objects},
    {"settings_done", cmd_settings_done},
    {"set_link",      cmd_set_link},
    {"ping",          cmd_ping},
    {"estop",         cmd_estop},
    {NULL, NULL},
};

// ---- Object commands -----

typedef struct {
    const char *type;
    const hostlink_cmd_t *cmds;
} type_entry_t;

static const type_entry_t types[] = {
    {"stepper", stepper_cmds},
};

static const hostlink_cmd_t *find_cmd(const hostlink_cmd_t *table, const char *name)
{
    for (; table->name != NULL; table++)
        if (strcmp(table->name, name) == 0)
            return table;
    return NULL;
}

// Look up an object by oid; returns its type entry and sets *index to its
// position in its board table, or returns NULL if unknown
static const type_entry_t *find_object(const char *oid, size_t *index)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        if (strcmp(board_steppers[i].oid, oid) == 0) {
            *index = i;
            return &types[0];
        }
    }
    return NULL;
}

static const char *cmd_list_objects(const cJSON *req, cJSON *reply, size_t index)
{
    (void)req;
    (void)index;

    cJSON *objects = cJSON_AddArrayToObject(reply, "objects");
    for (size_t i = 0; i < board_stepper_count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "oid", board_steppers[i].oid);
        cJSON_AddStringToObject(obj, "type", "stepper");
        cJSON_AddItemToArray(objects, obj);
    }

    cJSON *type_map = cJSON_AddObjectToObject(reply, "types");
    for (size_t t = 0; t < sizeof types / sizeof types[0]; t++) {
        cJSON *names = cJSON_AddArrayToObject(type_map, types[t].type);
        for (const hostlink_cmd_t *c = types[t].cmds; c->name != NULL; c++)
            cJSON_AddItemToArray(names, cJSON_CreateString(c->name));
    }
    return NULL;
}

// ---- Dispatch -----

// seq must be an integer in uint32 range (F-R31). Read through valuedouble,
// which is exact for every uint32 (valueint clamps at INT_MAX).
static bool valid_seq(const cJSON *seq)
{
    if (!cJSON_IsNumber(seq))
        return false;
    double v = seq->valuedouble;
    return v >= 0 && v <= 4294967295.0 && v == floor(v);
}

bool hostlink_get_int32(const cJSON *req, const char *key, int32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(req, key);
    if (!cJSON_IsNumber(item))
        return false;
    double v = item->valuedouble;
    if (v < INT32_MIN || v > INT32_MAX || v != floor(v))
        return false;
    *out = (int32_t)v;
    return true;
}

bool hostlink_get_opt_int32(const cJSON *req, const char *key, int32_t min,
                            int32_t max, int32_t *out)
{
    int32_t v;

    if (cJSON_GetObjectItemCaseSensitive(req, key) == NULL)
        return true;
    if (!hostlink_get_int32(req, key, &v) || v < min || v > max)
        return false;
    *out = v;
    return true;
}

static const hostlink_cmd_t *resolve(const cJSON *req, const char *name,
                                     size_t *index, const char **err)
{
    const hostlink_cmd_t *cmd = find_cmd(global_cmds, name);
    if (cmd != NULL)
        return cmd;

    const cJSON *oid = cJSON_GetObjectItemCaseSensitive(req, "oid");
    if (oid == NULL) {
        *err = "unknown_cmd";
        return NULL;
    }
    if (!cJSON_IsString(oid)) {
        *err = "bad_param";
        return NULL;
    }

    const type_entry_t *type = find_object(oid->valuestring, index);
    if (type == NULL) {
        *err = "unknown_oid";
        return NULL;
    }

    cmd = find_cmd(type->cmds, name);
    if (cmd == NULL)
        *err = "unknown_cmd";
    return cmd;
}

bool hostlink_is_estop(const char *line, size_t len)
{
    // Cheap test first: most lines never get parsed twice
    if (strlen(line) != len || strstr(line, "estop") == NULL)
        return false;

    cJSON *req = cJSON_ParseWithOpts(line, NULL, true);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    bool estop = cJSON_IsString(name) && strcmp(name->valuestring, "estop") == 0;
    cJSON_Delete(req);
    return estop;
}

void hostlink_handle_line(const char *line, size_t len)
{
    // An embedded NUL would hide the rest of the line from the parser
    if (strlen(line) != len) {
        hostlink_rx_error("bad_json");
        return;
    }

    // Require the whole line to be one JSON object (trailing whitespace,
    // such as a '\r' from a terminal, is allowed)
    cJSON *req = cJSON_ParseWithOpts(line, NULL, true);
    if (!cJSON_IsObject(req)) {
        cJSON_Delete(req);
        hostlink_rx_error("bad_json");
        return;
    }

    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(req, "seq");
    if (!valid_seq(seq)) {
        cJSON_Delete(req);
        hostlink_rx_error("no_seq");
        return;
    }
    double seq_v = seq->valuedouble;

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    const char *err = "unknown_cmd";
    const hostlink_cmd_t *cmd = NULL;
    size_t index = 0;
    if (cJSON_IsString(name))
        cmd = resolve(req, name->valuestring, &index, &err);

    if (cmd != NULL) {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddNumberToObject(reply, "seq", seq_v);
        cJSON_AddBoolToObject(reply, "ack", true);
        err = cmd->fn(req, reply, index);
        if (err == NULL)
            hostlink_send(reply);
        cJSON_Delete(reply);
    }

    if (err != NULL)
        send_error_reply(seq_v, err);

    cJSON_Delete(req);
}
