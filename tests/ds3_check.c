// runtime/input_ds3.c: the DualShock 3 input report decoded to inputs by position, and the output report's layout.
// Built and run by tests/ds3_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "input_ds3.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A report at rest: id 1, sticks centred, nothing pressed.
static void rest(uint8_t r[49]) {
    memset(r, 0, 49);
    r[0] = 0x01;
    r[6] = r[7] = r[8] = r[9] = 128;
}

int main(void) {
    uint8_t r[49], pd[PAD_COUNT];
    int32_t raw[4];

    rest(r);
    CHECK(ds3_decode(r, 49, pd, raw), "a rest report decodes");
    for (int i = 0; i < PAD_COUNT; i++) CHECK(!pd[i], "rest: input %d is down", i);
    for (int i = 0; i < 4; i++) CHECK(raw[i] == 0, "rest: axis %d is %d", i, raw[i]);

    // Every button of byte 2 and 3 alone lights exactly its own input.
    static const struct { int byte; uint8_t bit; int pad; const char *name; } map[] = {
        {2, 0x01, PAD_BACK, "select"}, {2, 0x02, PAD_LSTICK, "L3"}, {2, 0x04, PAD_RSTICK, "R3"},
        {2, 0x08, PAD_START, "start"}, {2, 0x10, PAD_DPAD_UP, "up"}, {2, 0x20, PAD_DPAD_RIGHT, "right"},
        {2, 0x40, PAD_DPAD_DOWN, "down"}, {2, 0x80, PAD_DPAD_LEFT, "left"},
        {3, 0x01, PAD_ZL, "L2"}, {3, 0x02, PAD_ZR, "R2"}, {3, 0x04, PAD_L, "L1"}, {3, 0x08, PAD_R, "R1"},
        {3, 0x10, PAD_NORTH, "triangle"}, {3, 0x20, PAD_EAST, "circle"}, {3, 0x40, PAD_SOUTH, "cross"},
        {3, 0x80, PAD_WEST, "square"}};
    for (size_t k = 0; k < sizeof map / sizeof *map; k++) {
        rest(r);
        r[map[k].byte] = map[k].bit;
        CHECK(ds3_decode(r, 49, pd, raw), "%s decodes", map[k].name);
        for (int i = 0; i < PAD_COUNT; i++)
            CHECK(!!pd[i] == (i == map[k].pad), "%s: input %d is %d", map[k].name, i, pd[i]);
    }

    // The PS button (byte 4) is not a Switch button.
    rest(r);
    r[4] = 1;
    ds3_decode(r, 49, pd, raw);
    for (int i = 0; i < PAD_COUNT; i++) CHECK(!pd[i], "PS button: input %d is down", i);

    // Sticks: 0 and 255 reach the ends, y is turned so that up is positive.
    rest(r);
    r[6] = 0, r[7] = 0, r[8] = 255, r[9] = 255;
    ds3_decode(r, 49, pd, raw);
    CHECK(raw[0] == -32767 && raw[1] == 32767, "left stick up-left: %d %d", raw[0], raw[1]);
    CHECK(raw[2] == 32767 && raw[3] == -32767, "right stick down-right: %d %d", raw[2], raw[3]);
    rest(r);
    r[6] = 129, r[7] = 127;
    ds3_decode(r, 49, pd, raw);
    CHECK(raw[0] > 0 && raw[0] < 400 && raw[1] > 0 && raw[1] < 400, "one step from rest: %d %d", raw[0], raw[1]);

    // Triggers: pressure decides when the report has it, the switch when the pressure reads 0.
    rest(r);
    r[3] = 0x01, r[18] = 100;
    ds3_decode(r, 49, pd, raw);
    CHECK(!pd[PAD_ZL], "L2 at pressure 100 with the switch on is %d", pd[PAD_ZL]);
    r[18] = 200;
    ds3_decode(r, 49, pd, raw);
    CHECK(pd[PAD_ZL], "L2 at pressure 200 is %d", pd[PAD_ZL]);
    r[19] = 255;
    r[3] = 0;
    ds3_decode(r, 49, pd, raw);
    CHECK(pd[PAD_ZR], "R2 at pressure 255 without the switch is %d", pd[PAD_ZR]);
    rest(r);
    r[3] = 0x02;
    ds3_decode(r, 12, pd, raw);   // a short report: the switch alone
    CHECK(pd[PAD_ZR], "R2 switch in a short report is %d", pd[PAD_ZR]);

    // Other reports are refused and leave pd and raw alone.
    memset(pd, 7, sizeof pd);
    raw[0] = 123;
    rest(r);
    r[0] = 0x02;
    CHECK(!ds3_decode(r, 49, pd, raw) && pd[PAD_SOUTH] == 7 && raw[0] == 123, "another report id is refused");
    rest(r);
    CHECK(!ds3_decode(r, 9, pd, raw) && pd[PAD_SOUTH] == 7, "a short report is refused");

    // Output report.
    uint8_t o[49];
    memset(o, 0xAA, sizeof o);
    ds3_output_report(o, sizeof o, 200, 1, 30, 1);
    CHECK(o[0] == 0x01 && o[1] == 0, "output: id %02x padding %02x", o[0], o[1]);
    CHECK(o[2] == 30 && o[3] == 1 && o[4] == 30 && o[5] == 200, "output: motors %d %d %d %d", o[2], o[3], o[4], o[5]);
    CHECK(o[10] == 0x02, "output: light mask %02x", o[10]);
    for (int i = 0; i < 4; i++)
        CHECK(!memcmp(o + 11 + 5 * i, "\xFF\x27\x10\x00\x32", 5), "output: light %d timing", i);
    for (int i = 6; i < 10; i++) CHECK(o[i] == 0, "output: byte %d is %02x", i, o[i]);
    for (int i = 31; i < 49; i++) CHECK(o[i] == 0, "output: tail byte %d is %02x", i, o[i]);
    ds3_output_report(o, sizeof o, 999, 5, 0, 0);
    CHECK(o[3] == 1 && o[2] == 0 && o[10] == 0, "output: light on is 1 (%d), stop (%d), no light (%02x)", o[3], o[2], o[10]);
    memset(o, 0xAA, sizeof o);
    ds3_output_report(o, 20, 1, 1, 1, 1);   // too short for the layout: zeros
    for (int i = 0; i < 20; i++) CHECK(o[i] == 0, "output: short buffer byte %d is %02x", i, o[i]);

    printf(fails ? "ds3_check: %d failed\n" : "ds3_check: ok\n", fails);
    return fails != 0;
}
