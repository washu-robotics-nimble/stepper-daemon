#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ini.h"
#include "log.h"

// Device settings, by type. The ranges match the firmware's set_motion
// checks (firmware spec, message catalogue); the firmware still checks the
// combination (F-R2).
static const config_key_t device_keys[] = {
    {"microsteps",     "stepper", true,  CONFIG_POW2,     1, 16},
    {"speed",          "stepper", true,  CONFIG_INT,      1, INT32_MAX},
    {"accel",          "stepper", true,  CONFIG_INT,      1, INT32_MAX},
    {"pulse_width_us", "stepper", true,  CONFIG_INT,      1, INT32_MAX},
    {"dir_polarity",   "stepper", true,  CONFIG_POLARITY, 0, 0},
    {"soft_limit_min", "stepper", true,  CONFIG_INT,      INT32_MIN, INT32_MAX},
    {"soft_limit_max", "stepper", true,  CONFIG_INT,      INT32_MIN, INT32_MAX},
    {"endstop",        "stepper", false, CONFIG_BOOL,     0, 1},   // D-R44
};

typedef struct {
    config_t *cfg;
    const char *name;       // file name, for messages
    int errors;
    config_device_t *dev;   // section being read; NULL for daemon / link
} parse_t;

#define parse_error(p, line, ...) do {                      \
        char msg_[256];                                     \
        snprintf(msg_, sizeof msg_, __VA_ARGS__);           \
        log_error("%s:%d: %s", (p)->name, (line), msg_);    \
        (p)->errors++;                                      \
    } while (0)

static bool parse_int32(const char *s, int32_t min, int32_t max, int32_t *out)
{
    char *end;
    long long v;

    errno = 0;
    v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < min || v > max)
        return false;
    *out = (int32_t)v;
    return true;
}

static bool parse_value(const config_key_t *key, const char *s, int32_t *out)
{
    switch (key->kind) {
    case CONFIG_INT:
        return parse_int32(s, key->min, key->max, out);
    case CONFIG_POW2:
        return parse_int32(s, key->min, key->max, out) && (*out & (*out - 1)) == 0;
    case CONFIG_BOOL:
        if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0) {
            *out = s[0] == 't';
            return true;
        }
        return false;
    case CONFIG_POLARITY:
        if (strcmp(s, "H") == 0 || strcmp(s, "L") == 0) {
            *out = s[0];
            return true;
        }
        return false;
    }
    return false;
}

static bool copy_string(char *dst, size_t size, const char *s)
{
    if (strlen(s) >= size || s[0] == '\0')
        return false;
    strcpy(dst, s);
    return true;
}

static void daemon_key(parse_t *p, const char *name, const char *value, int line)
{
    config_t *cfg = p->cfg;
    bool ok;

    if (strcmp(name, "device") == 0)
        ok = copy_string(cfg->device, sizeof cfg->device, value);
    else if (strcmp(name, "socket") == 0)
        ok = copy_string(cfg->socket, sizeof cfg->socket, value);
    else if (strcmp(name, "group") == 0)
        ok = copy_string(cfg->group, sizeof cfg->group, value);
    else if (strcmp(name, "lease_timeout_ms") == 0) {
        if (!parse_int32(value, 1, INT32_MAX, &cfg->lease_timeout_ms))
            parse_error(p, line, "[daemon]: bad value '%s' for '%s'", value, name);
        return;
    } else {
        parse_error(p, line, "[daemon]: unknown key '%s'", name);
        return;
    }
    if (!ok)
        parse_error(p, line, "[daemon]: '%s' is empty or too long", name);
}

static void link_key(parse_t *p, const char *name, const char *value, int line)
{
    config_t *cfg = p->cfg;
    bool ok;

    if (strcmp(name, "report_period") == 0)
        ok = parse_int32(value, 0, INT32_MAX, &cfg->report_period);
    else if (strcmp(name, "timeout_ms") == 0)
        ok = parse_int32(value, 0, INT32_MAX, &cfg->timeout_ms);
    else if (strcmp(name, "reply_timeout_ms") == 0)
        ok = parse_int32(value, 1, INT32_MAX, &cfg->reply_timeout_ms);
    else {
        parse_error(p, line, "[link]: unknown key '%s'", name);
        return;
    }
    if (!ok)
        parse_error(p, line, "[link]: bad value '%s' for '%s'", value, name);
}

static const config_key_t *find_key(const char *name)
{
    for (size_t i = 0; i < sizeof device_keys / sizeof device_keys[0]; i++)
        if (strcmp(device_keys[i].name, name) == 0)
            return &device_keys[i];
    return NULL;
}

