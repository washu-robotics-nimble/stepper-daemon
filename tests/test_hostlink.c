// Host link dispatch and replies (F-R31, F-R33, F-R35)
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "hostlink.h"

// Captures every line the dispatcher writes
static char lines[8][1100];
static int nlines;

static void capture(const char *line, size_t len)
{
    assert(nlines < 8);
    assert(len > 0 && line[len - 1] == '\n');
    assert(len < sizeof lines[0]);
    memcpy(lines[nlines], line, len);
    lines[nlines][len] = '\0';
    nlines++;
}

// Send one line; expect exactly one line back, returned parsed
static cJSON *roundtrip_n(const char *line, size_t len)
{
    nlines = 0;
    hostlink_handle_line(line, len);
    assert(nlines == 1);
    cJSON *msg = cJSON_Parse(lines[0]);
    assert(msg != NULL);
    return msg;
}

static cJSON *roundtrip(const char *line)
{
    return roundtrip_n(line, strlen(line));
}

static const char *str_field(const cJSON *msg, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(msg, key);
    assert(cJSON_IsString(v));
    return v->valuestring;
}

static double num_field(const cJSON *msg, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(msg, key);
    assert(cJSON_IsNumber(v));
    return v->valuedouble;
}

static void expect_ack(const char *line, double seq)
{
    cJSON *r = roundtrip(line);
    assert(num_field(r, "seq") == seq);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ack")));
    cJSON_Delete(r);
}

static void expect_error(const char *line, double seq, const char *err)
{
    cJSON *r = roundtrip(line);
    assert(num_field(r, "seq") == seq);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "ack")));
    assert(strcmp(str_field(r, "err"), err) == 0);
    cJSON_Delete(r);
}

static void expect_rx_error_n(const char *line, size_t len, const char *err)
{
    cJSON *r = roundtrip_n(line, len);
    assert(strcmp(str_field(r, "evt"), "rx_error") == 0);
    assert(strcmp(str_field(r, "err"), err) == 0);
    assert(cJSON_GetObjectItemCaseSensitive(r, "seq") == NULL);
    cJSON_Delete(r);
}

static void expect_rx_error(const char *line, const char *err)
{
    expect_rx_error_n(line, strlen(line), err);
}

static void test_identify(void)
{
    cJSON *r = roundtrip("{\"seq\":1,\"cmd\":\"identify\"}");
    assert(num_field(r, "seq") == 1);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ack")));
    assert(strlen(str_field(r, "fw")) > 0);
    assert(num_field(r, "proto") == HOSTLINK_PROTO);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "settings_applied")));
    cJSON_Delete(r);
}

static void test_list_objects(void)
{
    cJSON *r = roundtrip("{\"seq\":2,\"cmd\":\"list_objects\"}");
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(r, "objects");
    assert(cJSON_IsArray(objects) && cJSON_GetArraySize(objects) == 1);
    const cJSON *m0 = cJSON_GetArrayItem(objects, 0);
    assert(strcmp(str_field(m0, "oid"), "m0") == 0);
    assert(strcmp(str_field(m0, "type"), "stepper") == 0);

    const cJSON *types = cJSON_GetObjectItemCaseSensitive(r, "types");
    assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(types, "stepper")));
    cJSON_Delete(r);
}

static void test_seq_echo(void)
{
    expect_ack("{\"seq\":0,\"cmd\":\"ping\"}", 0);
    // Full uint32 range comes back exactly
    expect_ack("{\"seq\":4294967295,\"cmd\":\"ping\"}", 4294967295.0);
    // Unknown fields are ignored (F-R31)
    expect_ack("{\"seq\":3,\"cmd\":\"ping\",\"extra\":[1,2]}", 3);
    // Trailing whitespace, e.g. '\r' from a terminal
    expect_ack("{\"seq\":4,\"cmd\":\"ping\"}\r", 4);
}

static void test_command_errors(void)
{
    expect_error("{\"seq\":5,\"cmd\":\"fly\"}", 5, "unknown_cmd");
    expect_error("{\"seq\":6}", 6, "unknown_cmd");
    expect_error("{\"seq\":7,\"cmd\":42}", 7, "unknown_cmd");
    expect_error("{\"seq\":8,\"cmd\":\"move\",\"oid\":\"m9\"}", 8, "unknown_oid");
    expect_error("{\"seq\":9,\"cmd\":\"move\",\"oid\":3}", 9, "bad_param");
    // Known object, command not supported (yet) for its type
    expect_error("{\"seq\":10,\"cmd\":\"fly\",\"oid\":\"m0\"}", 10, "unknown_cmd");
}

static void test_dropped_frames(void)
{
    expect_rx_error("", "bad_json");
    expect_rx_error("{\"seq\":1,", "bad_json");
    expect_rx_error("[1,2,3]", "bad_json");
    expect_rx_error("{\"seq\":1,\"cmd\":\"ping\"} junk", "bad_json");
    expect_rx_error_n("{\"seq\":1,\"cmd\":\"ping\"}\0x", 24, "bad_json");

    expect_rx_error("{\"cmd\":\"ping\"}", "no_seq");
    expect_rx_error("{\"seq\":-1,\"cmd\":\"ping\"}", "no_seq");
    expect_rx_error("{\"seq\":1.5,\"cmd\":\"ping\"}", "no_seq");
    expect_rx_error("{\"seq\":4294967296,\"cmd\":\"ping\"}", "no_seq");
    expect_rx_error("{\"seq\":\"1\",\"cmd\":\"ping\"}", "no_seq");

    nlines = 0;
    hostlink_rx_error("too_long");
    assert(nlines == 1 && strstr(lines[0], "\"too_long\"") != NULL);
}

static void test_estop_detection(void)
{
    // Lines the receive path runs ahead of the queue (F-R24)
    const char *yes[] = {
        "{\"seq\":1,\"cmd\":\"estop\"}",
        "{ \"cmd\" : \"estop\", \"seq\" : 2 }\r",
        // Handled early, then dropped as usual (no_seq)
        "{\"cmd\":\"estop\"}",
    };
    for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++)
        assert(hostlink_is_estop(yes[i], strlen(yes[i])));

    const char *no[] = {
        "{\"seq\":1,\"cmd\":\"ping\"}",
        "{\"seq\":1,\"cmd\":\"ping\",\"note\":\"estop\"}",
        "{\"seq\":1,\"cmd\":\"estop\"",
        "{\"seq\":1,\"cmd\":\"estop\"} junk",
        "{\"seq\":1,\"cmd\":\"estops\"}",
    };
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++)
        assert(!hostlink_is_estop(no[i], strlen(no[i])));
    assert(!hostlink_is_estop("{\"seq\":1,\"cmd\":\"estop\"}\0x", 24));

    expect_ack("{\"seq\":11,\"cmd\":\"estop\"}", 11);
    expect_rx_error("{\"cmd\":\"estop\"}", "no_seq");
}

int main(void)
{
    hostlink_init(capture);
    test_identify();
    test_list_objects();
    test_seq_echo();
    test_command_errors();
    test_dropped_frames();
    test_estop_detection();
    printf("test_hostlink: ok\n");
    return 0;
}
