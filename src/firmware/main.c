// Firmware entry point.
#include "pico/stdlib.h"
#include "tusb.h"
#include "board.h"
#include "hostlink.h"
#include "linebuf.h"

static linebuf_t rx;

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
    // Unread bytes are flushed only on disconnect: on connect, the FIFO may
    // already hold the host's first frame.
    if (host_closed) {
        host_closed = false;
        tud_cdc_read_flush();
        linebuf_reset(&rx);
    }
    if (host_opened) {
        host_opened = false;
        linebuf_reset(&rx);
    }

    uint8_t chunk[64];
    while (tud_cdc_available()) {
        uint32_t n = tud_cdc_read(chunk, sizeof chunk);
        for (uint32_t i = 0; i < n; i++) {
            switch (linebuf_put(&rx, (char)chunk[i])) {
            case LINEBUF_LINE:
                hostlink_handle_line(rx.buf, rx.len);
                break;
            case LINEBUF_TOO_LONG:
                hostlink_rx_error("too_long");
                break;
            case LINEBUF_NONE:
                break;
            }
        }
    }
}

int main(void)
{
    // Safe driver outputs before anything else runs (F-R23)
    board_safe_init();

    linebuf_reset(&rx);
    hostlink_init(usb_write);
    tusb_init();

    while (true) {
        tud_task();
        rx_task();
    }
}
