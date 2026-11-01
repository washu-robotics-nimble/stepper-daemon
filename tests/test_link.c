// Link loss and recovery (F-R22)
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "dummy_stepgen.h"
#include "hostlink.h"
#include "stepper.h"

static char lines[8][1100];
static int nlines;
static uint32_t now;

static void capture(const char *line, size_t len)
{
    assert(nlines < 8);
    memcpy(lines[nlines], line, len);
    lines[nlines][len] = '\0';
    nlines++;
}

static void send_line(const char *line)
{
    nlines = 0;
    hostlink_handle_line(line, strlen(line));
}

// One main loop pass at time t, as main.c runs it (no line)
static void pass(uint32_t t, bool up)
{
    nlines = 0;
    now = t;
    hostlink_tick(now);
    hostlink_set_connected(up);
    stepper_poll(now);
}

static bool line_has(int i, const char *text)
{
    return i < nlines && strstr(lines[i], text) != NULL;
}

static void expect_recovered(int i)
{
    assert(strcmp(lines[i], "{\"evt\":\"link_recovered\"}\n") == 0);
}

static void start_move(void)
{
    send_line("{\"seq\":1,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":1000}");
    assert(nlines == 1 && line_has(0, "\"ack\":true"));
    stepper_poll(now);
    dummy_axes[0].emitted = 10;
}

static void test_port_closed(void)
{
    // First open after boot: no loss before it, so nothing to report
    pass(0, true);
    assert(nlines == 0);
    send_line("{\"seq\":1,\"cmd\":\"settings_done\"}");
    // No position events in these tests
    send_line("{\"seq\":1,\"cmd\":\"set_link\",\"report_period\":0}");

    // Closed during a move: stopped like stop, ENABLE untouched (F-R22)
    send_line("{\"seq\":1,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":1}");
    send_line("{\"seq\":1,\"cmd\":\"set_position\",\"oid\":\"m0\",\"pos\":0}");
    start_move();
    int stops = dummy_axes[0].stops;
    pass(100, false);
    assert(dummy_axes[0].stops == stops + 1);
    // (sent into the void on the board)
    assert(nlines == 1 && line_has(0, "\"move_end\"") && line_has(0, "\"pos\":10")
           && line_has(0, "\"reason\":\"link_lost\""));
    send_line("{\"seq\":2,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(line_has(0, "\"enabled\":true") && line_has(0, "\"pos_valid\":true"));

    // Still closed: nothing more
    pass(5000, false);
    assert(nlines == 0);

    // Reopened: link_recovered, once
    pass(6000, true);
    assert(nlines == 1);
    expect_recovered(0);
    pass(6001, true);
    assert(nlines == 0);

    // Closed while idle: still reported when it returns
    pass(7000, false);
    assert(nlines == 0);
    pass(7001, true);
    assert(nlines == 1);
    expect_recovered(0);
}

static void test_timeout(void)
{
    // No timeout by default (infinity)
    pass(100000, true);
    assert(nlines == 0);

    send_line("{\"seq\":3,\"cmd\":\"set_link\",\"timeout_ms\":1000}");
    assert(line_has(0, "\"ack\":true"));
    start_move();
    pass(100999, true);
    assert(nlines == 0);

    // No frame for timeout_ms: lost. The link itself still works, so
    // move_end gets through.
    pass(101000, true);
    assert(nlines == 1 && line_has(0, "\"reason\":\"link_lost\""));
    pass(200000, true);
    assert(nlines == 0);

    // The next frame brings it back: link_recovered, then the reply
    send_line("{\"seq\":4,\"cmd\":\"ping\"}");
    assert(nlines == 2);
    expect_recovered(0);
    assert(line_has(1, "\"seq\":4"));

    // Frames dropped with rx_error don't keep the link alive...
    pass(200500, true);
    send_line("not json");
    assert(nlines == 1 && line_has(0, "\"rx_error\""));
    send_line("{\"cmd\":\"ping\"}");
    pass(200999, true);
    assert(nlines == 0);
    pass(201000, true);   // 1000 ms after seq 4
    assert(nlines == 0);  // lost, idle: nothing to stop

    // ...but an error reply does, and also brings the link back
    send_line("{\"seq\":5,\"cmd\":\"fly\"}");
    assert(nlines == 2);
    expect_recovered(0);
    assert(line_has(1, "\"unknown_cmd\""));
    pass(201999, true);
    assert(nlines == 0);
    send_line("{\"seq\":6,\"cmd\":\"ping\"}");   // at 201999
    pass(202998, true);
    assert(nlines == 0);

    // A timeout, then the port closed and reopened: one link_recovered
    pass(203000, true);
    pass(203500, false);
    pass(204000, true);
    assert(nlines == 1);
    expect_recovered(0);

    // The timeout starts over when the port opens, not at the last frame
    pass(204999, true);
    assert(nlines == 0);

    send_line("{\"seq\":7,\"cmd\":\"set_link\",\"timeout_ms\":0}");
    pass(900000, true);
    assert(nlines == 0);
}

static void test_stop_pending(void)
{
    // A stop already under way keeps its reason
    start_move();
    send_line("{\"seq\":8,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    pass(900001, false);
    assert(nlines == 1 && line_has(0, "\"reason\":\"stop\""));
    pass(900002, true);
    expect_recovered(0);
}

int main(void)
{
    stepgen_init();
    stepper_init();
    hostlink_init(capture);

    test_port_closed();
    test_timeout();
    test_stop_pending();
    printf("test_link: ok\n");
    return 0;
}
