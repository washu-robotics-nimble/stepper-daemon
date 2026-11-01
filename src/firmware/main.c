// Firmware entry point.
#include "pico/stdlib.h"
#include "tusb.h"
#include "board.h"

// Temporary (build plan step 1): echo everything received back to the host
static void echo_task(void)
{
    uint8_t buf[64];

    if (!tud_cdc_available())
        return;

    uint32_t n = tud_cdc_read(buf, sizeof buf);
    tud_cdc_write(buf, n);
    tud_cdc_write_flush();
}

int main(void)
{
    // Safe driver outputs before anything else runs (F-R23)
    board_safe_init();

    tusb_init();

    while (true) {
        tud_task();
        echo_task();
    }
}
