#include "handshake.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "log.h"

static void on_identify(void *ctx, const cJSON *reply);
static void on_list_objects(void *ctx, const cJSON *reply);
static void on_set_link(void *ctx, const cJSON *reply);
static void on_set_motion(void *ctx, const cJSON *reply);
static void on_settings_done(void *ctx, const cJSON *reply);
static void on_get_state(void *ctx, const cJSON *reply);

static void fail(handshake_t *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void fail(handshake_t *h, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_error("handshake failed: %s", msg);
    h->status = HANDSHAKE_FAILED;
}

// Send req (and free it); fn gets the reply
static void send(handshake_t *h, cJSON *req, link_reply_fn fn)
{
    bool sent = link_request(h->link, req, fn, h);
    cJSON_Delete(req);
    // Not sent with the link up: a frame problem, already logged
    if (!sent && link_up(h->link))
        h->status = HANDSHAKE_FAILED;
}

static bool acked(const cJSON *reply)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ack"));
}

static const char *error_code(const cJSON *reply)
{
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(reply, "err");
    return cJSON_IsString(err) ? err->valuestring : "?";
}

void handshake_start(handshake_t *h, link_t *link, const config_t *cfg, model_t *model)
{
    memset(h, 0, sizeof *h);
    h->link = link;
    h->cfg = cfg;
    h->model = model;
    h->status = HANDSHAKE_RUNNING;
    model_reset(model);
    log_info("handshake: identify");
    send(h, link_new_request("identify", NULL), on_identify);
}

// F-R33: the first exchange; refuse a protocol this daemon doesn't speak.
// Every callback gets NULL if the link went down: the main loop sees that.
static void on_identify(void *ctx, const cJSON *reply)
{
    handshake_t *h = ctx;
    if (reply == NULL)
        return;
    const cJSON *fw = cJSON_GetObjectItemCaseSensitive(reply, "fw");
    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(reply, "proto");
    const cJSON *applied = cJSON_GetObjectItemCaseSensitive(reply, "settings_applied");

    if (!acked(reply)) {
        fail(h, "identify: %s", error_code(reply));
        return;
    }
    if (!cJSON_IsString(fw) || !cJSON_IsNumber(proto) || !cJSON_IsBool(applied)) {
        fail(h, "identify: reply without fw, proto or settings_applied");
        return;
    }
    if (proto->valuedouble != HANDSHAKE_PROTO) {
        fail(h, "the firmware speaks protocol %g; this daemon supports %d",
             proto->valuedouble, HANDSHAKE_PROTO);
        return;
    }
    snprintf(h->fw, sizeof h->fw, "%s", fw->valuestring);
    log_info("handshake: firmware %s, protocol %d, settings_applied %s",
             h->fw, HANDSHAKE_PROTO, cJSON_IsTrue(applied) ? "true" : "false");
    send(h, link_new_request("list_objects", NULL), on_list_objects);
}

// Every value in a device's section must be a setting of its type
static bool check_section(handshake_t *h, const model_device_t *d,
                          const config_device_t *dev)
{
    for (size_t i = 0; i < dev->count; i++) {
        const config_key_t *key = dev->values[i].key;
        if (strcmp(key->type, d->type) != 0) {
            fail(h, "[%s]: '%s' is not a setting of a %s", d->oid, key->name, d->type);
            return false;
        }
    }
    return true;
}

// F-R35, D-R43: the objects decide which INI sections apply
static void on_list_objects(void *ctx, const cJSON *reply)
{
    handshake_t *h = ctx;
    if (reply == NULL)
        return;
    const config_t *cfg = h->cfg;
    model_t *model = h->model;
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(reply, "objects");
    const cJSON *item;

    if (!acked(reply)) {
        fail(h, "list_objects: %s", error_code(reply));
        return;
    }
    if (!cJSON_IsArray(objects)) {
        fail(h, "list_objects: reply without objects");
        return;
    }
    cJSON_ArrayForEach(item, objects) {
        const cJSON *oid = cJSON_GetObjectItemCaseSensitive(item, "oid");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(oid) || !cJSON_IsString(type)) {
            fail(h, "list_objects: bad object entry");
            return;
        }
        if (model_add(model, oid->valuestring, type->valuestring) == NULL) {
            fail(h, "list_objects: more than %d objects, or a name too long",
                 MODEL_MAX_DEVICES);
            return;
        }
    }

    for (size_t i = 0; i < model->count; i++) {
        const model_device_t *d = &model->devices[i];
        const config_device_t *dev = config_find_device(cfg, d->oid);
        if (dev == NULL) {
            log_info("handshake: %s (%s): no INI section, firmware defaults", d->oid, d->type);
            continue;
        }
        log_info("handshake: %s (%s): INI section [%s]", d->oid, d->type, dev->oid);
        if (!check_section(h, d, dev))
            return;
    }
    for (size_t i = 0; i < cfg->device_count; i++) {
        const char *oid = cfg->devices[i].oid;
        if (model_find(model, oid) == NULL)
            log_warn("[%s] (line %d): the MCU has no such object; section ignored",
                     oid, cfg->devices[i].line);
    }

    // The [link] settings always go out, so none is left over from an
    // earlier session
    cJSON *req = link_new_request("set_link", NULL);
    cJSON_AddNumberToObject(req, "report_period", cfg->report_period);
    cJSON_AddNumberToObject(req, "timeout_ms", cfg->timeout_ms);
    log_info("handshake: set_link report_period %ld, timeout_ms %ld",
             (long)cfg->report_period, (long)cfg->timeout_ms);
    send(h, req, on_set_link);
}

