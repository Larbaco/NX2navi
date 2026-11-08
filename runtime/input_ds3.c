// The DualShock 3's report formats, free of any operating system (input_ds3.h): the input report to inputs by position
// and the output report that drives the motors and the player light. tests/ds3_check.c checks both; input_ds3_win.c
// moves the reports.
#include <string.h>

#include "input_ds3.h"

int ds3_decode(const uint8_t *r, unsigned n, uint8_t pd[PAD_COUNT], int32_t raw[4]) {
    if (n < 10 || r[0] != 0x01) return 0;
    uint8_t b1 = r[2], b2 = r[3];
    memset(pd, 0, PAD_COUNT);
    pd[PAD_BACK] = (b1 & 0x01) != 0, pd[PAD_LSTICK] = (b1 & 0x02) != 0, pd[PAD_RSTICK] = (b1 & 0x04) != 0;
    pd[PAD_START] = (b1 & 0x08) != 0;
    pd[PAD_DPAD_UP] = (b1 & 0x10) != 0, pd[PAD_DPAD_RIGHT] = (b1 & 0x20) != 0;
    pd[PAD_DPAD_DOWN] = (b1 & 0x40) != 0, pd[PAD_DPAD_LEFT] = (b1 & 0x80) != 0;
    pd[PAD_L] = (b2 & 0x04) != 0, pd[PAD_R] = (b2 & 0x08) != 0;
    pd[PAD_NORTH] = (b2 & 0x10) != 0, pd[PAD_EAST] = (b2 & 0x20) != 0;   // triangle, circle
    pd[PAD_SOUTH] = (b2 & 0x40) != 0, pd[PAD_WEST] = (b2 & 0x80) != 0;   // cross, square
    // The triggers go by pressure, as an XInput trigger does; a report without it (n < 20) has the switch only
    unsigned l2 = n > 19 ? r[18] : 0, r2 = n > 19 ? r[19] : 0;
    pd[PAD_ZL] = l2 ? l2 > DS3_TRIGGER_ON : (b2 & 0x01) != 0;
    pd[PAD_ZR] = r2 ? r2 > DS3_TRIGGER_ON : (b2 & 0x02) != 0;
    for (int i = 0; i < 4; i++) {
        int d = (int)r[6 + i] - 128;
        int v = d >= 0 ? d * 32767 / 127 : d * 32767 / 128;
        raw[i] = i & 1 ? -v : v;   // y: the controller's grows downwards, the game's upwards
    }
    return 1;
}

void ds3_output_report(uint8_t *o, unsigned n, int left, int right, unsigned dur, int led) {
    memset(o, 0, n);
    if (n < 36) return;
    o[0] = 0x01;
    o[2] = (uint8_t)dur, o[3] = (uint8_t)(right != 0);
    o[4] = (uint8_t)dur, o[5] = (uint8_t)left;
    o[10] = (uint8_t)(led << 1);
    for (int i = 0; i < 4; i++) {   // the four lights' blink timing: steady
        uint8_t *l = o + 11 + 5 * i;
        l[0] = 0xFF, l[1] = 0x27, l[2] = 0x10, l[3] = 0x00, l[4] = 0x32;
    }
}
