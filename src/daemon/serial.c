#include "serial.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "log.h"

// Find a process other than this one with dev (a resolved path) open. Only
// processes whose fd table this one may read are seen: all of them as root,
// otherwise the same user's. Returns its pid, or 0; *unseen counts the
// processes that couldn't be checked.
static pid_t find_holder(const char *dev, int *unseen)
{
    DIR *proc = opendir("/proc");
    pid_t self = getpid();
    pid_t found = 0;

    *unseen = 0;
    if (proc == NULL)
        return 0;

    struct dirent *p;
    while (found == 0 && (p = readdir(proc)) != NULL) {
        char *end;
        long pid = strtol(p->d_name, &end, 10);
        if (*end != '\0' || pid <= 0 || pid == self)
            continue;

        char path[64];
        snprintf(path, sizeof path, "/proc/%ld/fd", pid);
        DIR *fds = opendir(path);
        if (fds == NULL) {
            if (errno == EACCES)
                (*unseen)++;
            continue;       // or it has just exited
        }
        struct dirent *f;
        while ((f = readdir(fds)) != NULL) {
            char target[PATH_MAX];
            if (f->d_name[0] == '.')
                continue;
            ssize_t n = readlinkat(dirfd(fds), f->d_name, target, sizeof target - 1);
            if (n < 0)
                continue;
            target[n] = '\0';
            if (strcmp(target, dev) == 0) {
                found = (pid_t)pid;
                break;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return found;
}

// The holder's command name, for the message
static void process_name(pid_t pid, char *buf, size_t size)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/comm", (long)pid);

    snprintf(buf, size, "?");
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    if (fgets(buf, (int)size, f) != NULL)
        buf[strcspn(buf, "\n")] = '\0';
    fclose(f);
}

int serial_open(const char *path, bool *held, char *err, size_t errlen)
{
    *held = false;

    // O_NONBLOCK also keeps open() from waiting for a carrier
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        *held = errno == EBUSY;
        if (*held)
            snprintf(err, errlen, "%s is in exclusive use by another process", path);
        else
            snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        snprintf(err, errlen, "%s is not a serial device: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    // A second nimhubd, or a tool that locks the port too (e.g. picocom)
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        *held = errno == EWOULDBLOCK;
        if (*held)
            snprintf(err, errlen, "%s is locked by another process", path);
        else
            snprintf(err, errlen, "cannot lock %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    // From here on, other opens fail with EBUSY. A process that had the
    // device open already, without a lock (e.g. screen), is found in /proc.
    if (ioctl(fd, TIOCEXCL) < 0) {
        snprintf(err, errlen, "cannot get exclusive use of %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    char dev[PATH_MAX];
    if (realpath(path, dev) == NULL) {
        snprintf(err, errlen, "cannot resolve %s: %s", path, strerror(errno));
        serial_close(fd);
        return -1;
    }
    int unseen;
    pid_t holder = find_holder(dev, &unseen);
    if (holder != 0) {
        char name[64];
        process_name(holder, name, sizeof name);
        snprintf(err, errlen, "%s is already open by process %ld (%s)", path, (long)holder, name);
        *held = true;
        serial_close(fd);
        return -1;
    }
    if (unseen > 0)
        log_debug("%d processes of other users not checked for %s", unseen, dev);

    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        snprintf(err, errlen, "cannot set up %s: %s", path, strerror(errno));
        serial_close(fd);
        return -1;
    }
    // Nothing is flushed: the open itself brings the link up, so what has
    // arrived since (e.g. link_recovered, F-R22) belongs to this session
    return fd;
}

void serial_close(int fd)
{
    // Not left set on a device that outlives the open (e.g. a pty)
    ioctl(fd, TIOCNXCL);
    close(fd);
}
