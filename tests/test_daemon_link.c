// Host unit tests for the daemon's host link (src/daemon/link.c): estop
// jumps the outgoing queue (D-R17)

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "cJSON.h"
#include "link.h"
#include "log.h"

static int replies;

static void on_event(void *ctx, const cJSON *evt)
{
    (void)ctx;
    (void)evt;
}

static void on_reply(void *ctx, const cJSON *reply)
{
    (void)ctx;
    (void)reply;
    replies++;
}

// A device that doesn't read: a Unix socket with a small send buffer
static void open_pair(int sv[2])
{
    int size = 4096;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof size) == 0);
    assert(fcntl(sv[0], F_SETFL, O_NONBLOCK) == 0);
}

// A request with a large padding field, so a few fill the socket
static bool big_request(link_t *l, int i)
{
    char pad[800];
    memset(pad, 'x', sizeof pad - 1);
    pad[sizeof pad - 1] = '\0';
    cJSON *req = link_new_request("set_motion", "m0");
    cJSON_AddNumberToObject(req, "i", i);
    cJSON_AddStringToObject(req, "pad", pad);
    bool ok = link_request(l, req, on_reply, NULL);
    cJSON_Delete(req);
    return ok;
}

static size_t count_newlines(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++)
        n += s[i] == '\n';
    return n;
}

static void test_estop_jumps_the_queue(void)
{
    static link_t l;
    int sv[2];
    open_pair(sv);
    link_init(&l, sv[0], 500, on_event, NULL);

    // Fill the socket, until several frames wait in the link's buffer
    int sent = 0;
    while (count_newlines(l.tx, l.tx_len) < 4 && sent < LINK_MAX_PENDING) {
        assert(big_request(&l, sent));
        sent++;
    }
    assert(count_newlines(l.tx, l.tx_len) >= 4);
    bool partial = l.tx_partial;
    // Frames entirely unsent: the urgent one goes ahead of these
    size_t unsent = count_newlines(l.tx, l.tx_len) - (partial ? 1 : 0);

    cJSON *estop = link_new_request("estop", NULL);
    assert(link_request_urgent(&l, estop, on_reply, NULL));
    cJSON_Delete(estop);

    // Read everything the device got, flushing the rest
    static char got[65536];
    size_t got_len = 0;
    while (count_newlines(got, got_len) < (size_t)sent + 1) {
        ssize_t n = read(sv[1], got + got_len, sizeof got - got_len);
        assert(n > 0);
        got_len += (size_t)n;
        link_handle(&l, POLLOUT);
    }

    // Every frame intact, in order, but estop ahead of the unsent ones
    int index = 0, estop_at = -1, next_i = 0;
    char *line = got;
    for (char *nl; (nl = memchr(line, '\n', got + got_len - line)) != NULL; line = nl + 1) {
        *nl = '\0';
        cJSON *msg = cJSON_Parse(line);
        assert(cJSON_IsObject(msg));
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(msg, "cmd");
        if (strcmp(cmd->valuestring, "estop") == 0) {
            estop_at = index;
            assert(cJSON_GetObjectItemCaseSensitive(msg, "seq")->valuedouble == sent + 1);
        } else {
            assert(cJSON_GetObjectItemCaseSensitive(msg, "i")->valuedouble == next_i++);
        }
        cJSON_Delete(msg);
        index++;
    }
    assert(index == sent + 1);
    assert(estop_at == (int)(sent - unsent));
    printf("  %d frames queued, %zu unsent%s; estop went out as frame %d\n",
           sent, unsent, partial ? " (one partly written)" : "", estop_at + 1);
    close(sv[0]);
    close(sv[1]);
}

// A frame cut short by the device keeps its place: the urgent one goes
// right after it, never inside it
static void test_estop_after_a_partial_frame(void)
{
    static link_t l;
    int sv[2];
    open_pair(sv);
    link_init(&l, sv[0], 500, on_event, NULL);

    // As after a write that stopped inside the first frame
    const char *queued = "-of-frame-1\n{\"seq\":2,\"cmd\":\"ping\"}\n";
    strcpy(l.tx, queued);
    l.tx_len = strlen(queued);
    l.tx_partial = true;
    l.seq = 2;

    cJSON *estop = link_new_request("estop", NULL);
    assert(link_request_urgent(&l, estop, on_reply, NULL));
    cJSON_Delete(estop);

    char got[256];
    ssize_t n = read(sv[1], got, sizeof got - 1);
    assert(n > 0);
    got[n] = '\0';
    assert(strcmp(got, "-of-frame-1\n{\"seq\":3,\"cmd\":\"estop\"}\n{\"seq\":2,\"cmd\":\"ping\"}\n") == 0);
    assert(!l.tx_partial && l.tx_len == 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_estop_past_a_full_table(void)
{
    static link_t l;
    int sv[2];
    open_pair(sv);
    link_init(&l, sv[0], 500, on_event, NULL);

    // Small requests: the pending table fills before the socket does
    cJSON *ping = link_new_request("ping", NULL);
    for (int i = 0; i < LINK_MAX_PENDING; i++)
        assert(link_request(&l, ping, on_reply, NULL));
    assert(!link_request(&l, ping, on_reply, NULL));
    cJSON_Delete(ping);

    cJSON *estop = link_new_request("estop", NULL);
    for (int i = 0; i < LINK_URGENT_RESERVE; i++)
        assert(link_request_urgent(&l, estop, on_reply, NULL));
    assert(!link_request_urgent(&l, estop, on_reply, NULL));
    cJSON_Delete(estop);

    // Every waiting request is answered with NULL when the link goes down
    replies = 0;
    link_close(&l, "test");
    assert(replies == LINK_MAX_PENDING + LINK_URGENT_RESERVE);
    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    log_set_quiet(true);
    test_estop_jumps_the_queue();
    test_estop_after_a_partial_frame();
    test_estop_past_a_full_table();
    printf("test_daemon_link: ok\n");
    return 0;
}
