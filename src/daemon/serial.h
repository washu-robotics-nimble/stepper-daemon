#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stddef.h>

// The MCU's USB CDC serial device, held exclusively (D-R1)

// Open path in raw mode, non-blocking. Returns the fd, or -1 with the
// reason in err. *held is set when the reason is another process holding
// the device: one that has it open exclusively (TIOCEXCL) or locked
// (flock), or any other process this one can see with the device open.
// Afterwards, no other open succeeds, except by root.
int serial_open(const char *path, bool *held, char *err, size_t errlen);

void serial_close(int fd);

#endif
