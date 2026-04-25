#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "config.h"
#include "motor.h"

int main(int argc, char *argv[])
{
    const char *conf_file = "motor.ini";
    if (argc >= 2) conf_file = argv[1];

    motor_config_t cfg;
    if (config_load(conf_file, &cfg) != 0) {
        return EXIT_FAILURE;
    }

    motor_t motor;
    if (motor_init(&motor, &cfg) != MOTOR_OK) {
        return EXIT_FAILURE;
    }

    printf("Motor initialized.\n");

    // Test sequence:
    printf("Enable motor...\n");
    motor_enable(&motor);
    lguSleep(0.1);

    printf("Move relative +5000 steps...\n");
    motor_move_rel(&motor, 5000);
    motor_wait(&motor);
    int32_t pos;
    if (motor_get_position(&motor, &pos) == 0)
        printf("Position after +5000: %d\n", pos);
    else
        printf("Position invalid.\n");

    printf("Move relative -2000 steps...\n");
    motor_move_rel(&motor, -2000);
    motor_wait(&motor);
    if (motor_get_position(&motor, &pos) == 0)
        printf("Position after -2000: %d\n", pos);

    printf("Start long movement +20000 ...\n");
    motor_move_rel(&motor, 20000);
    lguSleep(0.3);  // 运动一部分
    printf("Emergency stop!\n");
    motor_stop(&motor);
    motor_wait(&motor);
    if (motor_get_position(&motor, &pos) == 0)
        printf("Position after stop: %d\n", pos);
    else
        printf("Position is now INVALID (no feedback or stop during motion).\n");

    printf("Zero position...\n");
    motor_zero(&motor);
    printf("Position after zero: ");
    if (motor_get_position(&motor, &pos) == 0) printf("%d\n", pos);
    else printf("INVALID\n");

    printf("Set microstep to 8...\n");
    if (motor_set_microstep(&motor, 8) != 0) {
        printf("Failed (maybe still moving?)\n");
    }

    printf("Move relative +1000 steps with microstep 8...\n");
    motor_move_rel(&motor, 1000);
    motor_wait(&motor);
    if (motor_get_position(&motor, &pos) == 0)
        printf("Position: %d\n", pos);

    printf("Disable motor...\n");
    motor_disable(&motor);

    printf("Test complete.\n");
    motor_close(&motor);
    return EXIT_SUCCESS;
}