// Stepper move command and events (F-R1, F-R2, F-R4, F-R32)
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "dummy_stepgen.h"
#include "hostlink.h"
#include "stepper.h"

// Board default for m0: 200 full steps/s, 20 us pulses, full step.
// At 125 MHz: period 625000 cycles, high time 2500 cycles.
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
    dummy_axes[0].nwords = 0;
    poll();
    assert(nlines == 1);
    expect_event(0, "move_end", pos, "done");
}

static void test_move(void)
{
    send_line("{\"seq\":1,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":400}");
    expect_reply(NULL);

    // One repeat pair: 400 steps, each 625000 cycles with 2500 high
    dummy_axis_t *a = &dummy_axes[0];
    assert(a->high == HIGH);
    assert(a->dir == true);
    assert(a->nwords == 2);
    assert(a->words[0] == stepgen_repeat_word(400));
    assert(a->words[1] == PERIOD - HIGH - STEPGEN_REPEAT_OVERHEAD);

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
    assert(a->words[0] == stepgen_repeat_word(400));
    poll();
    expect_event(0, "move_start", 400, NULL);
    finish(0);
}

static void test_zero_move(void)
{
    // Same events as any move, both from the next poll; nothing queued
    send_line("{\"seq\":4,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":0}");
    expect_reply(NULL);
    assert(dummy_axes[0].nwords == 0);
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
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        send_line(bad[i]);
        expect_reply("bad_param");
        assert(dummy_axes[0].nwords == 0);
    }
}

static void test_position_range(void)
{
    // Largest moves: the full int32 range, in one repeat word each
    send_line("{\"seq\":6,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":2147483647}");
    expect_reply(NULL);
    poll();
    finish(2147483647.0);

    // Past INT32_MAX
    send_line("{\"seq\":7,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":1}");
    expect_reply("bad_param");

    send_line("{\"seq\":8,\"cmd\":\"move\",\"oid\":\"m0\",\"steps\":-2147483648}");
    expect_reply(NULL);
    assert(dummy_axes[0].words[0] == stepgen_repeat_word(STEPGEN_MAX_REPEAT));
    poll();
    finish(-1);
}

static void test_listed(void)
{
    send_line("{\"seq\":9,\"cmd\":\"list_objects\"}");
    cJSON *r = line_json(0);
    const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(r, "types"), "stepper");
    assert(cJSON_GetArraySize(cmds) == 1);
    assert(strcmp(cJSON_GetArrayItem(cmds, 0)->valuestring, "move") == 0);
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
    test_listed();
    printf("test_stepper: ok\n");
    return 0;
}
