#ifndef SERIAL_H
#define SERIAL_H

// The MCU's USB CDC serial device, held exclusively (D-R1)

// Open path in raw mode, non-blocking. Fails (logged, -1) if another process holds the device: one
// that has it open exclusively (TIOCEXCL) or locked (flock), or any other
// process this one can see with the device open. Afterwards, no other open
// succeeds, except by root.
int serial_open(const char *path);

void serial_close(int fd);

#endif