void handshake_motion_fields(const config_device_t *dev, cJSON *req)
{
    for (size_t i = 0; i < dev->count; i++) {
        const config_value_t *v = &dev->values[i];
        if (!v->key->firmware)
            continue;
        if (v->key->kind == CONFIG_POLARITY)
            cJSON_AddStringToObject(req, v->key->name, v->value == 'H' ? "H" : "L");
        else
            cJSON_AddNumberToObject(req, v->key->name, v->value);
    }
    if (config_get(dev, "soft_limit_min") == NULL)
        cJSON_AddNullToObject(req, "soft_limit_min");
    if (config_get(dev, "soft_limit_max") == NULL)
        cJSON_AddNullToObject(req, "soft_limit_max");
}

// set_motion for the next stepper with an INI section, from h->next on;
// settings_done once there is none left
static void send_next_motion(handshake_t *h)
{
    model_t *model = h->model;

    for (; h->next < model->count; h->next++) {
        const model_device_t *d = &model->devices[h->next];
        const config_device_t *dev = config_find_device(h->cfg, d->oid);
        if (dev == NULL || !model_is_stepper(d))
            continue;

        cJSON *req = link_new_request("set_motion", d->oid);
        handshake_motion_fields(dev, req);
        char *fields = cJSON_PrintUnformatted(req);
        log_info("handshake: %s settings %s", d->oid, fields != NULL ? fields : "?");
        cJSON_free(fields);
        send(h, req, on_set_motion);
        return;
    }
    log_info("handshake: settings_done");
    send(h, link_new_request("settings_done", NULL), on_settings_done);
}

static void on_set_link(void *ctx, const cJSON *reply)
{
    handshake_t *h = ctx;
    if (reply == NULL)
        return;
    uint32_t timeout = (uint32_t)h->cfg->timeout_ms;

    if (!acked(reply)) {
        fail(h, "set_link: %s", error_code(reply));
        return;
    }
    // Ping when idle for a quarter of the firmware's timeout (F-R22)
    if (timeout > 0)
        link_set_keepalive(h->link, timeout / 4 > 0 ? timeout / 4 : 1);
    send_next_motion(h);
}

static void on_set_motion(void *ctx, const cJSON *reply)
{
    handshake_t *h = ctx;
    if (reply == NULL)
        return;
    const char *oid = h->model->devices[h->next].oid;

    if (acked(reply)) {
        h->busy_since = 0;
        h->next++;
        send_next_motion(h);
        return;
    }

    const char *err = error_code(reply);
    uint64_t now = monotonic_ms();
    // Still moving, e.g. finishing its last full step after an earlier
    // session's link loss: wait for it
    if (strcmp(err, "busy") == 0) {
        if (h->busy_since == 0)
            h->busy_since = now;
        if (now - h->busy_since < HANDSHAKE_BUSY_MAX_MS) {
            h->retry_at = now + HANDSHAKE_BUSY_RETRY_MS;
            return;
        }
        fail(h, "[%s]: still moving after %d ms", oid, HANDSHAKE_BUSY_MAX_MS);
        return;
    }
    if (strcmp(err, "bad_param") == 0)
        fail(h, "[%s]: the MCU rejected the settings: bad_param (no move could "
             "run with them, e.g. pulse_width_us too long for speed x microsteps)", oid);
    else
        fail(h, "[%s]: the MCU rejected the settings: %s", oid, err);
}

// get_state for the next stepper, from h->next on; ready once there is none
// left
static void send_next_get_state(handshake_t *h)
{
    model_t *model = h->model;

    for (; h->next < model->count; h->next++) {
        const model_device_t *d = &model->devices[h->next];
        if (model_is_stepper(d)) {
            send(h, link_new_request("get_state", d->oid), on_get_state);
            return;
        }
    }
    h->status = HANDSHAKE_READY;
    log_info("handshake: done, %zu object(s)", model->count);
}

static void on_settings_done(void *ctx, const cJSON *reply)
{
    handshake_t *h = ctx;
    if (reply == NULL)
        return;

    if (!acked(reply)) {
        fail(h, "settings_done: %s", error_code(reply));
        return;
    }
    h->next = 0;
    send_next_get_state(h);
}

static void on_get_state(void *ctx, const cJSON *reply)
{
    handshake_t *h = ctx;
    if (reply == NULL)
        return;
    model_device_t *d = &h->model->devices[h->next];

    if (!acked(reply)) {
        fail(h, "%s: get_state: %s", d->oid, error_code(reply));
        return;
    }
    model_set_state(d, reply);
    log_info("handshake: %s pos %ld, pos_valid %s, moving %s, enabled %s", d->oid,
             (long)d->pos, d->pos_valid ? "true" : "false",
             d->moving ? "true" : "false", d->locked ? "true" : "false");
    h->next++;
    send_next_get_state(h);
}

int handshake_poll_timeout(const handshake_t *h)
{
    if (h->status != HANDSHAKE_RUNNING || h->retry_at == 0)
        return -1;
    uint64_t now = monotonic_ms();
    return h->retry_at <= now ? 0 : (int)(h->retry_at - now);
}

void handshake_tick(handshake_t *h)
{
    if (h->status != HANDSHAKE_RUNNING || h->retry_at == 0 || monotonic_ms() < h->retry_at)
        return;
    h->retry_at = 0;
    send_next_motion(h);
}
