#include "stepgen.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "board.h"
#include "stepgen.pio.h"

// Axis i runs on state machine i of this PIO block
static PIO const pio = pio0;
static uint offset;

void stepgen_init(void)
{
    offset = pio_add_program(pio, &stepgen_program);

    for (size_t i = 0; i < board_stepper_count; i++) {
        uint sm = (uint)i;
        uint pin = board_steppers[i].step_pin;
        pio_sm_claim(pio, sm);

        pio_sm_config c = stepgen_program_get_default_config(offset);
        sm_config_set_sideset_pins(&c, pin);
        sm_config_set_out_shift(&c, true, false, 32);  // right, no autopull
        sm_config_set_clkdiv_int_frac(&c, 1, 0);       // full system clock
        pio_sm_init(pio, sm, offset + stepgen_offset_top, &c);

        // Drive STEP low from the PIO before it takes over the pin, so the
        // handover from board_safe_init() never glitches (F-R23)
        pio_sm_set_pins_with_mask(pio, sm, 0, 1u << pin);
        pio_sm_set_pindirs_with_mask(pio, sm, 1u << pin, 1u << pin);
        pio_gpio_init(pio, pin);

        stepgen_set_high(i, STEPGEN_HIGH_OVERHEAD);
        pio_sm_set_enabled(pio, sm, true);
    }
}

uint32_t stepgen_clock_hz(void)
{
    return clock_get_hz(clk_sys);
}

void stepgen_set_high(size_t axis, uint32_t cycles)
{
    uint sm = (uint)axis;
    bool enabled = pio->ctrl & (1u << sm);

    // Load ISR through the FIFO with the state machine stopped, so its own
    // stalled "pull" can't take the value
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_put(pio, sm, cycles - STEPGEN_HIGH_OVERHEAD);
    pio_sm_exec(pio, sm, pio_encode_pull(false, true));
    pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_osr));
    pio_sm_set_enabled(pio, sm, enabled);
}

void stepgen_set_dir(size_t axis, bool level)
{
    uint pin = board_steppers[axis].dir_pin;

    if (gpio_get_out_level(pin) == level)
        return;
    gpio_put(pin, level);
    // A4988 DIR setup time is 200 ns (F-R4)
    busy_wait_us_32(1);
}

bool stepgen_push(size_t axis, uint32_t word)
{
    uint sm = (uint)axis;

    if (pio_sm_is_tx_fifo_full(pio, sm))
        return false;
    pio_sm_put(pio, sm, word);
    return true;
}

bool stepgen_idle(size_t axis)
{
    uint sm = (uint)axis;

    // Back at "top" with nothing left to pull: the last word's final low
    // time has run, so DIR may change (F-R4 hold time)
    return pio_sm_is_tx_fifo_empty(pio, sm)
        && pio_sm_get_pc(pio, sm) == offset + stepgen_offset_top;
}
