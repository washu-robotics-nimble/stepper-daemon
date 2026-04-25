#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int chip;                               // Chipline number. Typically 0.
    int step_pin, dir_pin;                  // STEP and DIR pins.
    int ms1_pin, ms2_pin, ms3_pin;          // Microstep selection pins.
    int sleep_pin, enable_pin, reset_pin;   // Active low.
    int feedback_pin;                       // STEP loopback. -1 for no loopback.

    int microsteps;                         // 1, 2, 4, 8, 16 microsteps per full step.
    int default_speed;                      // Full steps / second.
    int pulse_width_us;                     // STEP HIGH width (us).

    char dir_polarity;                      // 'H': Forward=HIGH; 'L': Forward=LOW.

    // Not implemented yet:
    int soft_limit_min;                     // Full steps. 0 for unlimited.
    int soft_limit_max;
} motor_config_t;

int config_load(const char *filename, motor_config_t *cfg);

#endif