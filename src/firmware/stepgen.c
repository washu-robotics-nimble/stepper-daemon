#include "stepgen.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "board.h"
#include "stepgen.pio.h"

// Axis i runs on state machine i of this PIO block
static PIO const pio = pio0;
static uint offset;

// Words per DMA buffer. Two per axis: while DMA streams one into the FIFO,
// core 1 refills the other, so core 1 may fall behind by up to a chunk of
// steps before the state machine runs dry.
#define CHUNK 64

typedef enum { BUF_EMPTY, BUF_READY, BUF_SENDING } buf_state_t;

typedef struct {
    // Shared between the cores
    planner_t plan;               // written by core 0 while !active
    volatile bool active;         // set by core 0 to start a move; cleared by
                                  // core 1 once all of it is in the FIFO
    volatile uint32_t underruns;  // counted by core 1

    // Core 1 only, main loop and DMA interrupt
    uint dma;
    bool running;                 // core 1 has taken the move
    uint32_t buf[2][CHUNK];
    uint32_t len[2];
    volatile buf_state_t state[2];
    uint8_t fill_idx;             // next buffer to fill (main loop)
    volatile uint8_t send_idx;    // next buffer to send
    volatile uint8_t sending;     // buffer DMA is reading
} feed_t;

static feed_t feeds[BOARD_MAX_STEPPERS];

static inline uint32_t txstall_mask(size_t i)
{
    return 1u << (PIO_FDEBUG_TXSTALL_LSB + i);
}

// ---- Core 1 -----

// Start DMA on the next buffer, if it is ready. Called from the DMA
// interrupt, or with interrupts disabled.
static bool __not_in_flash_func(send_next)(size_t i)
{
    feed_t *f = &feeds[i];
    uint8_t b = f->send_idx;

    if (f->state[b] != BUF_READY)
        return false;
    f->state[b] = BUF_SENDING;
    f->sending = b;
    f->send_idx = b ^ 1;
    dma_channel_transfer_from_buffer_now(f->dma, f->buf[b], f->len[b]);
    return true;
}

// A stall seen when DMA resumes mid-move means the state machine ran out of
// words: that step came out late (F-R3). The flag is sticky, so a stall at
// any time since the last resume is caught.
static void __not_in_flash_func(check_stall)(size_t i)
{
    if (pio->fdebug & txstall_mask(i)) {
        pio->fdebug = txstall_mask(i);
        feeds[i].underruns++;
    }
}

static void __not_in_flash_func(dma_irq)(void)
{
    for (size_t i = 0; i < board_stepper_count; i++) {
        feed_t *f = &feeds[i];
        uint32_t mask = 1u << f->dma;

        if (!(dma_hw->ints1 & mask))
            continue;
        dma_hw->ints1 = mask;
        f->state[f->sending] = BUF_EMPTY;
        // If the next buffer isn't ready, the main loop sends it
        if (send_next(i))
            check_stall(i);
    }
}

static void fill(feed_t *f)
{
    while (f->state[f->fill_idx] == BUF_EMPTY && !planner_done(&f->plan)) {
        uint8_t b = f->fill_idx;
        f->len[b] = planner_fill(&f->plan, f->buf[b], CHUNK);
        if (f->len[b] == 0)
            break;
        f->state[b] = BUF_READY;
        f->fill_idx = b ^ 1;
    }
}

static void begin(size_t i)
{
    feed_t *f = &feeds[i];

    __dmb();   // read the plan only after seeing active
    f->running = true;
    f->state[0] = f->state[1] = BUF_EMPTY;
    f->fill_idx = f->send_idx = 0;
    fill(f);

    // The state machine was stalled at "top", waiting for this move. Once
    // the FIFO has filled (or the move is all in it), that stall is over;
    // forget it, so check_stall() only sees stalls inside the move. With
    // interrupts off until then, so the DMA interrupt can't check first.
    uint32_t irqs = save_and_disable_interrupts();
    send_next(i);
    while (dma_channel_is_busy(f->dma) && !pio_sm_is_tx_fifo_full(pio, (uint)i))
        tight_loop_contents();
    pio->fdebug = txstall_mask(i);
    restore_interrupts(irqs);
}

static void feed(size_t i)
{
    feed_t *f = &feeds[i];

    if (!f->running) {
        if (f->active)
            begin(i);
        return;
    }

    fill(f);

    uint32_t irqs = save_and_disable_interrupts();
    bool dma_idle = !dma_channel_is_busy(f->dma) && f->state[f->sending] != BUF_SENDING;
    if (dma_idle && send_next(i)) {
        // DMA had stopped for want of a buffer
        check_stall(i);
    } else if (dma_idle && planner_done(&f->plan)) {
        // All words are in the FIFO
        f->running = false;
        f->active = false;
    }
    restore_interrupts(irqs);
}

static void core1_main(void)
{
    // Handled on this core: the handler is installed and enabled here
    irq_set_exclusive_handler(DMA_IRQ_1, dma_irq);
    irq_set_enabled(DMA_IRQ_1, true);

    while (true)
        for (size_t i = 0; i < board_stepper_count; i++)
            feed(i);
}

// ---- Core 0 -----

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
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX); // 8 words deep
        sm_config_set_clkdiv_int_frac(&c, 1, 0);       // full system clock
        pio_sm_init(pio, sm, offset + stepgen_offset_top, &c);

        // Drive STEP low from the PIO before it takes over the pin, so the
        // handover from board_safe_init() never glitches (F-R23)
        pio_sm_set_pins_with_mask(pio, sm, 0, 1u << pin);
        pio_sm_set_pindirs_with_mask(pio, sm, 1u << pin, 1u << pin);
        pio_gpio_init(pio, pin);

        stepgen_set_high(i, STEPGEN_HIGH_OVERHEAD);
        pio_sm_set_enabled(pio, sm, true);

        // One DMA channel per axis, paced by the state machine's TX FIFO
        feed_t *f = &feeds[i];
        f->dma = (uint)dma_claim_unused_channel(true);
        dma_channel_config d = dma_channel_get_default_config(f->dma);
        channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
        channel_config_set_read_increment(&d, true);
        channel_config_set_write_increment(&d, false);
        channel_config_set_dreq(&d, pio_get_dreq(pio, sm, true));
        dma_channel_configure(f->dma, &d, &pio->txf[sm], NULL, 0, false);
        dma_channel_set_irq1_enabled(f->dma, true);
    }

    multicore_launch_core1(core1_main);
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

void stepgen_start(size_t axis, const planner_t *plan)
{
    feed_t *f = &feeds[axis];

    f->plan = *plan;
    __dmb();   // the plan is in place before core 1 sees active
    f->active = true;
}

bool stepgen_idle(size_t axis)
{
    uint sm = (uint)axis;

    // Core 1 is done, and the state machine is back at "top" with nothing
    // left to pull: the last word's final low time has run, so DIR may
    // change (F-R4 hold time)
    return !feeds[axis].active
        && pio_sm_is_tx_fifo_empty(pio, sm)
        && pio_sm_get_pc(pio, sm) == offset + stepgen_offset_top;
}

uint32_t stepgen_underruns(size_t axis)
{
    return feeds[axis].underruns;
}
