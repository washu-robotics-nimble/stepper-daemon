#include "conn.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

bool conn_open(conn_t *c, const char *path)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    c->fd = -1;
    c->rx_len = 0;
    if (strlen(path) >= sizeof addr.sun_path) {
        errno = ENAMETOOLONG;
        return false;
    }
    strcpy(addr.sun_path, path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return false;
    }
    c->fd = fd;
    return true;
}

void conn_close(conn_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->rx_len = 0;
}

bool conn_send(conn_t *c, const cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    if (text == NULL)
        return false;
    size_t len = strlen(text);
    text[len] = '\n';       // over the NUL; the length is known
    size_t done = 0;
    bool ok = true;
    while (done < len + 1) {
        ssize_t n = send(c->fd, text + done, len + 1 - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        done += (size_t)n;
    }
    cJSON_free(text);
    if (!ok)
        conn_close(c);
    return ok;
}

bool conn_read(conn_t *c, conn_line_fn fn, void *ctx)
{
    ssize_t n = recv(c->fd, c->rx + c->rx_len, sizeof c->rx - 1 - c->rx_len, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return true;
    if (n <= 0) {
        conn_close(c);
        return false;
    }
    c->rx_len += (size_t)n;

    size_t start = 0;
    for (size_t i = 0; i < c->rx_len; i++) {
        if (c->rx[i] != '\n')
            continue;
        c->rx[i] = '\0';
        cJSON *msg = cJSON_Parse(c->rx + start);
        fn(ctx, c->rx + start, msg);
        cJSON_Delete(msg);
        start = i + 1;
    }
    memmove(c->rx, c->rx + start, c->rx_len - start);
    c->rx_len -= start;
    // A line longer than the buffer: drop what there is of it
    if (c->rx_len == sizeof c->rx - 1)
        c->rx_len = 0;
    return true;
}
