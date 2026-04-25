#include "config.h"
#include <string.h> // So I can use memset without compiler yelling at me
#include "../lib/inih/ini.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int handler(void* user, const char* section, const char* name,
                   const char* value)
{
    motor_config_t* cfg = (motor_config_t*)user;

    #define MATCH(s, n) (strcmp(section, s) == 0 && strcmp(name, n) == 0)

    if (MATCH("driver", "chip")) {
        cfg->chip = atoi(value);
    } else if (MATCH("driver", "step_pin")) {
        cfg->step_pin = atoi(value);
    } else if (MATCH("driver", "dir_pin")) {
        cfg->dir_pin = atoi(value);
    } else if (MATCH("driver", "ms1_pin")) {
        cfg->ms1_pin = atoi(value);
    } else if (MATCH("driver", "ms2_pin")) {
        cfg->ms2_pin = atoi(value);
    } else if (MATCH("driver", "ms3_pin")) {
        cfg->ms3_pin = atoi(value);
    } else if (MATCH("driver", "sleep_pin")) {
        cfg->sleep_pin = atoi(value);
    } else if (MATCH("driver", "enable_pin")) {
        cfg->enable_pin = atoi(value);
    } else if (MATCH("driver", "reset_pin")) {
        cfg->reset_pin = atoi(value);
    } else if (MATCH("driver", "feedback_pin")) {
        cfg->feedback_pin = atoi(value);
    } else if (MATCH("motion", "microsteps")) {
        cfg->microsteps = atoi(value);
    } else if (MATCH("motion", "default_speed")) {
        cfg->default_speed = atoi(value);
    } else if (MATCH("motion", "pulse_width_us")) {
        cfg->pulse_width_us = atoi(value);
    } else if (MATCH("motion", "dir_polarity")) {
        cfg->dir_polarity = value[0];  // 'H' or 'L'
    } else if (MATCH("limits", "soft_limit_min")) {
        cfg->soft_limit_min = atoi(value);
    } else if (MATCH("limits", "soft_limit_max")) {
        cfg->soft_limit_max = atoi(value);
    } else {
        return 0;  // unknown
    }
    return 1;
}

int config_load(const char *filename, motor_config_t *cfg)
{
    // Default values:
    memset(cfg, 0, sizeof(*cfg));
    cfg->chip = 0;
    cfg->step_pin = 21;
    cfg->dir_pin = 20;
    cfg->ms1_pin = 16;
    cfg->ms2_pin = 19;
    cfg->ms3_pin = 13;
    cfg->sleep_pin = 12;
    cfg->enable_pin = 6;
    cfg->reset_pin = 5;
    cfg->feedback_pin = -1;
    cfg->microsteps = 16;
    cfg->default_speed = 200;
    cfg->pulse_width_us = 10;
    cfg->dir_polarity = 'H';

    if (ini_parse(filename, handler, cfg) < 0) {
        fprintf(stderr, "Failed to load config file '%s'\n", filename);
        return -1;
    }
    // Validity checks:
    if (cfg->microsteps != 1 && cfg->microsteps != 2 &&
        cfg->microsteps != 4 && cfg->microsteps != 8 &&
        cfg->microsteps != 16) {
        fprintf(stderr, "Config: Invalid microsteps: %d\n", cfg->microsteps);
        return -1;
    }
    if (cfg->default_speed <= 0) {
        fprintf(stderr, "Config: Invalid default_speed\n");
        return -1;
    }
    if (cfg->pulse_width_us < 2) {
        fprintf(stderr, "Config: Pulse width too small\n");
        return -1;
    }
    return 0;
}