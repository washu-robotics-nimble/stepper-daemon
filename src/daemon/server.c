#include "server.h"

#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "log.h"

int server_open(server_t *s, const char *path, const char *group,
                server_line_fn on_line, server_close_fn on_close, void *ctx)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    memset(s, 0, sizeof *s);
    s->listen_fd = -1;
    s->on_line = on_line;
    s->on_close = on_close;
    s->ctx = ctx;
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++)
        s->conns[i].fd = -1;
    if (strlen(path) >= sizeof addr.sun_path) {
        log_error("socket path too long: %s", path);
        return -1;
    }
    strcpy(addr.sun_path, path);

    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            log_error("%s exists and is not a socket", path);
            return -1;
        }
        unlink(path);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }
    // Created with mode 0660 from the start (D-R12)
    mode_t old = umask(0117);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(old);
    if (rc < 0) {
        log_error("cannot create the socket %s: %s%s", path, strerror(errno),
                  errno == ENOENT ? " (its directory is made by systemd; "
                                    "when run by hand, set socket = in [daemon])" : "");
        close(fd);
        return -1;
    }
    if (listen(fd, SERVER_MAX_CLIENTS) < 0) {
        log_error("listen: %s", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    s->listen_fd = fd;
    strcpy(s->path, path);

    struct group *g = getgrnam(group);
    if (g == NULL)
        log_warn("group '%s' doesn't exist; %s keeps its group", group, path);
    else if (chown(path, (uid_t)-1, g->gr_gid) < 0)
        log_warn("cannot give %s to group '%s': %s", path, group, strerror(errno));
    log_info("listening on %s", path);
    return 0;
}

static void drop(server_t *s, server_conn_t *c, const char *why)
{
    log_info("client %u: %s", c->id, why);
    if (s->on_close != NULL)
        s->on_close(s->ctx, (server_client_t){(int)(c - s->conns), c->gen});
    close(c->fd);
    c->fd = -1;
    c->gen++;
}

void server_close(server_t *s)
{
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++)
        if (s->conns[i].fd >= 0)
            drop(s, &s->conns[i], "closed (nimhubd stopping)");
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        unlink(s->path);
        s->listen_fd = -1;
    }
}

static void accept_client(server_t *s)
{
    int fd = accept4(s->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EINTR)
            log_warn("accept: %s", strerror(errno));
        return;
    }

    int count = 0, slot = -1;
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (s->conns[i].fd >= 0)
            count++;
        else if (slot < 0)
            slot = i;
    }
    if (slot < 0) {
        log_warn("client refused: %d clients connected already", SERVER_MAX_CLIENTS);
        close(fd);
        return;
    }
    server_conn_t *c = &s->conns[slot];
    c->fd = fd;
    c->id = ++s->next_id;
    c->pending = 0;
    c->closing = false;
    c->rx_len = 0;
    c->rx_overflow = false;
    c->tx_len = 0;
    log_info("client %u: connected (%d of %d)", c->id, count + 1, SERVER_MAX_CLIENTS);
}

// A closing client with nothing left to answer or send
static void maybe_finish(server_t *s, server_conn_t *c)
{
    if (c->fd >= 0 && c->closing && c->pending == 0 && c->tx_len == 0)
        drop(s, c, "disconnected");
}

// Write as much output as the client takes now
static void flush(server_t *s, server_conn_t *c)
{
    size_t done = 0;

    while (done < c->tx_len) {
        ssize_t n = send(c->fd, c->tx + done, c->tx_len - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            drop(s, c, "write failed, dropped");
            return;
        }
        done += (size_t)n;
    }
    memmove(c->tx, c->tx + done, c->tx_len - done);
    c->tx_len -= done;
    maybe_finish(s, c);
}

