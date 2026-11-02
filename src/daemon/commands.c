#include "commands.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

// A request waiting for the MCU's reply: what to do with the model once the
// MCU has accepted it
typedef enum {
    FW_MOVE,            // move, move_to
    FW_STOP,
    FW_ESTOP,
    FW_SET,             // set_motion with one setting (speed, accel, micro)
    FW_ZERO,            // set_position 0
    FW_LOCK,            // set_enable 1
    FW_UNLOCK,          // set_enable 0
} fw_kind_t;

typedef struct {
    commands_t *c;
    server_client_t client;
    double seq;
    fw_kind_t kind;
    model_device_t *dev;        // NULL for estop
    model_setting_t setting;    // FW_SET
    int32_t value;              // FW_SET
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
    command_class_t cls;        // D-R13
    command_fn fn;              // NULL: defined in the spec, not implemented yet
} command_t;

void commands_init(commands_t *c, link_t *link, model_t *model,
                   const handshake_t *hs, server_t *server, uint32_t lease_timeout_ms)
{
    c->link = link;
    c->model = model;
    c->hs = hs;
    c->server = server;
    lease_init(&c->lease, lease_timeout_ms);
    memset(c->watching, 0, sizeof c->watching);
}

// A client as a lease key: never 0, and a new one for every connection
static uint64_t lease_key(server_client_t client)
{
    return (uint64_t)(client.index + 1) << 32 | client.gen;
}

void commands_client_gone(void *ctx, server_client_t client)
{
    commands_t *c = ctx;
    // Freed at once on disconnect (D-R18)
    if (lease_holder(&c->lease, monotonic_ms()) == lease_key(client))
        log_info("client %u: lease freed (disconnected)", server_client_id(c->server, client));
    lease_release(&c->lease, lease_key(client));
    c->watching[client.index] = false;
}

// ---- Status events (D-R21) -----

// A stepper's reported state, from one snapshot of the model (D-R22): the
// fields of status replies and status events
static void add_state(cJSON *msg, const model_device_t *d)
{
    cJSON_AddNumberToObject(msg, "pos", d->pos);
    cJSON_AddBoolToObject(msg, "pos_valid", d->pos_valid);
    cJSON_AddBoolToObject(msg, "moving", d->moving);
    cJSON_AddBoolToObject(msg, "locked", d->locked);
}

static void send_status_event(commands_t *c, server_client_t client, const model_device_t *d)
{
    cJSON *evt = cJSON_CreateObject();
    cJSON_AddStringToObject(evt, "evt", "status");
    cJSON_AddStringToObject(evt, "dev", d->oid);
    add_state(evt, d);
    server_send(c->server, client, evt);
    cJSON_Delete(evt);
}

static bool is_watching(const commands_t *c, int index)
{
    return c->watching[index] && server_client_id(c->server,
               (server_client_t){index, c->watching_gen[index]}) != 0;
}

void commands_broadcast(commands_t *c)
{
    model_t *m = c->model;

    for (size_t i = 0; i < m->count; i++) {
        model_device_t *d = &m->devices[i];
        if (!model_is_stepper(d) || !model_take_change(d))
            continue;
        for (int k = 0; k < SERVER_MAX_CLIENTS; k++)
            if (is_watching(c, k))
                send_status_event(c, (server_client_t){k, c->watching_gen[k]}, d);
    }
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
        case FW_SET:
            model_set_setting(f->dev, f->setting, f->value);
            break;
        case FW_ZERO:
            model_zeroed(f->dev);
            break;
        case FW_LOCK:
        case FW_UNLOCK:
            model_enabled(f->dev, f->kind == FW_LOCK);
            break;
        }
        send_reply(c, f->client, out);
    }
    // After the reply, so the client that sent the command gets it first
    commands_broadcast(c);
    server_release(c->server, f->client);
    free(f);
}

