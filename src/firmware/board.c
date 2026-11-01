// Board pin setup. The table itself is in board_table.c.
#include "board.h"
#include "hardware/gpio.h"

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
