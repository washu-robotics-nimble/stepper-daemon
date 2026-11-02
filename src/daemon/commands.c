#include "commands.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

// A request waiting for the MCU's reply
typedef enum { FW_MOVE, FW_STOP, FW_ESTOP } fw_kind_t;

typedef struct {
    commands_t *c;
    server_client_t client;
    double seq;
    fw_kind_t kind;
    model_device_t *dev;        // NULL for estop
} fw_request_t;

// The context of one client request
typedef struct {
    commands_t *c;
    server_client_t client;
    double seq;
    const cJSON *req;
    model_device_t *dev;        // NULL for daemon commands
} request_t;

typedef void (*command_fn)(const request_t *r);

typedef struct {
    const char *name;
    command_fn fn;              // NULL: defined in the spec, not implemented yet
} command_t;

void commands_init(commands_t *c, link_t *link, model_t *model,
                   const handshake_t *hs, server_t *server)
{
    c->link = link;
    c->model = model;
    c->hs = hs;
    c->server = server;
}

// ---- Replies -----

// {"seq":n,"ack":...}; seq first, as in the spec's examples
static cJSON *new_reply(double seq, bool ack)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "seq", seq);
    cJSON_AddBoolToObject(reply, "ack", ack);
    return reply;
}

static void send_reply(commands_t *c, server_client_t client, cJSON *reply)
{
    server_send(c->server, client, reply);
    cJSON_Delete(reply);
}

static void send_error(commands_t *c, server_client_t client, double seq,
                       const char *err, const char *msg)
{
    cJSON *reply = new_reply(seq, false);
    cJSON_AddStringToObject(reply, "err", err);
    if (msg != NULL)
        cJSON_AddStringToObject(reply, "msg", msg);
    send_reply(c, client, reply);
}

static void reply_error(const request_t *r, const char *err, const char *msg)
{
    send_error(r->c, r->client, r->seq, err, msg);
}

// D-R40: a line whose seq can't be read gets an event instead of a reply
static void bad_request_event(commands_t *c, server_client_t client, const char *msg)
{
    cJSON *evt = cJSON_CreateObject();
    cJSON_AddStringToObject(evt, "evt", "bad_request");
    cJSON_AddStringToObject(evt, "msg", msg);
    server_send(c->server, client, evt);
    cJSON_Delete(evt);
}

// ---- Requests to the MCU -----

// A firmware error code as a client one; most are the same
static const char *client_error(const char *fw)
{
    if (strcmp(fw, "unknown_oid") == 0)
        return "unknown_dev";
    if (strcmp(fw, "not_configured") == 0)
        return "link_down";
    return fw;
}

static void on_fw_reply(void *ctx, const cJSON *reply)
{
    fw_request_t *f = ctx;
    commands_t *c = f->c;

    if (reply == NULL) {
        send_error(c, f->client, f->seq, "link_down", "the link to the MCU went down");
    } else if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ack"))) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(reply, "err");
        send_error(c, f->client, f->seq,
                   client_error(cJSON_IsString(err) ? err->valuestring : "?"),
                   "rejected by the MCU");
    } else {
        cJSON *out = new_reply(f->seq, true);
        switch (f->kind) {
        case FW_MOVE:
            model_move_accepted(f->dev);
            break;
        case FW_STOP: {
            // The position the move ends at (F-R21)
            const cJSON *pos = cJSON_GetObjectItemCaseSensitive(reply, "pos");
            if (cJSON_IsNumber(pos))
                cJSON_AddNumberToObject(out, "pos", pos->valuedouble);
            break;
        }
        case FW_ESTOP:
            model_estop(c->model);
            break;
        }
        send_reply(c, f->client, out);
    }
    server_release(c->server, f->client);
    free(f);
}

// Send req (and free it) for r; the client's reply comes from the MCU's
static void forward(const request_t *r, cJSON *req, fw_kind_t kind)
{
    fw_request_t *f = malloc(sizeof *f);
    *f = (fw_request_t){r->c, r->client, r->seq, kind, r->dev};

    // Released in on_fw_reply(), which link_request() may call before it
    // returns
    server_hold(r->c->server, r->client);
    bool sent = link_request(r->c->link, req, on_fw_reply, f);
    cJSON_Delete(req);
    if (!sent) {
        free(f);
        reply_error(r, "link_down", "cannot send to the MCU");
        server_release(r->c->server, r->client);
    }
}

// An int32 argument, or false (with a bad_param reply sent)
static bool get_int32(const request_t *r, const char *key, int32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r->req, key);
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble)
        || v->valuedouble < INT32_MIN || v->valuedouble > INT32_MAX) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s must be an integer in int32 range", key);
        reply_error(r, "bad_param", msg);
        return false;
    }
    *out = (int32_t)v->valuedouble;
    return true;
}

// ---- Motor commands -----

static void cmd_rel(const request_t *r)
{
    int32_t steps;
    if (!get_int32(r, "steps", &steps))
        return;
    cJSON *req = link_new_request("move", r->dev->oid);
    cJSON_AddNumberToObject(req, "steps", steps);
    forward(r, req, FW_MOVE);
}

static void cmd_abs(const request_t *r)
{
    int32_t pos;
    if (!get_int32(r, "pos", &pos))
        return;
    // An absolute target means nothing without a known position. The
    // firmware checks this only when soft limits are set (F-R25).
    if (!r->dev->pos_valid) {
        reply_error(r, "pos_invalid", "position invalid");
        return;
    }
    cJSON *req = link_new_request("move_to", r->dev->oid);
    cJSON_AddNumberToObject(req, "pos", pos);
    forward(r, req, FW_MOVE);
}

