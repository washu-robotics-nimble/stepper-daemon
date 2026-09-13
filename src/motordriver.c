#include <stdio.h>
#include <stdlib.h>
#include "../lib/lg/lgpio.h"

#define OUT_PIN 12
#define LOOPS 15000
#define LFLAGS 0
int main(int argc, char *argv[])
{
    int h;
    int i;
    double start, end;
    h = lgGpiochipOpen(0);
    if (h >= 0)
    {
        if (lgGpioClaimOutput(h, LFLAGS, OUT_PIN, 0) == LG_OKAY)
        {
            lgTxPulse(h, OUT_PIN, 20000, 30000, 0, 0); 
            // This, very unfortunately, is software PWM. There exists a
            // way to do hardware PWM by directly write to /dev/ but lgpio
            // does not support it. Maybe a future task.

            lguSleep(2);
            lgTxPulse(h, OUT_PIN, 500, 100, 0, LOOPS);
            start = lguTime();
            while (lgTxBusy(h, OUT_PIN, LG_TX_PWM)) lguSleep(0.01);
            end = lguTime();
            printf("%d cycles at 40Hz took %.1f seconds\n", LOOPS, end-start);
        }
    }
    lgGpiochipClose(h);
}