static void receive(server_t *s, int slot)
{
    server_conn_t *c = &s->conns[slot];
    uint32_t gen = c->gen;
    char buf[4096];

    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof buf, 0);
        if (n == 0) {
            // It may still read: its replies go out first (D-R41)
            c->closing = true;
            maybe_finish(s, c);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            drop(s, c, "read failed, dropped");
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                server_client_t ref = {slot, gen};
                if (c->rx_overflow) {
                    log_debug("client %u < (over-length line)", c->id);
                    s->on_line(s->ctx, ref, NULL, 0);
                } else {
                    c->rx[c->rx_len] = '\0';
                    log_debug("client %u < %s", c->id, c->rx);
                    s->on_line(s->ctx, ref, c->rx, c->rx_len);
                }
                // The callback may have dropped this client
                if (c->fd < 0 || c->gen != gen)
                    return;
                c->rx_len = 0;
                c->rx_overflow = false;
            } else if (c->rx_len < sizeof c->rx - 1) {
                c->rx[c->rx_len++] = ch;
            } else {
                c->rx_overflow = true;
            }
        }
    }
}

size_t server_poll_fill(server_t *s, struct pollfd *pfd)
{
    size_t n = 0;

    pfd[n++] = (struct pollfd){.fd = s->listen_fd, .events = POLLIN};
    s->poll_count = 0;
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        const server_conn_t *c = &s->conns[i];
        if (c->fd < 0)
            continue;
        s->poll_map[s->poll_count++] = i;
        pfd[n++] = (struct pollfd){
            .fd = c->fd,
            .events = (c->closing ? 0 : POLLIN) | (c->tx_len > 0 ? POLLOUT : 0),
        };
    }
    return n;
}

void server_poll_handle(server_t *s, const struct pollfd *pfd)
{
    for (size_t k = 0; k < s->poll_count; k++) {
        int slot = s->poll_map[k];
        server_conn_t *c = &s->conns[slot];
        short ev = pfd[1 + k].revents;
        // Skip a slot closed, or taken by a new client, since the poll
        if (c->fd != pfd[1 + k].fd || ev == 0)
            continue;
        if (ev & POLLOUT)
            flush(s, c);
        if (c->fd >= 0 && c->closing && (ev & (POLLHUP | POLLERR)))
            drop(s, c, "disconnected");     // gone for good: nothing to wait for
        else if (c->fd >= 0 && !c->closing && (ev & (POLLIN | POLLHUP | POLLERR)))
            receive(s, slot);
    }
    if (pfd[0].revents & POLLIN)
        accept_client(s);
}

static server_conn_t *find(const server_t *s, server_client_t client)
{
    if (client.index < 0 || client.index >= SERVER_MAX_CLIENTS)
        return NULL;
    const server_conn_t *c = &s->conns[client.index];
    return c->fd >= 0 && c->gen == client.gen ? (server_conn_t *)c : NULL;
}

unsigned server_client_id(const server_t *s, server_client_t client)
{
    const server_conn_t *c = find(s, client);
    return c != NULL ? c->id : 0;
}

void server_send(server_t *s, server_client_t client, const cJSON *msg)
{
    server_conn_t *c = find(s, client);
    if (c == NULL)
        return;

    char *text = cJSON_PrintUnformatted(msg);
    if (text == NULL)
        return;
    size_t len = strlen(text);
    log_debug("client %u > %s", c->id, text);
    if (len + 1 > sizeof c->tx - c->tx_len) {
        cJSON_free(text);
        drop(s, c, "not reading its replies, dropped");
        return;
    }
    memcpy(c->tx + c->tx_len, text, len);
    c->tx[c->tx_len + len] = '\n';
    c->tx_len += len + 1;
    cJSON_free(text);
    flush(s, c);
}

void server_hold(server_t *s, server_client_t client)
{
    server_conn_t *c = find(s, client);
    if (c != NULL)
        c->pending++;
}

void server_release(server_t *s, server_client_t client)
{
    server_conn_t *c = find(s, client);
    if (c != NULL && c->pending > 0) {
        c->pending--;
        maybe_finish(s, c);
    }
}
