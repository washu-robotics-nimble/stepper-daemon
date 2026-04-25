#include "motor.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h> // So I can use memset without compiler yelling at me

static void* motor_thread_func(void *arg); // Internal helper function
// (NOT IMPLEMENTED:)
// static void update_position_from_feedback(motor_t *motor, int dir_positive,
//                                           int microsteps);

// Callback: Step loopback
static void step_callback(int num_alerts, lgGpioAlert_p alerts, void *userdata)
{
    motor_t *motor = (motor_t*)userdata;
    for (int i = 0; i < num_alerts; i++) {
        if (alerts[i].report.level == 1) {  // Rising edge
            atomic_fetch_add(&motor->feedback_cnt, 1);
        }
    }
}

int motor_init(motor_t *motor, const motor_config_t *cfg)
{
    memset(motor, 0, sizeof(*motor));
    motor->handle = -1;

    // Open & acquire gpiochip handle
    int h = lgGpiochipOpen(cfg->chip);
    if (h < 0) {
        fprintf(stderr, "Failed to open gpiochip%d: %s\n",
                cfg->chip, lguErrorText(h));
        return MOTOR_ERR_GENERAL;
    }
    motor->handle = h;

    // Configure motor struct from INI config:
    motor->step_pin = cfg->step_pin;
    motor->dir_pin = cfg->dir_pin;
    motor->ms_pins[0] = cfg->ms1_pin;
    motor->ms_pins[1] = cfg->ms2_pin;
    motor->ms_pins[2] = cfg->ms3_pin;
    motor->sleep_pin = cfg->sleep_pin;
    motor->enable_pin = cfg->enable_pin;
    motor->reset_pin = cfg->reset_pin;
    motor->feedback_pin = cfg->feedback_pin;

    motor->microsteps = cfg->microsteps;
    motor->speed = cfg->default_speed;
    motor->pulse_width_us = cfg->pulse_width_us;

    // Claiming pins:
    int err;

    // STEP OUT, Initial LOW
    err = lgGpioClaimOutput(h, 0, motor->step_pin, 0);
    if (err < 0) goto fail_pin;

    // DIR OUT
    int dir_init = (cfg->dir_polarity == 'H') ? 1 : 0; // Assume forward=HIGH
                                                       // Adjust it when moving
    motor->dir_active_high = (cfg->dir_polarity == 'H') ? 1 : 0;
    err = lgGpioClaimOutput(h, 0, motor->dir_pin, dir_init);
    if (err < 0) goto fail_pin;

    // SLEEP OUT, initial HIGH (wake it up)
    err = lgGpioClaimOutput(h, 0, motor->sleep_pin, 1);
    if (err < 0) goto fail_pin;

    // ENABLE OUT, initial LOW (enable output)
    err = lgGpioClaimOutput(h, 0, motor->enable_pin, 0);
    if (err < 0) goto fail_pin;

    // RESET OUT, initial HIGH (release reset)
    err = lgGpioClaimOutput(h, 0, motor->reset_pin, 1);
    if (err < 0) goto fail_pin;

    // MS1-3 OUT, adjust according to microsteps
    err = motor_set_microstep(motor, cfg->microsteps);
    if (err < 0) goto fail_pin;

    // Loopback pin
    if (cfg->feedback_pin >= 0) {
        motor->feedback_enabled = true;
        err = lgGpioClaimAlert(h, 0, LG_RISING_EDGE, cfg->feedback_pin, -1);
        if (err < 0) {
            fprintf(stderr, "Failed to claim feedback pin: %s\n", lguErrorText(err));
            return MOTOR_ERR_GENERAL;
        }
        err = lgGpioSetAlertsFunc(h, cfg->feedback_pin, step_callback, motor);
        if (err < 0) {
            fprintf(stderr, "Failed to set alerts func: %s\n", lguErrorText(err));
            return MOTOR_ERR_GENERAL;
        }
    } else {
        motor->feedback_enabled = false;
    }

    // Initialize atomic variables
    atomic_init(&motor->position, 0);
    atomic_init(&motor->position_valid, true); // Initially valid (FIXME: limit switch)
    atomic_init(&motor->moving, false);
    atomic_init(&motor->stop_requested, false);
    atomic_init(&motor->cmd_type, 0);
    atomic_init(&motor->cmd_done, true);
    atomic_init(&motor->feedback_cnt, 0);

    // Initialize synchronized structs
    pthread_mutex_init(&motor->cmd_mutex, NULL);
    pthread_cond_init(&motor->cmd_cond, NULL);

    // Create control thred
    pthread_t *pt = lgThreadStart(motor_thread_func, motor);
    if (!pt) {
        fprintf(stderr, "Failed to start motor thread\n");
        return MOTOR_ERR_GENERAL;
    }
    motor->thread = pt;

    return MOTOR_OK;

fail_pin:
    fprintf(stderr, "Failed to claim GPIO: %s\n", lguErrorText(err));
    return MOTOR_ERR_GENERAL;
}

