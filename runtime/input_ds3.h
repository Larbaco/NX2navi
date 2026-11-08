// The DualShock 3 reader of the Windows build (input_ds3_win.c): the controller's own HID reports, read directly, for
// when no driver presents it to SDL or XInput as a gamepad. Windows only; input.c calls it from its gamepad poll.
// NX2NAVI_DS3=0 (`ds3 = 0` in nx2navi.ini) turns it off. Controls: docs/controls.md.
#pragma once
#include <stdint.h>

#include "input_map.h"

// One poll, under input.c's lock: 1 while a DualShock 3 is connected and reporting, with its inputs by position in
// pd[PAD_*] (every entry written; PAD_ZL and PAD_ZR from the trigger pressure) and its sticks in raw[] as lx, ly, rx, ry
// in -32767..32767, up is +y, before any dead zone. 0 (pd and raw untouched) without one. Also finds a controller that
// is plugged in or switched on later, and lets go of one that went away.
int ds3_read(uint8_t pd[PAD_COUNT], int32_t raw[4]);

// Rumble for the connected controller, under input.c's lock: low (0..65535) drives the heavy (left) motor, high the
// light (right) motor, which the controller can only switch on or off (on from 1/16 of full strength). Lasts ms
// milliseconds; 0 stops it. 1 when a controller took it.
int ds3_rumble(uint16_t low, uint16_t high, uint32_t ms);

// ---- the report formats (input_ds3.c, any OS)
enum { DS3_TRIGGER_ON = 128 };   // L2/R2 pressure, 0..255, that counts as pressed

// Input report 0x01 (n bytes, the report id first) to inputs by position in pd[PAD_*] (every entry written) and the
// sticks in raw[] as lx, ly, rx, ry (-32767..32767, up is +y, rest 0). 0, with pd and raw untouched, for another report.
int ds3_decode(const uint8_t *r, unsigned n, uint8_t pd[PAD_COUNT], int32_t raw[4]);

// Output report 0x01 into o (n bytes, 36 or more; zeroed first): the heavy motor at left (0..255), the light motor on
// when right is nonzero, both for dur steps of 10 ms (0 stops them), and player light led (1..4, 0 none).
void ds3_output_report(uint8_t *o, unsigned n, int left, int right, unsigned dur, int led);
