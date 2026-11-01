#ifndef DUMMY_STEPGEN_H
#define DUMMY_STEPGEN_H

// Dummy step generator for host tests: records what the stepper module
// starts, instead of driving a PIO state machine (src/firmware/stepgen.h)

#include <stdbool.h>
#include <stdint.h>
#include "planner.h"
#include "stepgen.h"

#define DUMMY_CLOCK_HZ 125000000u

typedef struct {
    uint32_t  high;           // last stepgen_set_high()
    bool      dir;            // DIR level
    planner_t plan;           // last stepgen_start()
    int       starts;         // stepgen_start() calls
    bool      busy;           // set by a start; tests clear it to "finish"
    uint32_t  underruns;      // returned by stepgen_underruns(); tests set it
    uint32_t  emitted;        // returned by stepgen_stop(); tests set it
    int       stops;          // stepgen_stop() calls on a busy axis
} dummy_axis_t;

extern dummy_axis_t dummy_axes[];

// Forget everything recorded; every axis idle
void dummy_stepgen_reset(void);

// Run a copy of the axis's last plan to the end; returns its step count
uint64_t dummy_plan_steps(size_t axis);

#endif