void motor_close(motor_t *motor)
{
    if (!motor) return;
    // Stop thread
    if (motor->thread) {
        lgThreadStop(motor->thread);
        motor->thread = NULL;
    }
    if (motor->handle >= 0) {
        // Free all pins (maybe not necessary but better be safe)
        lgGpioFree(motor->handle, motor->step_pin);
        lgGpioFree(motor->handle, motor->dir_pin);
        lgGpioFree(motor->handle, motor->ms_pins[0]);
        lgGpioFree(motor->handle, motor->ms_pins[1]);
        lgGpioFree(motor->handle, motor->ms_pins[2]);
        lgGpioFree(motor->handle, motor->sleep_pin);
        lgGpioFree(motor->handle, motor->enable_pin);
        lgGpioFree(motor->handle, motor->reset_pin);
        if (motor->feedback_pin >= 0)
            lgGpioFree(motor->handle, motor->feedback_pin);
        lgGpiochipClose(motor->handle);
    }
    pthread_mutex_destroy(&motor->cmd_mutex);
    pthread_cond_destroy(&motor->cmd_cond);
}

// MAIN THREAD LOOP
static void* motor_thread_func(void *arg)
{
    motor_t *motor = (motor_t*)arg;

    while (1) {
        // Wait for command
        pthread_mutex_lock(&motor->cmd_mutex);
        while (atomic_load(&motor->cmd_type) == 0) {
            pthread_cond_wait(&motor->cmd_cond, &motor->cmd_mutex);
        }
        int cmd = atomic_load(&motor->cmd_type);
        int32_t param = motor->cmd_param;
        atomic_store(&motor->cmd_type, 0);
        pthread_mutex_unlock(&motor->cmd_mutex);

        if (cmd == 3) {
            // STOP command is set by `motor_stop` externally through 
            // `stop_requested`, we don't deal with it here.
            // The thread is only responsible for movement control
        } else if (cmd == 1 || cmd == 2) {
            // cmd 1: relative; cmd 2: absolute
            int32_t target_rel = 0;
            int32_t cur_pos;
            bool pos_valid = motor_get_position(motor, &cur_pos) == 0;

            if (cmd == 1) {
                target_rel = param;
            } else {
                if (!pos_valid) {
                    // Invalid position
                    fprintf(stderr, "Motor: cannot absolute move, position invalid\n");
                    atomic_store(&motor->cmd_done, true);
                    continue;
                }
                target_rel = param - cur_pos;
            }

            // Set 'moving' flag
            atomic_store(&motor->stop_requested, false);
            atomic_store(&motor->moving, true);

            // Set direction
            int dir_positive = (target_rel > 0) ? 1 : 0;
            int dir_level = dir_positive ? motor->dir_active_high : !motor->dir_active_high;
            lgGpioWrite(motor->handle, motor->dir_pin, dir_level);

            // calculate pulse parameters:
            int abs_steps = (target_rel >= 0) ? target_rel : -target_rel;
            if (abs_steps == 0) {
                atomic_store(&motor->moving, false);
                atomic_store(&motor->cmd_done, true);
                continue;
            }
            int total_pulses = abs_steps * motor->microsteps;
            int freq = motor->speed * motor->microsteps; // Pulses per second
            if (freq <= 0) freq = 1;
            int period_us = 1000000 / freq; // microseconds
            int pulse_on = motor->pulse_width_us;
            int pulse_off = period_us - pulse_on;

            if (pulse_off < 10) {
                fprintf(stderr, "Motor: speed too high, period too short.\n");
                atomic_store(&motor->moving, false);
                atomic_store(&motor->cmd_done, true);
                continue;
            }

            // Record initial feedback value
            uint64_t start_cnt = 0;
            if (motor->feedback_enabled) {
                start_cnt = atomic_load(&motor->feedback_cnt);
            }

            // Begin pulses
            lgTxPulse(motor->handle, motor->step_pin, pulse_on, pulse_off, 0, total_pulses);

            // Wait for completion of STOP
            while (lgTxBusy(motor->handle, motor->step_pin, LG_TX_PWM)) {
                if (atomic_load(&motor->stop_requested)) {
                    // STOP: This stops it immediately
                    lgTxPulse(motor->handle, motor->step_pin, 0, 0, 0, 0);
                    // Wait a bit so the last edge can be properly picked up
                    lguSleep(0.002);
                    break;
                }
                lguSleep(0.001);
            }

            // Update position
            if (motor->feedback_enabled) {
                uint64_t end_cnt = atomic_load(&motor->feedback_cnt);
                int64_t actual_pulses = (int64_t)(end_cnt - start_cnt);
                if (actual_pulses < 0) actual_pulses = 0;
                // +dir -> increment, -dir -> decrement
                int32_t delta_steps = actual_pulses / motor->microsteps;
                if (!dir_positive) delta_steps = -delta_steps;

                // Atomic position update
                int32_t old_pos = atomic_load(&motor->position);
                atomic_store(&motor->position, old_pos + delta_steps);
                atomic_store(&motor->position_valid, true);
            } else {
                // No loopback: Natural completion gives theoretical position
                if (!atomic_load(&motor->stop_requested)) {
                    int32_t old_pos = atomic_load(&motor->position);
                    atomic_store(&motor->position, old_pos + target_rel);
                    atomic_store(&motor->position_valid, true);
                } else {
                    // Unnatural (STOP) invalidates position
                    atomic_store(&motor->position_valid, false);
                }
            }

            atomic_store(&motor->stop_requested, false);
            atomic_store(&motor->moving, false);
        }

        atomic_store(&motor->cmd_done, true);
    }
    return NULL;
}

