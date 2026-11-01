// Board pin setup. The table itself is in board_table.c.
#include "board.h"
#include "hardware/gpio.h"
#include "pico/time.h"

// Set the level before switching to output, so the pin never glitches
static void output_init(uint8_t pin, bool level)
{
    gpio_init(pin);
    gpio_put(pin, level);
    gpio_set_dir(pin, GPIO_OUT);
}

void board_safe_init(void)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        const board_stepper_t *s = &board_steppers[i];

        // F-R23: STEP low, ENABLE high (driver disabled)
        output_init(s->enable_pin, 1);
        output_init(s->step_pin, 0);

        // Not set by F-R23: hold the driver asleep and in reset, DIR and
        // microstep pins low, until the host configures the axis
        output_init(s->dir_pin, 0);
        for (int m = 0; m < 3; m++)
            output_init(s->ms_pin[m], 0);
        output_init(s->sleep_pin, 0);
        output_init(s->reset_pin, 0);
    }
}

void board_set_enable(size_t i, bool on)
{
    const board_stepper_t *s = &board_steppers[i];

    if (on && !(gpio_get_out_level(s->sleep_pin) && gpio_get_out_level(s->reset_pin))) {
        gpio_put(s->reset_pin, 1);
        gpio_put(s->sleep_pin, 1);
        busy_wait_us_32(1000);   // A4988 wake-up time before stepping
    }
    gpio_put(s->enable_pin, !on);   // active low
}

void board_set_microsteps(size_t i, uint32_t microsteps)
{
    // A4988 MS1, MS2, MS3 (datasheet table 1)
    static const struct { uint32_t microsteps; bool ms[3]; } modes[] = {
        {1,  {0, 0, 0}},
        {2,  {1, 0, 0}},
        {4,  {0, 1, 0}},
        {8,  {1, 1, 0}},
        {16, {1, 1, 1}},
    };
    const board_stepper_t *s = &board_steppers[i];

    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        if (modes[m].microsteps == microsteps) {
            for (int k = 0; k < 3; k++)
                gpio_put(s->ms_pin[k], modes[m].ms[k]);
            return;
        }
    }
}
