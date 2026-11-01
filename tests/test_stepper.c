// Stepper move command and events (F-R1, F-R2, F-R3, F-R4, F-R5, F-R32)
#include <assert.h>
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
    stepper_poll();
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
    cJSON_Delete(e);
}

// Finish the move in progress, and expect its move_end
static void finish(double pos)
{
    dummy_axes[0].busy = false;
    poll();
    assert(nlines == 1);
    expect_event(0, "move_end", pos, "done");
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

static void test_listed(void)
{
    send_line("{\"seq\":9,\"cmd\":\"list_objects\"}");
    cJSON *r = line_json(0);
    const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(r, "types"), "stepper");
    const char *names[] = {"move", "stop", "set_enable"};
    assert(cJSON_GetArraySize(cmds) == 3);
    for (int i = 0; i < 3; i++)
        assert(strcmp(cJSON_GetArrayItem(cmds, i)->valuestring, names[i]) == 0);
    cJSON_Delete(r);
}

int main(void)
{
    stepgen_init();
    stepper_init();
    hostlink_init(capture);

    test_move();
    test_zero_move();
    test_bad_steps();
    test_position_range();
    test_underrun();
    test_stop();
    test_set_enable();
    test_estop();
    test_listed();
    printf("test_stepper: ok\n");
    return 0;
}
