// Stepper commands and events (F-R1..F-R5, F-R10, F-R21, F-R24, F-R32..F-R34)
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "dummy_board.h"
#include "dummy_stepgen.h"
#include "hostlink.h"
#include "stepper.h"

// Board default for m0: 200 full steps/s, 400 full steps/s^2, 20 us
// pulses, full step. At 125 MHz: cruise period 625000 cycles, high time
// 2500 cycles.
#define PERIOD 625000u
#define HIGH   2500u

static char lines[8][1100];
static int nlines;
static uint32_t now_ms;   // clock passed to stepper_poll()

static void capture(const char *line, size_t len)
{
    assert(nlines < 8);
    memcpy(lines[nlines], line, len);
    lines[nlines][len] = '\0';
    nlines++;
}

static cJSON *line_json(int i)
{
    cJSON *msg = cJSON_Parse(lines[i]);
    assert(msg != NULL);
    return msg;
}

static void send_line(const char *line)
{
    nlines = 0;
    hostlink_handle_line(line, strlen(line));
}

static void poll(void)
{
    nlines = 0;
    stepper_poll(now_ms);
}

static void expect_reply(const char *err)
{
    assert(nlines == 1);
    cJSON *r = line_json(0);
    const cJSON *ack = cJSON_GetObjectItemCaseSensitive(r, "ack");
    if (err == NULL) {
        assert(cJSON_IsTrue(ack));
    } else {
        assert(cJSON_IsFalse(ack));
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(r, "err")->valuestring, err) == 0);
    }
    cJSON_Delete(r);
}

static void expect_event(int i, const char *evt, double pos, const char *reason)
{
    cJSON *e = line_json(i);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(e, "evt")->valuestring, evt) == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(e, "oid")->valuestring, "m0") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(e, "pos")->valuedouble == pos);
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(e, "reason");
    if (reason == NULL)
        assert(r == NULL);
    else
        assert(strcmp(r->valuestring, reason) == 0);
    assert(cJSON_GetObjectItemCaseSensitive(e, "seq") == NULL);
    // State reports carry the F-R33 flag
    const cJSON *applied = cJSON_GetObjectItemCaseSensitive(e, "settings_applied");
    assert(cJSON_IsBool(applied) && cJSON_IsTrue(applied) == hostlink_settings_applied());
    cJSON_Delete(e);
}

// Finish the move in progress, and expect its move_end
static void finish_reason(double pos, const char *reason)
{
    dummy_axes[0].busy = false;
    poll();
    assert(nlines == 1);
    expect_event(0, "move_end", pos, reason);
}

static void finish(double pos)
{
    finish_reason(pos, "done");
}

// Field of a reply to a command just sent
static double reply_number(const char *key)
{
    assert(nlines == 1);
    cJSON *r = line_json(0);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r, key);
    assert(cJSON_IsNumber(v));
    double d = v->valuedouble;
    cJSON_Delete(r);
    return d;
}

static bool reply_bool(const char *key)
{
    assert(nlines == 1);
    cJSON *r = line_json(0);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r, key);
    assert(cJSON_IsBool(v));
    bool b = cJSON_IsTrue(v);
    cJSON_Delete(r);
    return b;
}

