#ifndef DUMMY_STEPGEN_H
#define DUMMY_STEPGEN_H

// Dummy step generator for host tests: records what the stepper module
// queues, instead of driving a PIO state machine (src/firmware/stepgen.h)

#include <stdbool.h>
#include <stdint.h>
#include "stepgen.h"

#define DUMMY_CLOCK_HZ 125000000u
#define DUMMY_MAX_WORDS 16

typedef struct {
    uint32_t high;            // last stepgen_set_high()
    bool     dir;             // DIR level
    uint32_t words[DUMMY_MAX_WORDS];
    int      nwords;
    bool     busy;            // set by a push; tests clear it to "finish"
} dummy_axis_t;

extern dummy_axis_t dummy_axes[];

// Forget everything recorded; every axis idle
void dummy_stepgen_reset(void);

#endif
