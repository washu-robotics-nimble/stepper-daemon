// Host unit tests for the daemon's INI config (src/daemon/config.c) and the
// set_motion fields built from it (src/daemon/handshake.c)

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "config.h"
#include "handshake.h"
#include "log.h"

static config_t cfg;

static int parse(const char *text)
{
    return config_parse_string(text, "test.ini", &cfg);
}

static int32_t value(const char *oid, const char *key)
{
    const config_device_t *dev = config_find_device(&cfg, oid);
    assert(dev != NULL);
    const config_value_t *v = config_get(dev, key);
    assert(v != NULL);
    return v->value;
}

static void test_full(void)
{
    assert(parse("[daemon]\n"
                 "device = /dev/x\n"
                 "socket = /tmp/s.sock\n"
                 "group = g\n"
                 "lease_timeout_ms = 2000\n"
                 "[link]\n"
                 "report_period = 0\n"
                 "timeout_ms = 1000   ; inline comment\n"
                 "reply_timeout_ms = 200\n"
                 "[m0]\n"
                 "microsteps = 16\n"
                 "speed = 300\n"
                 "accel = 1000\n"
                 "pulse_width_us = 10\n"
                 "dir_polarity = L\n"
                 "endstop = true\n"
                 "soft_limit_min = -5\n"
                 "soft_limit_max = 5000\n"
                 "[m1]\n") == 0);
    assert(strcmp(cfg.device, "/dev/x") == 0);
    assert(strcmp(cfg.socket, "/tmp/s.sock") == 0);
    assert(strcmp(cfg.group, "g") == 0);
    assert(cfg.lease_timeout_ms == 2000);
    assert(cfg.report_period == 0);
    assert(cfg.timeout_ms == 1000);
    assert(cfg.reply_timeout_ms == 200);
    assert(cfg.device_count == 2);
    assert(value("m0", "microsteps") == 16);
    assert(value("m0", "speed") == 300);
    assert(value("m0", "dir_polarity") == 'L');
    assert(value("m0", "endstop") == 1);
    assert(value("m0", "soft_limit_min") == -5);
    // An empty section still counts
    assert(config_find_device(&cfg, "m1") != NULL);
    assert(config_find_device(&cfg, "m1")->count == 0);
    assert(config_find_device(&cfg, "m2") == NULL);
}

static void test_defaults(void)
{
    assert(parse("[daemon]\ndevice = /dev/x\n") == 0);
    assert(strcmp(cfg.socket, "/run/nimhubd/control.sock") == 0);
    assert(strcmp(cfg.group, "nimhub") == 0);
    assert(cfg.lease_timeout_ms == 5000);
    assert(cfg.report_period == 250);
    assert(cfg.timeout_ms == 0);
    assert(cfg.reply_timeout_ms == 500);
    assert(cfg.device_count == 0);
}

static void test_errors(void)
{
    const char *base = "[daemon]\ndevice = /dev/x\n";
    static const char *const bad[] = {
        "[m0]\nmicrosteps = 3\n",
        "[m0]\nmicrosteps = 32\n",
        "[m0]\nmicrosteps = 0\n",
        "[m0]\nspeed = 0\n",
        "[m0]\nspeed = 2147483648\n",
        "[m0]\nspeed = 12x\n",
        "[m0]\nspeed = \n",
        "[m0]\naccel = 0\n",
        "[m0]\npulse_width_us = -1\n",
        "[m0]\ndir_polarity = X\n",
        "[m0]\ndir_polarity = h\n",
        "[m0]\nendstop = yes\n",
        "[m0]\nsoft_limit_min = 1.5\n",
        "[m0]\nsoft_limit_min = 10\nsoft_limit_max = 9\n",
        "[m0]\nsped = 3\n",                 // unknown key
        "[m0]\nmax_accel = 400\n",          // the old name
        "[m0]\nspeed = 1\nspeed = 2\n",     // set twice
        "[m0]\nspeed = 1\n[m0]\nspeed = 2\n",
        "[link]\ntimeout_ms = -1\n",
        "[link]\nreport_period = x\n",
        "[link]\nreply_timeout_ms = 0\n",
        "[link]\nretries = 3\n",
        "[daemon]\nport = 3\n",
        "[daemon]\ngroup = \n",
        "[daemon]\nlease_timeout_ms = 0\n",
        "[m0\nspeed = 1\n",                 // syntax
        "[aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]\n",   // oid too long
    };
    char text[512];

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(text, sizeof text, "%s%s", base, bad[i]);
        if (parse(text) != -1) {
            fprintf(stderr, "accepted: %s\n", bad[i]);
            assert(0);
        }
    }
    // device is required
    assert(parse("[link]\ntimeout_ms = 1\n") == -1);
    assert(parse("") == -1);
    // A key before any section
    assert(parse("speed = 1\n[daemon]\ndevice = /dev/x\n") == -1);

    // Every power of 2 up to 16, and the int32 range for limits
    assert(parse("[daemon]\ndevice = /dev/x\n[a]\nmicrosteps = 1\n[b]\nmicrosteps = 2\n"
                 "[c]\nmicrosteps = 4\n[d]\nmicrosteps = 8\n"
                 "[e]\nsoft_limit_min = -2147483648\nsoft_limit_max = 2147483647\n") == 0);
    assert(value("e", "soft_limit_min") == INT32_MIN);
    assert(value("e", "soft_limit_max") == INT32_MAX);
    // Limits equal are fine
    assert(parse("[daemon]\ndevice = /dev/x\n[m0]\nsoft_limit_min = 7\nsoft_limit_max = 7\n") == 0);
}

static char *motion(const char *section)
{
    char text[512];
    snprintf(text, sizeof text, "[daemon]\ndevice = /dev/x\n[m0]\n%s", section);
    assert(parse(text) == 0);

    cJSON *req = cJSON_CreateObject();
    handshake_motion_fields(config_find_device(&cfg, "m0"), req);
    char *s = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return s;
}

static void expect_motion(const char *section, const char *expect)
{
    char *s = motion(section);
    if (strcmp(s, expect) != 0) {
        fprintf(stderr, "got %s\nexpected %s\n", s, expect);
        assert(0);
    }
    cJSON_free(s);
}

static void test_motion_fields(void)
{
    // In INI order; endstop stays in the daemon (D-R44); missing soft
    // limits are sent as null (F-R25)
    expect_motion("speed = 300\ndir_polarity = L\nendstop = true\nmicrosteps = 4\n",
                  "{\"speed\":300,\"dir_polarity\":\"L\",\"microsteps\":4,"
                  "\"soft_limit_min\":null,\"soft_limit_max\":null}");
    expect_motion("soft_limit_max = 5000\n",
                  "{\"soft_limit_max\":5000,\"soft_limit_min\":null}");
    expect_motion("soft_limit_min = -1\nsoft_limit_max = 1\naccel = 9\npulse_width_us = 3\n",
                  "{\"soft_limit_min\":-1,\"soft_limit_max\":1,\"accel\":9,\"pulse_width_us\":3}");
    expect_motion("", "{\"soft_limit_min\":null,\"soft_limit_max\":null}");
}

int main(void)
{
    log_set_quiet(true);    // the error cases log on purpose
    test_full();
    test_defaults();
    test_errors();
    test_motion_fields();
    printf("test_daemon_config: ok\n");
    return 0;
}