static void test_not_configured(void)
{
    // Motion is refused until settings_done (F-R33); the rest works
    send_line("{\"seq\":1,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":400}");
    expect_reply("not_configured");
    send_line("{\"seq\":1,\"cmd\":\"move_to\",\"oid\":\"m0\",\"pos\":400}");
    expect_reply("not_configured");
    assert(dummy_axes[0].starts == 0);
    send_line("{\"seq\":1,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(!reply_bool("settings_applied"));
    assert(!reply_bool("pos_valid"));
    send_line("{\"seq\":1,\"cmd\":\"set_motion\",\"oid\":\"m0\",\"speed\":200}");
    expect_reply(NULL);

    send_line("{\"seq\":1,\"cmd\":\"settings_done\"}");
    expect_reply(NULL);
    send_line("{\"seq\":1,\"cmd\":\"identify\"}");
    assert(reply_bool("settings_applied"));
}

static void test_move(void)
{
    send_line("{\"seq\":1,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":400}");
    expect_reply(NULL);

    // A planned move of 400 steps, cruising at 625000 cycles with 2500 high
    // (the profile itself is tested in test_planner)
    dummy_axis_t *a = &dummy_axes[0];
    assert(a->starts == 1);
    assert(a->high == HIGH);
    assert(a->dir == true);
    assert(a->plan.pulses == 400);
    assert(a->plan.cruise == PERIOD);
    assert(a->plan.high == HIGH);
    assert(dummy_plan_steps(0) == 400);

    // move_start follows the reply, from the next poll; sent once
    poll();
    assert(nlines == 1);
    expect_event(0, "move_start", 0, NULL);
    poll();
    assert(nlines == 0);

    send_line("{\"seq\":2,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":5}");
    expect_reply("busy");

    finish(400);

    // Negative move: DIR low
    send_line("{\"seq\":3,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":-400}");
    expect_reply(NULL);
    assert(a->dir == false);
    assert(a->starts == 2);
    assert(a->plan.pulses == 400);
    poll();
    expect_event(0, "move_start", 400, NULL);
    finish(0);
}

static void test_zero_move(void)
{
    // Same events as any move, both from the next poll; nothing started
    int starts = dummy_axes[0].starts;
    send_line("{\"seq\":4,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":0}");
    expect_reply(NULL);
    assert(dummy_axes[0].starts == starts);
    poll();
    assert(nlines == 2);
    expect_event(0, "move_start", 0, NULL);
    expect_event(1, "move_end", 0, "done");
    poll();
    assert(nlines == 0);
}

static void test_bad_steps(void)
{
    const char *bad[] = {
        "{\"seq\":5,\"cmd\":\"move\",\"oid\":\"m0\"}",
        "{\"seq\":5,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":1.5}",
        "{\"seq\":5,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":\"400\"}",
        "{\"seq\":5,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":2147483648}",
        "{\"seq\":5,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":-2147483649}",
    };
    int starts = dummy_axes[0].starts;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        send_line(bad[i]);
        expect_reply("bad_param");
        assert(dummy_axes[0].starts == starts);
    }
}

static void test_position_range(void)
{
    // Largest moves: the full int32 range
    send_line("{\"seq\":6,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":2147483647}");
    expect_reply(NULL);
    assert(dummy_plan_steps(0) == 2147483647u);
    poll();
    finish(2147483647.0);

    // Past INT32_MAX
    send_line("{\"seq\":7,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":1}");
    expect_reply("bad_param");

    send_line("{\"seq\":8,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":-2147483648}");
    expect_reply(NULL);
    assert(dummy_axes[0].plan.pulses == STEPGEN_MAX_REPEAT);
    assert(dummy_plan_steps(0) == STEPGEN_MAX_REPEAT);
    poll();
    finish(-1);
}

static void test_underrun(void)
{
    send_line("{\"seq\":10,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":1}");
    expect_reply(NULL);
    poll();
    expect_event(0, "move_start", -1, NULL);

    // A late step is reported once per underrun; the move goes on
    dummy_axes[0].underruns++;
    poll();
    assert(nlines == 1);
    cJSON *e = line_json(0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(e, "evt")->valuestring, "fault") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(e, "oid")->valuestring, "m0") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(e, "code")->valuestring, "step_underrun") == 0);
    cJSON_Delete(e);
    poll();
    assert(nlines == 0);

    // Two more before the move ends: one fault each poll that sees a change,
    // then move_end after it
    dummy_axes[0].underruns += 2;
    dummy_axes[0].busy = false;
    poll();
    assert(nlines == 2);
    e = line_json(0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(e, "evt")->valuestring, "fault") == 0);
    cJSON_Delete(e);
    expect_event(1, "move_end", 0, "done");
}