// Send req (and free it) for r; the client's reply comes from the MCU's.
// setting and value are for FW_SET.
static void forward_set(const request_t *r, cJSON *req, fw_kind_t kind,
                        model_setting_t setting, int32_t value)
{
    fw_request_t *f = malloc(sizeof *f);
    *f = (fw_request_t){r->c, r->client, r->seq, kind, r->dev, setting, value};

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

static void forward(const request_t *r, cJSON *req, fw_kind_t kind)
{
    forward_set(r, req, kind, MODEL_SPEED, 0);
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
    cJSON *reply = new_reply(r->seq, true);
    add_state(reply, r->dev);
    send_reply(r->c, r->client, reply);
}

// speed, accel, micro {value}: one set_motion field. The firmware checks the
// value and the combination (F-R2), and refuses while moving (F-R10).
static void set_motion(const request_t *r, const char *field, model_setting_t setting)
{
    int32_t value;
    if (!get_int32(r, "value", &value))
        return;
    cJSON *req = link_new_request("set_motion", r->dev->oid);
    cJSON_AddNumberToObject(req, field, value);
    forward_set(r, req, FW_SET, setting, value);
}

static void cmd_speed(const request_t *r)
{
    set_motion(r, "speed", MODEL_SPEED);
}

static void cmd_accel(const request_t *r)
{
    set_motion(r, "accel", MODEL_ACCEL);
}

static void cmd_micro(const request_t *r)
{
    set_motion(r, "microsteps", MODEL_MICRO);
}

static void cmd_zero(const request_t *r)
{
    cJSON *req = link_new_request("set_position", r->dev->oid);
    cJSON_AddNumberToObject(req, "pos", 0);
    forward(r, req, FW_ZERO);
}

static void set_enable(const request_t *r, int value)
{
    cJSON *req = link_new_request("set_enable", r->dev->oid);
    cJSON_AddNumberToObject(req, "value", value);
    forward(r, req, value ? FW_LOCK : FW_UNLOCK);
}

static void cmd_lock(const request_t *r)
{
    set_enable(r, 1);
}

static void cmd_unlock(const request_t *r)
{
    set_enable(r, 0);
}

static const command_t motor_commands[] = {
    {"rel",    CLASS_WRITE, cmd_rel},
    {"abs",    CLASS_WRITE, cmd_abs},
    {"stop",   CLASS_WRITE, cmd_stop},
    {"pos",    CLASS_READ,  cmd_pos},
    {"status", CLASS_READ,  cmd_status},
    {"speed",  CLASS_WRITE, cmd_speed},
    {"accel",  CLASS_WRITE, cmd_accel},
    {"micro",  CLASS_WRITE, cmd_micro},
    {"zero",   CLASS_WRITE, cmd_zero},
    {"home",   CLASS_WRITE, NULL},
    {"lock",   CLASS_WRITE, cmd_lock},
    {"unlock", CLASS_WRITE, cmd_unlock},
    {NULL, 0, NULL},
};

// ---- Daemon commands -----

// F-R24. Sent whenever the link is up, even during the handshake.
static void cmd_estop(const request_t *r)
{
    forward(r, link_new_request("estop", NULL), FW_ESTOP);
}

// watch (D-R21): the reply, then the current state of every device, then an
// event whenever it changes. Repeating it sends the current state again.
static void cmd_watch(const request_t *r)
{
    commands_t *c = r->c;
    const model_t *m = c->model;

    c->watching[r->client.index] = true;
    c->watching_gen[r->client.index] = r->client.gen;
    send_reply(c, r->client, new_reply(r->seq, true));
    for (size_t i = 0; i < m->count; i++)
        if (model_is_stepper(&m->devices[i]) && m->devices[i].known)
            send_status_event(c, r->client, &m->devices[i]);
}

static void cmd_unwatch(const request_t *r)
{
    r->c->watching[r->client.index] = false;
    send_reply(r->c, r->client, new_reply(r->seq, true));
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

// The lease (D-R18). lease and renew return timeout_ms, the time it is
// held for without renewal.
static void lease_reply(const request_t *r, bool ok)
{
    if (!ok) {
        reply_error(r, "lease_held", "lease held by another client");
        return;
    }
    cJSON *reply = new_reply(r->seq, true);
    cJSON_AddNumberToObject(reply, "timeout_ms", r->c->lease.timeout_ms);
    send_reply(r->c, r->client, reply);
}

static void cmd_lease(const request_t *r)
{
    lease_t *l = &r->c->lease;
    bool had = lease_holder(l, monotonic_ms()) == lease_key(r->client);
    bool ok = lease_take(l, lease_key(r->client), monotonic_ms());
    if (ok && !had)
        log_info("client %u: lease taken", server_client_id(r->c->server, r->client));
    lease_reply(r, ok);
}

static void cmd_renew(const request_t *r)
{
    bool ok = lease_renew(&r->c->lease, lease_key(r->client), monotonic_ms());
    if (!ok) {
        reply_error(r, "lease_held", "this client doesn't hold the lease");
        return;
    }
    lease_reply(r, true);
}

static void cmd_release(const request_t *r)
{
    lease_t *l = &r->c->lease;
    if (lease_holder(l, monotonic_ms()) == lease_key(r->client))
        log_info("client %u: lease released", server_client_id(r->c->server, r->client));
    lease_release(l, lease_key(r->client));
    send_reply(r->c, r->client, new_reply(r->seq, true));
}

static const command_t daemon_commands[] = {
    {"estop",   CLASS_EMERGENCY, cmd_estop},
    {"devices", CLASS_READ,      cmd_devices},
    {"lease",   CLASS_SESSION,   cmd_lease},
    {"renew",   CLASS_SESSION,   cmd_renew},
    {"release", CLASS_SESSION,   cmd_release},
    {"watch",   CLASS_READ,      cmd_watch},
    {"unwatch", CLASS_READ,      cmd_unwatch},
    {NULL, 0, NULL},
};

// ---- Dispatch -----

static const command_t *find(const command_t *table, const char *name)
{
    for (; table->name != NULL; table++)
        if (strcmp(table->name, name) == 0)
            return table;
    return NULL;
}

// The lease check (D-R16–D-R18), then the command
static void run(const request_t *r, const command_t *cmd)
{
    if (!lease_check(&r->c->lease, cmd->cls, lease_key(r->client), monotonic_ms()))
        reply_error(r, "lease_held", lease_holder(&r->c->lease, monotonic_ms()) == 0
                    ? "take the lease first" : "lease held by another client");
    else if (cmd->fn == NULL)
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
