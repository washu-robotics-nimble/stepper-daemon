#ifndef STEPPER_H
#define STEPPER_H

#include "hostlink.h"

// Stepper objects: motion state, object commands and move events (F-R1,
// F-R32). Pure C; drives the hardware through stepgen.h.

// Command table for type "stepper" (message catalogue)
extern const hostlink_cmd_t stepper_cmds[];

// Load every axis's built-in defaults (F-R34). Call after stepgen_init().
void stepper_init(void);

// Send pending events and detect finished moves. Call from the main loop.
void stepper_poll(void);

#endif
