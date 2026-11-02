#include "model.h"

#include <math.h>
#include "log.h"

void model_reset(model_t *m)
{
    m->count = 0;
    memset(m->devices, 0, sizeof m->devices);
}

model_device_t *model_add(model_t *m, const char *oid, const char *type)
{
    if (m->count == MODEL_MAX_DEVICES
        || strlen(oid) >= sizeof m->devices[0].oid
        || strlen(type) >= sizeof m->devices[0].type)
        return NULL;
    model_device_t *d = &m->devices[m->count++];
    memset(d, 0, sizeof *d);
    strcpy(d->oid, oid);
    strcpy(d->type, type);
    return d;
}

model_device_t *model_find(model_t *m, const char *oid)
{
    for (size_t i = 0; i < m->count; i++)
        if (strcmp(m->devices[i].oid, oid) == 0)
            return &m->devices[i];
    return NULL;
}

// An int32 field of msg into *out; false if missing or not one
static bool get_pos(const cJSON *msg, int32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(msg, "pos");
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble)
        || v->valuedouble < INT32_MIN || v->valuedouble > INT32_MAX)
        return false;
    *out = (int32_t)v->valuedouble;
    return true;
}

static void get_bool(const cJSON *msg, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(msg, key);
    if (cJSON_IsBool(v))
        *out = cJSON_IsTrue(v);
}

void model_set_state(model_device_t *d, const cJSON *reply)
{
    get_pos(reply, &d->pos);
    get_bool(reply, "pos_valid", &d->pos_valid);
    get_bool(reply, "moving", &d->moving);
    get_bool(reply, "enabled", &d->locked);
    d->known = true;
}

void model_event(model_t *m, const cJSON *evt)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(evt, "evt");
    const cJSON *oid = cJSON_GetObjectItemCaseSensitive(evt, "oid");
    if (!cJSON_IsString(name) || !cJSON_IsString(oid))
        return;
    model_device_t *d = model_find(m, oid->valuestring);
    if (d == NULL || !model_is_stepper(d))
        return;

    const char *e = name->valuestring;
    bool start = strcmp(e, "move_start") == 0;
    bool end = strcmp(e, "move_end") == 0;
    if (!start && !end && strcmp(e, "position") != 0)
        return;
    if (!get_pos(evt, &d->pos))
        log_warn("%s event without a valid pos", e);
    if (start)
        d->moving = true;
    else if (end)
        d->moving = false;
}

void model_set_setting(model_t *m, const model_device_t *d, model_setting_t s, int32_t value)
{
    model_runtime_t *r = (model_runtime_t *)model_runtime(m, d->oid);
    if (r == NULL) {
        if (m->runtime_count == MODEL_MAX_DEVICES)
            return;     // can't happen: oids come from at most that many devices
        r = &m->runtime[m->runtime_count++];
        memset(r, 0, sizeof *r);
        strcpy(r->oid, d->oid);
    }
    r->has[s] = true;
    r->value[s] = value;
}

const model_runtime_t *model_runtime(const model_t *m, const char *oid)
{
    for (size_t i = 0; i < m->runtime_count; i++)
        if (strcmp(m->runtime[i].oid, oid) == 0)
            return &m->runtime[i];
    return NULL;
}

bool model_take_change(model_device_t *d)
{
    if (!d->known)
        return false;
    if (d->sent && d->sent_pos == d->pos && d->sent_pos_valid == d->pos_valid
        && d->sent_moving == d->moving && d->sent_locked == d->locked)
        return false;
    d->sent = true;
    d->sent_pos = d->pos;
    d->sent_pos_valid = d->pos_valid;
    d->sent_moving = d->moving;
    d->sent_locked = d->locked;
    return true;
}
