#ifndef STEPPER_H
#define STEPPER_H

#include "hostlink.h"

// Stepper objects: motion state, object commands and move events (F-R1,
// F-R10, F-R21, F-R24, F-R32, F-R33, F-R34). Pure C; drives the hardware through
// stepgen.h and board.h.

// Command table for type "stepper" (message catalogue)
extern const hostlink_cmd_t stepper_cmds[];

// Load every axis's built-in defaults (F-R34). Call after stepgen_init().
void stepper_init(void);

// Send pending events, periodic positions, and detect finished moves.
// Call from the main loop, with a millisecond clock (wrapping is fine).
void stepper_poll(uint32_t now_ms);

// Emergency stop (F-R24): halt every axis now, even off a full step, and
// disable every driver; every axis's position becomes invalid. Each moving
// axis gets a move_end with reason "estop" from the next poll.
void stepper_estop(void);

#endif