// PUBLIC FUNCTIONS
int motor_move_rel(motor_t *motor, int32_t steps)
{
    if (atomic_load(&motor->moving)) return MOTOR_ERR_MOVING;

    pthread_mutex_lock(&motor->cmd_mutex);
    motor->cmd_param = steps;
    atomic_store(&motor->cmd_type, 1);
    atomic_store(&motor->cmd_done, false);
    pthread_cond_signal(&motor->cmd_cond);
    pthread_mutex_unlock(&motor->cmd_mutex);
    return MOTOR_OK;
}

int motor_move_abs(motor_t *motor, int32_t pos)
{
    if (atomic_load(&motor->moving)) return MOTOR_ERR_MOVING;

    pthread_mutex_lock(&motor->cmd_mutex);
    motor->cmd_param = pos;
    atomic_store(&motor->cmd_type, 2);
    atomic_store(&motor->cmd_done, false);
    pthread_cond_signal(&motor->cmd_cond);
    pthread_mutex_unlock(&motor->cmd_mutex);
    return MOTOR_OK;
}

int motor_stop(motor_t *motor)
{
    if (!atomic_load(&motor->moving)) return MOTOR_ERR_NOT_MOVING;
    atomic_store(&motor->stop_requested, true);
    return MOTOR_OK;
}

