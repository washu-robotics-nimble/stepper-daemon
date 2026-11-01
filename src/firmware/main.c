// Firmware entry point.
#include "pico/stdlib.h"
#include "tusb.h"
#include "board.h"
#include "hostlink.h"
#include "linebuf.h"
#include "linequeue.h"
#include "stepgen.h"
#include "stepper.h"

static linebuf_t rx;
static linequeue_t queue;

// Bytes read from USB but not yet assembled, kept while the queue is full
static uint8_t carry[64];
static uint32_t carry_len, carry_pos;

// Set from the TinyUSB callback, handled in the main loop
static volatile bool host_opened;
static volatile bool host_closed;

// DTR follows the host opening and closing the port
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    if (dtr)
        host_opened = true;
    else
        host_closed = true;
}

// Write one line to the host. Blocks while the TX buffer is full, pumping
// USB meanwhile; drops the rest if the host goes away.
static void usb_write(const char *data, size_t len)
{
    while (len > 0) {
        if (!tud_cdc_connected())
            return;
        uint32_t n = tud_cdc_write(data, len);
        data += n;
        len -= n;
        if (len > 0) {
            tud_cdc_write_flush();
            tud_task();
        }
    }
    tud_cdc_write_flush();
}

static void rx_task(void)
{
    // Discard any partial line on disconnect and on connect (F-R31).
    // Unread and queued input is dropped only on disconnect: on connect,
    // the FIFO may already hold the host's first frame.
    if (host_closed) {
        host_closed = false;
        tud_cdc_read_flush();
        carry_len = carry_pos = 0;
        linequeue_reset(&queue);
        linebuf_reset(&rx);
    }
    if (host_opened) {
        host_opened = false;
        linebuf_reset(&rx);
    }

    // Assemble everything received into lines while the queue has room. An
    // estop runs as soon as its line is complete, ahead of every line still
    // queued (F-R24).
    while (!linequeue_full(&queue)) {
        if (carry_pos == carry_len) {
            carry_len = tud_cdc_available() ? tud_cdc_read(carry, sizeof carry) : 0;
            carry_pos = 0;
            if (carry_len == 0)
                break;
        }
        switch (linebuf_put(&rx, (char)carry[carry_pos++])) {
        case LINEBUF_LINE:
            if (hostlink_is_estop(rx.buf, rx.len))
                hostlink_handle_line(rx.buf, rx.len);
            else
                linequeue_push(&queue, rx.buf, rx.len);
            break;
        case LINEBUF_TOO_LONG:
            linequeue_push_too_long(&queue);
            break;
        case LINEBUF_NONE:
            break;
        }
    }

    // One queued line per pass of the main loop, so events from
    // stepper_poll() (e.g. a stopped move's move_end) come before the next
    // command runs
    if (!linequeue_empty(&queue)) {
        size_t len;
        const char *line = linequeue_front(&queue, &len);
        if (line != NULL)
            hostlink_handle_line(line, len);
        else
            hostlink_rx_error("too_long");
        linequeue_drop(&queue);
    }
}

int main(void)
{
    // Safe driver outputs before anything else runs (F-R23)
    board_safe_init();

    stepgen_init();
    stepper_init();

    linebuf_reset(&rx);
    linequeue_reset(&queue);
    hostlink_init(usb_write);
    tusb_init();

    while (true) {
        tud_task();
        rx_task();
        stepper_poll();
    }
}
