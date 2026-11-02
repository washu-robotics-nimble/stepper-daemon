// Host unit tests for nimhub-cli's line parser (src/cli/parse.c)

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "parse.h"

// Parse line as a request of this kind and compare it, printed, with expect
static void expect_kind_request(const char *line, cli_kind_t kind, const char *expect)
{
    cJSON *req;
    char err[128];

    assert(cli_parse(line, &req, err, sizeof err) == kind);
    char *got = cJSON_PrintUnformatted(req);
    if (strcmp(got, expect) != 0) {
        fprintf(stderr, "%s\n  got      %s\n  expected %s\n", line, got, expect);
        assert(0);
    }
    cJSON_free(got);
    cJSON_Delete(req);
}

static void expect_request(const char *line, const char *expect)
{
    expect_kind_request(line, CLI_REQUEST, expect);
}

// Whether the line's request is a write command (needs the lease)
static bool is_write(const char *line)
{
    cJSON *req;
    char err[128];

    cli_kind_t kind = cli_parse(line, &req, err, sizeof err);
    assert(kind == CLI_REQUEST || kind == CLI_RAW);
    bool w = cli_is_write(req);
    cJSON_Delete(req);
    return w;
}

static void expect_kind(const char *line, cli_kind_t kind)
{
    cJSON *req;
    char err[128];

    if (cli_parse(line, &req, err, sizeof err) != kind) {
        fprintf(stderr, "%s: wrong kind\n", line);
        assert(0);
    }
    assert(req == NULL);
}

int main(void)
{
    // Device commands, with the argument in its field (D-R14)
    expect_request("m0 rel 400", "{\"dev\":\"m0\",\"cmd\":\"rel\",\"steps\":400}");
    expect_request("m0 rel -100", "{\"dev\":\"m0\",\"cmd\":\"rel\",\"steps\":-100}");
    expect_request("  m0   abs  7  ", "{\"dev\":\"m0\",\"cmd\":\"abs\",\"pos\":7}");
    expect_request("m0 speed 300", "{\"dev\":\"m0\",\"cmd\":\"speed\",\"value\":300}");
    expect_request("m0 micro 16", "{\"dev\":\"m0\",\"cmd\":\"micro\",\"value\":16}");
    expect_request("m0 stop", "{\"dev\":\"m0\",\"cmd\":\"stop\"}");
    expect_request("m0 status", "{\"dev\":\"m0\",\"cmd\":\"status\"}");
    expect_request("m0\tpos", "{\"dev\":\"m0\",\"cmd\":\"pos\"}");
    // Anything else goes out as typed; nimhubd checks it
    expect_request("m0 fly 3", "{\"dev\":\"m0\",\"cmd\":\"fly\",\"value\":3}");
    expect_request("m0 rel x", "{\"dev\":\"m0\",\"cmd\":\"rel\",\"steps\":\"x\"}");
    expect_request("m0 rel 1.5", "{\"dev\":\"m0\",\"cmd\":\"rel\",\"steps\":1.5}");
    expect_request("m0 rel nan", "{\"dev\":\"m0\",\"cmd\":\"rel\",\"steps\":\"nan\"}");
    expect_request("m0 rel 12abc", "{\"dev\":\"m0\",\"cmd\":\"rel\",\"steps\":\"12abc\"}");
    // Daemon commands
    expect_request("estop", "{\"cmd\":\"estop\"}");
    expect_request("devices", "{\"cmd\":\"devices\"}");
    expect_request("frobnicate", "{\"cmd\":\"frobnicate\"}");
    // Raw JSON, as it is
    expect_kind_request("{\"seq\":9,\"cmd\":\"estop\"}", CLI_RAW, "{\"seq\":9,\"cmd\":\"estop\"}");
    expect_kind_request("  {\"cmd\":\"devices\"}", CLI_RAW, "{\"cmd\":\"devices\"}");

    // Write commands (D-R13): device commands that change something
    static const char *const writes[] = {
        "m0 rel 1", "m0 abs 1", "m0 stop", "m0 speed 1", "m0 accel 1", "m0 micro 4",
        "m0 zero", "m0 home", "m0 lock", "m0 unlock",
    };
    for (size_t i = 0; i < sizeof writes / sizeof writes[0]; i++)
        assert(is_write(writes[i]));
    static const char *const others[] = {
        "m0 pos", "m0 status", "m0 fly", "estop", "devices", "lease", "release", "rel",
    };
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
        assert(!is_write(others[i]));
    // Raw JSON is classified too; the CLI just doesn't wrap it in a lease
    assert(is_write("{\"dev\":\"m0\",\"cmd\":\"rel\",\"steps\":1}"));

    // Local
    expect_kind("", CLI_EMPTY);
    expect_kind("   \t ", CLI_EMPTY);
    expect_kind("help", CLI_HELP);
    expect_kind("?", CLI_HELP);
    expect_kind("quit", CLI_QUIT);
    expect_kind(" exit ", CLI_QUIT);

    // Errors
    expect_kind("m0 rel 1 2", CLI_ERROR);
    expect_kind("a b c d e", CLI_ERROR);
    expect_kind("{not json", CLI_ERROR);
    expect_kind("{\"cmd\":\"x\"} trailing", CLI_ERROR);

    printf("test_cli_parse: ok\n");
    return 0;
}