int motor_wait(motor_t *motor)
{
    while (atomic_load(&motor->moving) || !atomic_load(&motor->cmd_done)) {
        lguSleep(0.001);
    }
    return MOTOR_OK;
}

int motor_is_moving(motor_t *motor)
{
    return atomic_load(&motor->moving);
}

int motor_get_position(motor_t *motor, int32_t *pos)
{
    if (!atomic_load(&motor->position_valid)) {
        if (pos) *pos = 0;
        return MOTOR_ERR_POS_INVALID;
    }
    if (pos) *pos = atomic_load(&motor->position);
    return MOTOR_OK;
}

int motor_set_speed(motor_t *motor, int speed)
{
    if (atomic_load(&motor->moving)) return MOTOR_ERR_MOVING;
    if (speed <= 0) return MOTOR_ERR_PARAM;
    motor->speed = speed;
    return MOTOR_OK;
}

int motor_set_microstep(motor_t *motor, int ms)
{
    if (atomic_load(&motor->moving)) return MOTOR_ERR_MOVING;
    if (ms != 1 && ms != 2 && ms != 4 && ms != 8 && ms != 16)
        return MOTOR_ERR_PARAM;

    // MS1-3 真值表
    // Full:  LLL, Half: HLL, Quarter: LHL, Eighth: HHL, Sixteenth: HHH
    int levels[3];
    switch (ms) {
        case 1:  levels[0]=0; levels[1]=0; levels[2]=0; break;
        case 2:  levels[0]=1; levels[1]=0; levels[2]=0; break;
        case 4:  levels[0]=0; levels[1]=1; levels[2]=0; break;
        case 8:  levels[0]=1; levels[1]=1; levels[2]=0; break;
        case 16: levels[0]=1; levels[1]=1; levels[2]=1; break;
        default: return MOTOR_ERR_PARAM;
    }
    for (int i = 0; i < 3; i++) {
        lgGpioWrite(motor->handle, motor->ms_pins[i], levels[i]);
    }
    motor->microsteps = ms;
    return MOTOR_OK;
}

int motor_set_pulse_width(motor_t *motor, int us)
{
    if (atomic_load(&motor->moving)) return MOTOR_ERR_MOVING;
    if (us < 2) return MOTOR_ERR_PARAM;
    motor->pulse_width_us = us;
    return MOTOR_OK;
}

int motor_enable(motor_t *motor) {
    return lgGpioWrite(motor->handle, motor->enable_pin, 0);
}

int motor_disable(motor_t *motor) {
    return lgGpioWrite(motor->handle, motor->enable_pin, 1);
}

int motor_sleep(motor_t *motor) {
    return lgGpioWrite(motor->handle, motor->sleep_pin, 0);
}

int motor_wake(motor_t *motor) {
    return lgGpioWrite(motor->handle, motor->sleep_pin, 1);
}

int motor_reset(motor_t *motor) {
    return lgGpioWrite(motor->handle, motor->reset_pin, 0);
}

int motor_release_reset(motor_t *motor) {
    return lgGpioWrite(motor->handle, motor->reset_pin, 1);
}

int motor_zero(motor_t *motor) {
    // Sets current position to 0
    atomic_store(&motor->position, 0);
    atomic_store(&motor->position_valid, true);
    return MOTOR_OK;
}

int motor_home(motor_t *motor) {
    // (NOT IMPLEMENTED)
    // Homing
    return MOTOR_ERR_NOT_IMPL;
}

int motor_lock(motor_t *motor) {
    // Re-enable output to 'lock' motor from external forces
    return motor_enable(motor);
}

int motor_unlock(motor_t *motor) {
    // Shutdown output: pull ENABLE high
    // so the motor spins freely
    return motor_disable(motor);
}