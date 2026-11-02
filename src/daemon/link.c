#include "link.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "log.h"

uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void go_down(link_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// The reason is kept in down_reason; the caller of the link logs it (once
// per change of state, D-R31)
static void go_down(link_t *l, const char *fmt, ...)
{
    va_list ap;

    if (!l->up)
        return;
    l->up = false;
    va_start(ap, fmt);
    vsnprintf(l->down_reason, sizeof l->down_reason, fmt, ap);
    va_end(ap);
    log_debug("link down: %s", l->down_reason);

    // No reply will come: every waiting request gets NULL instead
    while (l->pending_count > 0) {
        link_pending_t p = l->pending[--l->pending_count];
        if (p.fn != NULL)
            p.fn(p.ctx, NULL);
    }
    l->tx_len = 0;
    l->tx_partial = false;
}

void link_close(link_t *l, const char *reason)
{
    go_down(l, "%s", reason);
}

void link_init(link_t *l, int fd, uint32_t reply_timeout_ms,
               link_event_fn on_event, void *event_ctx)
{
    memset(l, 0, sizeof *l);
    l->fd = fd;
    l->up = true;
    l->reply_timeout_ms = reply_timeout_ms;
    l->on_event = on_event;
    l->event_ctx = event_ctx;
    l->last_tx = monotonic_ms();
}

void link_set_keepalive(link_t *l, uint32_t ms)
{
    l->keepalive_ms = ms;
}

// Write as much of the tx buffer as the device takes now
static void flush(link_t *l)
{
    size_t done = 0;

    while (done < l->tx_len) {
        ssize_t n = write(l->fd, l->tx + done, l->tx_len - done);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            go_down(l, "write: %s", strerror(errno));
            return;
        }
        done += (size_t)n;
    }
    // A frame cut short stays first, so an urgent one can't land inside it
    if (done > 0)
        l->tx_partial = done < l->tx_len && l->tx[done - 1] != '\n';
    memmove(l->tx, l->tx + done, l->tx_len - done);
    l->tx_len -= done;
}

cJSON *link_new_request(const char *cmd, const char *oid)
{
    cJSON *req = cJSON_CreateObject();

    cJSON_AddStringToObject(req, "cmd", cmd);
    if (oid != NULL)
        cJSON_AddStringToObject(req, "oid", oid);
    return req;
}

static bool request(link_t *l, const cJSON *req, link_reply_fn fn, void *ctx, bool urgent)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    const char *name = cJSON_IsString(cmd) ? cmd->valuestring : "?";

    if (!l->up)
        return false;
    if (l->pending_count == LINK_MAX_PENDING + (urgent ? LINK_URGENT_RESERVE : 0)) {
        log_error("%s not sent: %zu requests waiting for a reply", name, l->pending_count);
        return false;
    }

    // seq goes first: {"seq":n, then the request's own fields
    char *body = cJSON_PrintUnformatted(req);
    if (body == NULL)
        return false;
    uint32_t seq = l->seq + 1;
    char frame[LINK_FRAME_MAX + 1];
    int len = snprintf(frame, sizeof frame, "{\"seq\":%lu%s%s\n", (unsigned long)seq,
                       strcmp(body, "{}") == 0 ? "" : ",", body + 1);
    cJSON_free(body);
    if (len < 0 || len > LINK_FRAME_MAX) {
        log_error("%s not sent: frame over %d bytes", name, LINK_FRAME_MAX);
        return false;
    }
    if ((size_t)len > sizeof l->tx - l->tx_len) {
        log_error("%s not sent: the device isn't taking data", name);
        return false;
    }

    l->seq = seq;
    log_debug("> %.*s", len - 1, frame);
    // Normally last in line; an urgent frame goes first, but after a frame
    // already partly written (D-R17)
    size_t at = l->tx_len;
    if (urgent) {
        at = 0;
        if (l->tx_partial)
            at = (size_t)((char *)memchr(l->tx, '\n', l->tx_len) - l->tx) + 1;
        memmove(l->tx + at + len, l->tx + at, l->tx_len - at);
    }
    memcpy(l->tx + at, frame, (size_t)len);
    l->tx_len += (size_t)len;
    l->last_tx = monotonic_ms();

    link_pending_t *p = &l->pending[l->pending_count++];
    *p = (link_pending_t){
        .seq = seq,
        .deadline = l->last_tx + l->reply_timeout_ms,
        .fn = fn,
        .ctx = ctx,
    };
    snprintf(p->cmd, sizeof p->cmd, "%s", name);

    flush(l);
    return true;
}

bool link_request(link_t *l, const cJSON *req, link_reply_fn fn, void *ctx)
{
    return request(l, req, fn, ctx, false);
}