static void cmd_stop(const request_t *r)
{
    forward(r, link_new_request("stop", r->dev->oid), FW_STOP);
}

// Reads come from the machine model (D-R20, D-R22): one snapshot
static void cmd_pos(const request_t *r)
{
    cJSON *reply = new_reply(r->seq, true);
    cJSON_AddNumberToObject(reply, "pos", r->dev->pos);
    cJSON_AddBoolToObject(reply, "pos_valid", r->dev->pos_valid);
    send_reply(r->c, r->client, reply);
}

static void cmd_status(const request_t *r)
{
    const model_device_t *d = r->dev;
    cJSON *reply = new_reply(r->seq, true);
    cJSON_AddNumberToObject(reply, "pos", d->pos);
    cJSON_AddBoolToObject(reply, "pos_valid", d->pos_valid);
    cJSON_AddBoolToObject(reply, "moving", d->moving);
    cJSON_AddBoolToObject(reply, "locked", d->locked);
    send_reply(r->c, r->client, reply);
}

static const command_t motor_commands[] = {
    {"rel",    cmd_rel},
    {"abs",    cmd_abs},
    {"stop",   cmd_stop},
    {"pos",    cmd_pos},
    {"status", cmd_status},
    {"speed",  NULL},
    {"accel",  NULL},
    {"micro",  NULL},
    {"zero",   NULL},
    {"home",   NULL},
    {"lock",   NULL},
    {"unlock", NULL},
    {NULL, NULL},
};

// ---- Daemon commands -----

// F-R24. Sent whenever the link is up, even during the handshake.
static void cmd_estop(const request_t *r)
{
    forward(r, link_new_request("estop", NULL), FW_ESTOP);
}

// The devices the firmware reported (D-R43), for clients to discover them
static void cmd_devices(const request_t *r)
{
    const model_t *m = r->c->model;

    if (r->c->hs->status != HANDSHAKE_READY) {
        reply_error(r, "link_down", "the MCU isn't connected and configured");
        return;
    }
    cJSON *reply = new_reply(r->seq, true);
    cJSON *list = cJSON_AddArrayToObject(reply, "devices");
    for (size_t i = 0; i < m->count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "dev", m->devices[i].oid);
        cJSON_AddStringToObject(d, "type", m->devices[i].type);
        cJSON_AddItemToArray(list, d);
    }
    send_reply(r->c, r->client, reply);
}

static const command_t daemon_commands[] = {
    {"estop",   cmd_estop},
    {"devices", cmd_devices},
    {"lease",   NULL},
    {"renew",   NULL},
    {"release", NULL},
    {"watch",   NULL},
    {"unwatch", NULL},
    {NULL, NULL},
};

// ---- Dispatch -----

static const command_t *find(const command_t *table, const char *name)
{
    for (; table->name != NULL; table++)
        if (strcmp(table->name, name) == 0)
            return table;
    return NULL;
}

static void run(const request_t *r, const command_t *cmd)
{
    if (cmd->fn == NULL)
        reply_error(r, "not_impl", "not implemented yet");
    else
        cmd->fn(r);
}

static bool valid_seq(const cJSON *seq)
{
    return cJSON_IsNumber(seq) && seq->valuedouble >= 0
        && seq->valuedouble <= 4294967295.0 && seq->valuedouble == floor(seq->valuedouble);
}

static void dispatch(request_t *r, const char *name, const cJSON *dev)
{
    commands_t *c = r->c;

    // Daemon commands have no dev (D-R14)
    if (dev == NULL) {
        const command_t *cmd = find(daemon_commands, name);
        if (cmd != NULL) {
            if (!link_up(c->link))
                reply_error(r, "link_down", "no link to the MCU");
            else
                run(r, cmd);
        } else if (find(motor_commands, name) != NULL) {
            reply_error(r, "unknown_cmd", "a device command needs dev");
        } else {
            reply_error(r, "unknown_cmd", NULL);
        }
        return;
    }

    // Devices are known, and their state valid, once the handshake is done
    // (D-R3, D-R30)
    if (!link_up(c->link) || c->hs->status != HANDSHAKE_READY) {
        reply_error(r, "link_down", "the MCU isn't connected and configured");
        return;
    }
    r->dev = model_find(c->model, dev->valuestring);
    if (r->dev == NULL) {
        reply_error(r, "unknown_dev", NULL);
        return;
    }
    const command_t *cmd = model_is_stepper(r->dev) ? find(motor_commands, name) : NULL;
    if (cmd == NULL)
        reply_error(r, "unknown_cmd", NULL);
    else
        run(r, cmd);
}

void commands_line(void *ctx, server_client_t client, char *line, size_t len)
{
    commands_t *c = ctx;

    if (line == NULL) {
        bad_request_event(c, client, "line over 4096 bytes");
        return;
    }
    if (strlen(line) != len) {
        bad_request_event(c, client, "line contains a NUL byte");
        return;
    }

    cJSON *req = cJSON_ParseWithOpts(line, NULL, true);
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(req, "seq");
    if (!cJSON_IsObject(req)) {
        bad_request_event(c, client, "not a JSON object");
    } else if (!valid_seq(seq)) {
        bad_request_event(c, client, "seq missing, or not an integer in uint32 range");
    } else {
        request_t r = {c, client, seq->valuedouble, req, NULL};
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
        const cJSON *dev = cJSON_GetObjectItemCaseSensitive(req, "dev");
        if (!cJSON_IsString(cmd))
            reply_error(&r, "bad_request", "cmd missing, or not a string");
        else if (dev != NULL && !cJSON_IsString(dev))
            reply_error(&r, "bad_request", "dev must be a string");
        else
            dispatch(&r, cmd->valuestring, dev);
    }
    cJSON_Delete(req);
}
