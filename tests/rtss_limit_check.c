// runtime/rtss_decide.h: the parts of RivaTuner Statistics Server's frame limiting (rtss_limit.c) that make no system
// call. Built and run by tests/rtss_limit_check.py on any system; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "rtss_decide.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// RTSS's own settings: which of them hold the game's frames, and the log's text of them.
static void settings(void) {
    RtssSettings none = {0, 1, 0, 0, 0, 0};
    CHECK(!rtss_settings_limit(&none), "no limit, no frame time limit, no scanline sync: RTSS holds nothing");
    RtssSettings s = none;
    s.limit = 50;
    CHECK(rtss_settings_limit(&s), "a frame rate limit holds the frames");
    s = none, s.time = 16667, s.time_den = 1;
    CHECK(rtss_settings_limit(&s), "a frame time limit holds the frames");
    s = none, s.scan0 = -40;
    CHECK(rtss_settings_limit(&s), "scanline sync from the bottom holds the frames");
    s = none, s.scan1 = 1;
    CHECK(rtss_settings_limit(&s), "the second scanline sync holds the frames");
    s = none, s.limit_den = 1000, s.time_den = 7;
    CHECK(!rtss_settings_limit(&s), "denominators alone hold nothing");
    char text[128];
    s = (RtssSettings){59940, 1000, 16667, 1, -40, 0};
    rtss_settings_text(text, sizeof text, &s);
    CHECK(!strcmp(text, "limit 59940/1000, frame time limit 16667/1, scanline sync -40/0"), "text: %s", text);
    char small[16];
    rtss_settings_text(small, sizeof small, &s);
    CHECK(strlen(small) == sizeof small - 1, "a short buffer is cut, not overrun");
}

// The kept-out reasons, for the log.
static void reasons(void) {
    CHECK(!strcmp(rtss_out_text(RTSS_OUT_SETTING), "NX2NAVI_RTSS=0"), "the setting");
    CHECK(!strcmp(rtss_out_text(RTSS_OUT_FRAMEGEN), "frame generation"), "frame generation");
    CHECK(!strcmp(rtss_out_text(RTSS_OUT_LOWLAT), "the driver's low latency"), "low latency");
    CHECK(!strcmp(rtss_out_limiter(RTSS_OUT_SETTING), "the game's limiter"), "the setting leaves the game's limiter");
    CHECK(!strcmp(rtss_out_limiter(RTSS_OUT_LOWLAT), "the driver's low latency"), "low latency holds the frames");
    CHECK(RTSS_UNSET < 0 && RTSS_OUT_SETTING < 0 && RTSS_OUT_FRAMEGEN < 0 && RTSS_OUT_LOWLAT < 0,
          "every reason is below 0, where a limit never is");
    CHECK(RTSS_UNSET != RTSS_OUT_SETTING && RTSS_UNSET != RTSS_OUT_FRAMEGEN && RTSS_UNSET != RTSS_OUT_LOWLAT &&
              RTSS_OUT_SETTING != RTSS_OUT_FRAMEGEN && RTSS_OUT_FRAMEGEN != RTSS_OUT_LOWLAT &&
              RTSS_OUT_SETTING != RTSS_OUT_LOWLAT,
          "the reasons differ");
}

int main(void) {
    settings();
    reasons();
    printf("rtss_limit_check: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
