#ifndef MOTOR_H
#define MOTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include "../lib/lg/lgpio.h"

#include "config.h"

// Motor status struct
typedef struct {
    int handle;                     // Chip handle
    int step_pin;
    int dir_pin;
    int ms_pins[3];                 // MS1, MS2, MS3
    int sleep_pin;
    int enable_pin;
    int reset_pin;
    int feedback_pin;               // -1 for none

    int microsteps;                 // Current microstep resolution
    int speed;                      // Current speed (full steps / sec)
    int accel;                      // Max acceleration (full steps / sec^2)
                                    // 0 disables ramping
    int pulse_width_us;             // Current pulsewidth
    int dir_active_high;            // Current direction polarity

    atomic_int_least64_t position;  // Current position (full steps)
    atomic_bool position_valid;     // Validity of current position

    atomic_bool moving;             // "Moving" flag
    atomic_bool stop_requested;     // Stop request
    atomic_bool feedback_enabled;   // Loopback status
    _Atomic uint64_t feedback_cnt;  // STEP loopback rising edge count

    // Current command
    pthread_mutex_t cmd_mutex;
    pthread_cond_t cmd_cond;
    atomic_int cmd_type;            // 0=IDLE, 1=REL, 2=ABS, 3=STOP
    int32_t cmd_param;              // Steps / target position
    atomic_bool cmd_done;

    pthread_mutex_t done_mutex;     // Command done
    pthread_cond_t done_cond;      // Command done condition

    pthread_t *thread;
} motor_t;


// Error codes
#define MOTOR_OK                0
#define MOTOR_ERR_GENERAL      -1
#define MOTOR_ERR_MOVING       -2
#define MOTOR_ERR_NOT_MOVING   -3
#define MOTOR_ERR_MICROSTEP    -4
#define MOTOR_ERR_PARAM        -5
#define MOTOR_ERR_NOT_IMPL     -6
#define MOTOR_ERR_POS_INVALID  -7

// Init and closing:
int motor_init(motor_t *motor, const motor_config_t *cfg);
void motor_close(motor_t *motor);

// Motion control:
int motor_move_rel(motor_t *motor, int32_t steps);
int motor_move_abs(motor_t *motor, int32_t pos);
int motor_stop(motor_t *motor);
int motor_wait(motor_t *motor);

// Status checking:
int motor_is_moving(motor_t *motor);
int motor_get_position(motor_t *motor, int32_t *pos); // 0=valid, -7=invalid

// Config adjustment: (must not be called during motion)
int motor_set_speed(motor_t *motor, int speed);        // Full steps / sec
int motor_set_accel(motor_t *motor, int accel);        // Full steps / sec^2, 0=off
int motor_set_microstep(motor_t *motor, int ms);
int motor_set_pulse_width(motor_t *motor, int us);

// AD4988 control pins
int motor_enable(motor_t *motor);
int motor_disable(motor_t *motor);
int motor_sleep(motor_t *motor);
int motor_wake(motor_t *motor);
int motor_reset(motor_t *motor);
int motor_release_reset(motor_t *motor);

// Zeroing / locking (NOT IMPLEMENTED)
int motor_zero(motor_t *motor);
int motor_home(motor_t *motor);
int motor_lock(motor_t *motor);   // Enable output
int motor_unlock(motor_t *motor); // Disable output

#endif