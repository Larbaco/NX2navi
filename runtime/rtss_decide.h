// RivaTuner Statistics Server as the frame limiter (rtss_limit.c): the parts that make no system call, so that
// tests/rtss_limit_check.c builds them on any system.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// rtss_limit_want's argument: a limit in millihertz (>0), 0 for none, or why RTSS is kept out of it (frame_rate.c
// frame_step_cap_ns). RTSS_UNSET is the want before the first one.
enum {
    RTSS_UNSET = -2,
    RTSS_OUT_SETTING = -1,    // NX2NAVI_RTSS=0, for the whole run
    RTSS_OUT_FRAMEGEN = -3,   // frame generation: RTSS would count the generated frames too
    RTSS_OUT_LOWLAT = -4,     // a driver's low latency (vk_lowlat.c) holds the frames where they start
};

// What keeps RTSS out, for the log.
static inline const char *rtss_out_text(int64_t want) {
    return want == RTSS_OUT_FRAMEGEN ? "frame generation"
           : want == RTSS_OUT_LOWLAT ? "the driver's low latency"
                                     : "NX2NAVI_RTSS=0";
}

// The limiter that holds the frames while RTSS is kept out, for the log.
static inline const char *rtss_out_limiter(int64_t want) {
    return want == RTSS_OUT_FRAMEGEN ? "frame generation and the game's limiter"
           : want == RTSS_OUT_LOWLAT ? "the driver's low latency"
                                     : "the game's limiter";
}

// RTSS's own [Framerate] settings for the game, as its profile file holds them (rtss_limit.c read_settings): a frame
// rate limit (Limit over LimitDenominator), a frame time limit (LimitTime over LimitTimeDenominator) and scanline sync
// (SyncScanline0 and SyncScanline1); 0 is off for each.
typedef struct {
    int limit, limit_den, time, time_den, scan0, scan1;
} RtssSettings;

// RTSS holds the game's frames by a setting of its own.
static inline int rtss_settings_limit(const RtssSettings *s) { return s->limit || s->time || s->scan0 || s->scan1; }

// "limit L/D, frame time limit T/TD, scanline sync S0/S1", the values as the file holds them.
static inline void rtss_settings_text(char *out, size_t n, const RtssSettings *s) {
    snprintf(out, n, "limit %d/%d, frame time limit %d/%d, scanline sync %d/%d", s->limit, s->limit_den, s->time,
             s->time_den, s->scan0, s->scan1);
}