// Start (or, for a repeated header, continue) the section for oid
static config_device_t *open_device(parse_t *p, const char *oid, int line)
{
    config_t *cfg = p->cfg;

    for (size_t i = 0; i < cfg->device_count; i++)
        if (strcmp(cfg->devices[i].oid, oid) == 0)
            return &cfg->devices[i];

    if (strlen(oid) >= CONFIG_OID_MAX) {
        parse_error(p, line, "[%s]: section name too long", oid);
        return NULL;
    }
    if (cfg->device_count == CONFIG_MAX_DEVICES) {
        parse_error(p, line, "[%s]: more than %d device sections", oid, CONFIG_MAX_DEVICES);
        return NULL;
    }
    config_device_t *dev = &cfg->devices[cfg->device_count++];
    memset(dev, 0, sizeof *dev);
    strcpy(dev->oid, oid);
    dev->line = line;
    return dev;
}

static void device_key(parse_t *p, const char *name, const char *value, int line)
{
    config_device_t *dev = p->dev;
    const config_key_t *key = find_key(name);
    int32_t v;

    if (key == NULL) {
        parse_error(p, line, "[%s]: unknown key '%s'", dev->oid, name);
        return;
    }
    if (config_get(dev, name) != NULL) {
        parse_error(p, line, "[%s]: '%s' set twice", dev->oid, name);
        return;
    }
    if (!parse_value(key, value, &v)) {
        parse_error(p, line, "[%s]: bad value '%s' for '%s'", dev->oid, value, name);
        return;
    }
    if (dev->count == CONFIG_MAX_VALUES) {
        parse_error(p, line, "[%s]: too many keys", dev->oid);
        return;
    }
    dev->values[dev->count++] = (config_value_t){key, v};
}

// inih handler; with INI_CALL_HANDLER_ON_NEW_SECTION, a new section header
// comes with name and value NULL
static int handler(void *user, const char *section, const char *name,
                   const char *value, int line)
{
    parse_t *p = user;

    if (name == NULL) {
        p->dev = NULL;
        if (strcmp(section, "daemon") != 0 && strcmp(section, "link") != 0)
            p->dev = open_device(p, section, line);
        return 1;
    }

    if (section[0] == '\0')
        parse_error(p, line, "'%s' outside any section", name);
    else if (strcmp(section, "daemon") == 0)
        daemon_key(p, name, value, line);
    else if (strcmp(section, "link") == 0)
        link_key(p, name, value, line);
    else if (p->dev != NULL)
        device_key(p, name, value, line);
    // else: a section that couldn't be opened, already reported
    return 1;
}

static void set_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    strcpy(cfg->socket, "/run/nimhubd/control.sock");   // D-R12
    strcpy(cfg->group, "stepper");
    cfg->lease_timeout_ms = 5000;   // D-R18
    cfg->report_period = 250;       // the firmware's defaults (F-R32, F-R22)
    cfg->timeout_ms = 0;
    cfg->reply_timeout_ms = 500;    // D-R30
}

// Checks across keys, once the whole file is read
static void check(parse_t *p)
{
    const config_t *cfg = p->cfg;

    if (cfg->device[0] == '\0') {
        log_error("%s: [daemon]: 'device' is missing", p->name);
        p->errors++;
    }
    for (size_t i = 0; i < cfg->device_count; i++) {
        const config_device_t *dev = &cfg->devices[i];
        const config_value_t *min = config_get(dev, "soft_limit_min");
        const config_value_t *max = config_get(dev, "soft_limit_max");
        if (min != NULL && max != NULL && min->value > max->value) {
            log_error("%s:%d: [%s]: soft_limit_min is above soft_limit_max",
                      p->name, dev->line, dev->oid);
            p->errors++;
        }
    }
}

static int finish(parse_t *p, int rc)
{
    // rc > 0: the line of the first syntax error (the handler never fails)
    if (rc > 0)
        parse_error(p, rc, "syntax error");
    if (rc >= 0)
        check(p);
    return p->errors == 0 && rc >= 0 ? 0 : -1;
}

int config_load(const char *path, config_t *cfg)
{
    parse_t p = {.cfg = cfg, .name = path};

    set_defaults(cfg);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        log_error("cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    int rc = ini_parse_file(f, handler, &p);
    fclose(f);
    return finish(&p, rc);
}

int config_parse_string(const char *text, const char *name, config_t *cfg)
{
    parse_t p = {.cfg = cfg, .name = name};

    set_defaults(cfg);
    return finish(&p, ini_parse_string(text, handler, &p));
}

const config_device_t *config_find_device(const config_t *cfg, const char *oid)
{
    for (size_t i = 0; i < cfg->device_count; i++)
        if (strcmp(cfg->devices[i].oid, oid) == 0)
            return &cfg->devices[i];
    return NULL;
}

const config_value_t *config_get(const config_device_t *dev, const char *key)
{
    for (size_t i = 0; i < dev->count; i++)
        if (strcmp(dev->values[i].key->name, key) == 0)
            return &dev->values[i];
    return NULL;
}