bool link_request_urgent(link_t *l, const cJSON *req, link_reply_fn fn, void *ctx)
{
    return request(l, req, fn, ctx, true);
}

// A reply: find its request. Replies come in request order, but a request
// dropped by the MCU (rx_error) gets none, and times out instead.
static void handle_reply(link_t *l, const cJSON *msg, double seq)
{
    for (size_t i = 0; i < l->pending_count; i++) {
        link_pending_t p = l->pending[i];
        if (p.seq != seq)
            continue;
        memmove(&l->pending[i], &l->pending[i + 1],
                (l->pending_count - i - 1) * sizeof l->pending[0]);
        l->pending_count--;

        if (p.fn != NULL) {
            p.fn(p.ctx, msg);
        } else if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "ack"))) {
            const cJSON *err = cJSON_GetObjectItemCaseSensitive(msg, "err");
            log_warn("%s (seq %lu) failed: %s", p.cmd, (unsigned long)p.seq,
                     cJSON_IsString(err) ? err->valuestring : "?");
        }
        return;
    }
    log_warn("reply with unknown seq %.0f", seq);
}

static void handle_line(link_t *l, char *line, size_t len)
{
    if (len > 0 && line[len - 1] == '\r')
        line[--len] = '\0';
    log_debug("< %s", line);

    cJSON *msg = cJSON_ParseWithOpts(line, NULL, true);
    const cJSON *evt = cJSON_GetObjectItemCaseSensitive(msg, "evt");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(msg, "seq");

    if (!cJSON_IsObject(msg))
        log_warn("unparseable line from the MCU: %s", line);
    else if (evt != NULL)
        l->on_event(l->event_ctx, msg);
    else if (cJSON_IsNumber(seq))
        handle_reply(l, msg, seq->valuedouble);
    else
        log_warn("line from the MCU is neither a reply nor an event: %s", line);
    cJSON_Delete(msg);
}

// Split what was received into lines
static void receive(link_t *l)
{
    char buf[1024];

    // A line's handler may take the link down (link_close())
    while (l->up) {
        ssize_t n = read(l->fd, buf, sizeof buf);
        if (n == 0) {
            go_down(l, "device closed");
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            go_down(l, "read: %s", strerror(errno));
            return;
        }
        for (ssize_t i = 0; i < n && l->up; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (l->rx_overflow)
                    log_warn("over-length line from the MCU dropped");
                else {
                    l->rx[l->rx_len] = '\0';
                    handle_line(l, l->rx, l->rx_len);
                }
                l->rx_len = 0;
                l->rx_overflow = false;
            } else if (l->rx_len < sizeof l->rx - 1) {
                l->rx[l->rx_len++] = c;
            } else {
                l->rx_overflow = true;
            }
        }
    }
}

short link_poll_events(const link_t *l)
{
    return POLLIN | (l->tx_len > 0 ? POLLOUT : 0);
}

int link_poll_timeout(const link_t *l)
{
    if (!l->up)
        return -1;

    uint64_t now = monotonic_ms();
    uint64_t next = UINT64_MAX;
    if (l->pending_count > 0)
        next = l->pending[0].deadline;   // the oldest is the first due
    if (l->keepalive_ms > 0 && l->last_tx + l->keepalive_ms < next)
        next = l->last_tx + l->keepalive_ms;
    if (next == UINT64_MAX)
        return -1;
    if (next <= now)
        return 0;
    return next - now > INT32_MAX ? INT32_MAX : (int)(next - now);
}

void link_handle(link_t *l, short revents)
{
    if (!l->up)
        return;
    if (revents & POLLIN)
        receive(l);
    if (l->up && (revents & POLLOUT))
        flush(l);
    // A hangup or error with nothing left to read: unplugged, or the far end
    // of a pty closed
    if (l->up && (revents & (POLLHUP | POLLERR)) && !(revents & POLLIN))
        go_down(l, "device hung up");
}

void link_tick(link_t *l)
{
    if (!l->up)
        return;

    uint64_t now = monotonic_ms();
    // D-R30: a missing reply is a link drop; no retransmission
    if (l->pending_count > 0 && now >= l->pending[0].deadline) {
        go_down(l, "no reply to %s (seq %lu) within %lu ms", l->pending[0].cmd,
                (unsigned long)l->pending[0].seq, (unsigned long)l->reply_timeout_ms);
        return;
    }
    // F-R22: keep the firmware's timeout from expiring
    if (l->keepalive_ms > 0 && now >= l->last_tx + l->keepalive_ms) {
        cJSON *ping = link_new_request("ping", NULL);
        link_request(l, ping, NULL, NULL);
        cJSON_Delete(ping);
    }
}