// Expect a stop's reply: ack with pos
static void expect_stop_reply(double pos)
{
    assert(nlines == 1);
    cJSON *r = line_json(0);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ack")));
    assert(cJSON_GetObjectItemCaseSensitive(r, "pos")->valuedouble == pos);
    cJSON_Delete(r);
}

static void test_stop(void)
{
    dummy_axis_t *a = &dummy_axes[0];

    send_line("{\"seq\":20,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_reply("not_moving");

    // Stopped after 150 of 400 steps: pos is what was emitted (F-R1)
    send_line("{\"seq\":21,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":400}");
    expect_reply(NULL);
    poll();
    a->emitted = 150;
    int stops = a->stops;
    send_line("{\"seq\":22,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_stop_reply(150);
    assert(a->stops == stops + 1);

    // Still moving until move_end is out, which follows the reply
    send_line("{\"seq\":23,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":1}");
    expect_reply("busy");
    send_line("{\"seq\":24,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_reply("not_moving");
    poll();
    assert(nlines == 1);
    expect_event(0, "move_end", 150, "stop");
    poll();
    assert(nlines == 0);

    // Negative move: emitted steps count down
    send_line("{\"seq\":25,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":-100}");
    expect_reply(NULL);
    poll();
    a->emitted = 30;
    send_line("{\"seq\":26,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_stop_reply(120);
    poll();
    expect_event(0, "move_end", 120, "stop");

    // Stopped before move_start was out: both events, in order
    send_line("{\"seq\":27,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":10}");
    expect_reply(NULL);
    a->emitted = 0;
    send_line("{\"seq\":28,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_stop_reply(120);
    poll();
    assert(nlines == 2);
    expect_event(0, "move_start", 120, NULL);
    expect_event(1, "move_end", 120, "stop");

    // A move of 0 steps can be stopped too; nothing to halt
    stops = a->stops;
    a->emitted = 99;   // the last move's count: must not be used
    send_line("{\"seq\":29,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":0}");
    expect_reply(NULL);
    send_line("{\"seq\":30,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_stop_reply(120);
    assert(a->stops == stops);
    poll();
    assert(nlines == 2);
    expect_event(1, "move_end", 120, "stop");
}

static void test_set_enable(void)
{
    send_line("{\"seq\":31,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":1}");
    expect_reply(NULL);
    assert(dummy_enabled[0]);
    send_line("{\"seq\":32,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":0}");
    expect_reply(NULL);
    assert(!dummy_enabled[0]);

    const char *bad[] = {
        "{\"seq\":33,\"cmd\":\"set_enable\",\"oid\":\"m0\"}",
        "{\"seq\":33,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":2}",
        "{\"seq\":33,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":true}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        send_line(bad[i]);
        expect_reply("bad_param");
    }
}

static void test_estop(void)
{
    dummy_axis_t *a = &dummy_axes[0];

    send_line("{\"seq\":40,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":1}");
    expect_reply(NULL);

    // Halts the move and disables the driver (F-R24)
    send_line("{\"seq\":41,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":400}");
    expect_reply(NULL);
    poll();
    a->emitted = 7;
    send_line("{\"seq\":42,\"cmd\":\"estop\"}");
    expect_reply(NULL);
    assert(!a->busy);
    assert(!dummy_enabled[0]);
    poll();
    assert(nlines == 1);
    expect_event(0, "move_end", 127, "estop");

    // Idle: only disables
    send_line("{\"seq\":43,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":1}");
    expect_reply(NULL);
    send_line("{\"seq\":44,\"cmd\":\"estop\"}");
    expect_reply(NULL);
    assert(!dummy_enabled[0]);
    poll();
    assert(nlines == 0);

    // A stop already pending keeps its reason
    send_line("{\"seq\":45,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":5}");
    expect_reply(NULL);
    poll();
    a->emitted = 2;
    send_line("{\"seq\":46,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_stop_reply(129);
    send_line("{\"seq\":47,\"cmd\":\"estop\"}");
    expect_reply(NULL);
    poll();
    assert(nlines == 1);
    expect_event(0, "move_end", 129, "stop");

    // Not latched: moves work again afterwards
    send_line("{\"seq\":48,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":1}");
    expect_reply(NULL);
    poll();
    finish(130);
}

static void test_set_motion(void)
{
    dummy_axis_t *a = &dummy_axes[0];

    // Fields apply together; microsteps drive MS1..MS3 (F-R10)
    send_line("{\"seq\":50,\"cmd\":\"set_motion\",\"oid\":\"m0\",\"microsteps\":16,"
              "\"speed\":100,\"accel\":50,\"pulse_width_us\":2,\"dir_polarity\":\"L\"}");
    expect_reply(NULL);
    assert(dummy_microsteps[0] == 16);
    send_line("{\"seq\":51,\"cmd\":\"set_position\",\"oid\":\"m0\",\"pos\":0}");
    expect_reply(NULL);
    send_line("{\"seq\":52,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":10}");
    expect_reply(NULL);
    // 1600 pulses/s, 800 pulses/s^2, 2 us; "L": DIR low for positive moves
    assert(a->plan.pulses == 160);
    assert(a->plan.cruise == DUMMY_CLOCK_HZ / 1600);
    assert(a->high == 250);
    assert(a->dir == false);

    // Rejected while moving, microsteps or not (F-R10)
    send_line("{\"seq\":53,\"cmd\":\"set_motion\",\"oid\":\"m0\",\"microsteps\":1}");
    expect_reply("busy");
    send_line("{\"seq\":53,\"cmd\":\"set_motion\",\"oid\":\"m0\",\"speed\":10}");
    expect_reply("busy");
    poll();
    finish(10);

    // Bad fields, and combinations no move could run with; none applies
    const char *bad[] = {
        "\"microsteps\":3", "\"microsteps\":32", "\"microsteps\":0",
        "\"speed\":0", "\"accel\":0", "\"pulse_width_us\":0",
        "\"speed\":-5", "\"speed\":1.5", "\"dir_polarity\":\"X\"",
        "\"dir_polarity\":1",
        // 16 x 100000 pulses/s: a 0.6 us period, under 2 us + 1 us
        "\"speed\":100000",
        // 16 x 400 pulses/s: a 156 us period, under 200 us + 1 us
        "\"speed\":400,\"pulse_width_us\":200",
        // A valid field is not applied either
        "\"microsteps\":1,\"speed\":0",
    };
    char line[200];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(line, sizeof line,
                 "{\"seq\":54,\"cmd\":\"set_motion\",\"oid\":\"m0\",%s}", bad[i]);
        send_line(line);
        expect_reply("bad_param");
    }
    assert(dummy_microsteps[0] == 16);

    // Soft limits come in step 7
    send_line("{\"seq\":55,\"cmd\":\"set_motion\",\"oid\":\"m0\",\"soft_limit_min\":0}");
    expect_reply("not_impl");

    // Back to the defaults for the tests after this
    send_line("{\"seq\":56,\"cmd\":\"set_motion\",\"oid\":\"m0\",\"microsteps\":1,"
              "\"speed\":200,\"accel\":400,\"pulse_width_us\":20,\"dir_polarity\":\"H\"}");
    expect_reply(NULL);
    assert(dummy_microsteps[0] == 1);
}

static void test_move_to_and_position(void)
{
    send_line("{\"seq\":60,\"cmd\":\"set_position\",\"oid\":\"m0\",\"pos\":1000}");
    expect_reply(NULL);
    send_line("{\"seq\":61,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(reply_number("pos") == 1000);
    assert(reply_bool("pos_valid"));

    // Disabling the driver voids it; enabling doesn't restore it
    send_line("{\"seq\":61,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":1}");
    expect_reply(NULL);
    send_line("{\"seq\":61,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(reply_bool("pos_valid") && reply_bool("enabled"));
    send_line("{\"seq\":61,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":0}");
    expect_reply(NULL);
    send_line("{\"seq\":61,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(!reply_bool("pos_valid") && reply_number("pos") == 1000);
    send_line("{\"seq\":61,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":1}");
    expect_reply(NULL);
    send_line("{\"seq\":61,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(!reply_bool("pos_valid"));
    send_line("{\"seq\":61,\"cmd\":\"set_position\",\"oid\":\"m0\",\"pos\":1000}");
    expect_reply(NULL);

    // Absolute: 1000 -> 600 is 400 steps back
    send_line("{\"seq\":62,\"cmd\":\"move_to\",\"oid\":\"m0\",\"pos\":600}");
    expect_reply(NULL);
    assert(dummy_axes[0].plan.pulses == 400);
    assert(dummy_axes[0].dir == false);
    send_line("{\"seq\":63,\"cmd\":\"set_position\",\"oid\":\"m0\",\"pos\":0}");
    expect_reply("busy");
    poll();
    expect_event(0, "move_start", 1000, NULL);
    finish(600);

    // To where it is: a move of 0 steps
    send_line("{\"seq\":64,\"cmd\":\"move_to\",\"oid\":\"m0\",\"pos\":600}");
    expect_reply(NULL);
    poll();
    assert(nlines == 2);
    expect_event(1, "move_end", 600, "done");

    // The whole int32 range away is too far for one move
    send_line("{\"seq\":65,\"cmd\":\"set_position\",\"oid\":\"m0\",\"pos\":-2147483648}");
    expect_reply(NULL);
    send_line("{\"seq\":66,\"cmd\":\"move_to\",\"oid\":\"m0\",\"pos\":2147483647}");
    expect_reply("bad_param");
    send_line("{\"seq\":67,\"cmd\":\"move_to\",\"oid\":\"m0\",\"pos\":\"1\"}");
    expect_reply("bad_param");
    send_line("{\"seq\":68,\"cmd\":\"set_position\",\"oid\":\"m0\",\"pos\":0}");
    expect_reply(NULL);
}

static void test_live_position(void)
{
    dummy_axis_t *a = &dummy_axes[0];

    send_line("{\"seq\":70,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":-400}");
    expect_reply(NULL);
    now_ms = 1000;
    poll();
    expect_event(0, "move_start", 0, NULL);

    // get_state reads the step counter
    a->emitted = 25;
    send_line("{\"seq\":71,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(reply_number("pos") == -25);
    assert(reply_bool("moving"));

    // position events every report_period (default 250 ms), not before
    now_ms = 1249;
    poll();
    assert(nlines == 0);
    now_ms = 1250;
    poll();
    assert(nlines == 1);
    expect_event(0, "position", -25, NULL);
    a->emitted = 60;
    now_ms = 1400;
    poll();
    assert(nlines == 0);
    now_ms = 1500;
    poll();
    expect_event(0, "position", -60, NULL);

    // set_link changes the period; 0 turns them off
    send_line("{\"seq\":72,\"cmd\":\"set_link\",\"report_period\":100}");
    expect_reply(NULL);
    now_ms = 1600;
    poll();
    assert(nlines == 1);
    send_line("{\"seq\":73,\"cmd\":\"set_link\",\"report_period\":0}");
    expect_reply(NULL);
    now_ms = 5000;
    poll();
    assert(nlines == 0);
    const char *bad[] = {
        "{\"seq\":74,\"cmd\":\"set_link\",\"report_period\":-1}",
        "{\"seq\":74,\"cmd\":\"set_link\",\"report_period\":1.5}",
        "{\"seq\":74,\"cmd\":\"set_link\",\"timeout_ms\":-1}",
        // All or nothing: the valid field isn't applied either
        "{\"seq\":74,\"cmd\":\"set_link\",\"report_period\":250,\"timeout_ms\":\"x\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        send_line(bad[i]);
        expect_reply("bad_param");
    }
    now_ms = 9000;
    poll();
    assert(nlines == 0);

    send_line("{\"seq\":75,\"cmd\":\"set_link\",\"report_period\":250,\"timeout_ms\":0}");
    expect_reply(NULL);
    finish(-400);
}

static void test_stop_on_full_step(void)
{
    dummy_axis_t *a = &dummy_axes[0];

    send_line("{\"seq\":80,\"cmd\":\"set_motion\",\"oid\":\"m0\",\"microsteps\":16}");
    expect_reply(NULL);
    send_line("{\"seq\":81,\"cmd\":\"set_position\",\"oid\":\"m0\",\"pos\":0}");
    expect_reply(NULL);

    // Stopped 37 microsteps in: 11 more finish the third full step
    send_line("{\"seq\":82,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":100}");
    expect_reply(NULL);
    poll();
    a->emitted = 37;
    int starts = a->starts;
    send_line("{\"seq\":83,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_stop_reply(3);
    assert(a->starts == starts + 1 && a->busy);
    assert(a->plan.pulses == 11);
    assert(a->dir == true);
    // Not over until that run is: move_end once it is
    send_line("{\"seq\":84,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_reply("not_moving");
    poll();
    assert(nlines == 0);
    a->emitted = 11;
    send_line("{\"seq\":85,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(reply_number("pos") == 3);
    finish_reason(3, "stop");

    // On a full step already: nothing more to run
    send_line("{\"seq\":86,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":-100}");
    expect_reply(NULL);
    poll();
    a->emitted = 32;
    starts = a->starts;
    send_line("{\"seq\":87,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_stop_reply(1);
    assert(a->starts == starts);
    poll();
    expect_event(0, "move_end", 1, "stop");

    // estop cuts at once, even mid-step, and voids the position
    send_line("{\"seq\":88,\"cmd\":\"set_enable\",\"oid\":\"m0\",\"value\":1}");
    expect_reply(NULL);
    send_line("{\"seq\":89,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":100}");
    expect_reply(NULL);
    poll();
    a->emitted = 40;
    starts = a->starts;
    send_line("{\"seq\":90,\"cmd\":\"estop\"}");
    expect_reply(NULL);
    assert(a->starts == starts && !a->busy);
    poll();
    expect_event(0, "move_end", 3, "estop");
    send_line("{\"seq\":91,\"cmd\":\"get_state\",\"oid\":\"m0\"}");
    assert(!reply_bool("pos_valid"));
    assert(!reply_bool("enabled"));

    // ... also while a stop is finishing its full step
    send_line("{\"seq\":92,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":100}");
    expect_reply(NULL);
    poll();
    a->emitted = 5;
    send_line("{\"seq\":93,\"cmd\":\"stop\",\"oid\":\"m0\"}");
    expect_stop_reply(4);
    a->emitted = 4;   // of the 11 to the full step
    send_line("{\"seq\":94,\"cmd\":\"estop\"}");
    expect_reply(NULL);
    assert(!a->busy);
    poll();
    expect_event(0, "move_end", 3, "stop");

    send_line("{\"seq\":95,\"cmd\":\"set_motion\",\"oid\":\"m0\",\"microsteps\":1}");
    expect_reply(NULL);
}

static void test_listed(void)
{
    send_line("{\"seq\":9,\"cmd\":\"list_objects\"}");
    cJSON *r = line_json(0);
    const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(r, "types"), "stepper");
    const char *names[] = {"set_motion", "move", "move_to", "stop", "set_enable",
                           "set_position", "get_state"};
    int n = sizeof names / sizeof names[0];
    assert(cJSON_GetArraySize(cmds) == n);
    for (int i = 0; i < n; i++)
        assert(strcmp(cJSON_GetArrayItem(cmds, i)->valuestring, names[i]) == 0);
    cJSON_Delete(r);
}

int main(void)
{
    stepgen_init();
    stepper_init();
    hostlink_init(capture);

    test_not_configured();
    test_move();
    test_zero_move();
    test_bad_steps();
    test_position_range();
    test_underrun();
    test_stop();
    test_set_enable();
    test_estop();
    test_set_motion();
    test_move_to_and_position();
    test_live_position();
    test_stop_on_full_step();
    test_listed();
    printf("test_stepper: ok\n");
    return 0;
}
