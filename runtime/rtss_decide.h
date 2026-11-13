// RivaTuner Statistics Server as the frame limiter (rtss_limit.c): the parts that make no system call, so that
// tests/rtss_limit_check.c builds them on any system. rtss_parse reads RTSS's shared memory, rtss_decide decides each
// poll of the worker, and rtss_poll carries that decision out through the calls rtss_limit.c gives it (RtssOps).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

// ---- RTSS's shared memory, "RTSSSharedMemoryV2". The header: signature, version, application entry size, offset of
// the application array, its length. An entry: the process id, the executable path (260 bytes), flags, then RTSS's
// last frame rate period for the process: its start and end in ms and the frames in it.
#define RTSS_SIGNATURE 0x52545353u   // 'RTSS' as RTSS writes it: the memory holds valid data
enum { RTSS_PERIOD_AT = 4 + 260 + 4, RTSS_ENTRY_MIN = RTSS_PERIOD_AT + 12, RTSS_APPS_MAX = 4096 };

typedef struct {
    int running;                 // the signature and a version of 2.0 or later
    int listed;                  // the application list holds this process: RTSS has hooked it
    uint32_t t0, t1, frames;     // the period of this process's entry
} RtssApp;

// Reads the memory of size bytes at p for the process pid; never reads past size.
static inline void rtss_parse(const uint8_t *p, size_t size, uint32_t pid, RtssApp *a) {
    memset(a, 0, sizeof *a);
    if (!p || size < 20) return;
    uint32_t h[5];
    memcpy(h, p, sizeof h);
    a->running = h[0] == RTSS_SIGNATURE && h[1] >= 0x20000;
    for (uint32_t i = 0; a->running && h[2] >= RTSS_ENTRY_MIN && i < h[4] && i < RTSS_APPS_MAX; i++) {
        uint64_t at = (uint64_t)h[3] + (uint64_t)i * h[2];
        if (at + RTSS_ENTRY_MIN > size) break;
        uint32_t id, t[3];
        memcpy(&id, p + at, 4);
        if (id != pid) continue;
        a->listed = 1;
        memcpy(t, p + at + RTSS_PERIOD_AT, sizeof t);
        a->t0 = t[0], a->t1 = t[1], a->frames = t[2];
        break;
    }
}

// The frame rate RTSS measured for the process over its period; 0 unknown.
static inline double rtss_fps(const RtssApp *a) {
    return a->listed && a->t1 > a->t0 ? 1000.0 * a->frames / (a->t1 - a->t0) : 0;
}

// ---- the worker's decisions
#define RTSS_CHECK_POLLS 6     // polls in a row RTSS's own frame rate of the game may stay above the limit
#define RTSS_CHECK_SLACK 1.2   // above limit * this, RTSS is taken not to hold the game to it

enum { RTSS_FAIL_NONE, RTSS_FAIL_LIBRARY, RTSS_FAIL_READONLY, RTSS_FAIL_NOT_LIMITING };   // out of it for the run
enum { RTSS_DO_NOTHING, RTSS_DO_APPLY, RTSS_DO_PUT_BACK };
enum {
    RTSS_WHY_HOLDS,         // RTSS holds the game to the want
    RTSS_WHY_NOT_LISTED,    // RTSS has the limit but has not hooked the game yet
    RTSS_WHY_OUT,           // kept out (the want says why)
    RTSS_WHY_FAILED,        // RtssState.fail
    RTSS_WHY_NOT_RUNNING,
    RTSS_WHY_REFUSED,       // RTSS refused this limit
    RTSS_WHY_NEWER,         // the want changed while the poll ran: nothing decided, the next poll takes the new one
    RTSS_WHY_UNSET,         // no want yet
    RTSS_WHY_CLOSED,        // the exit put the profile back (rtss_limit_restore)
};

// What the worker keeps between polls.
typedef struct {
    int64_t applied;   // the millihertz the profile holds from this run; -1 untouched
    int64_t refused;   // a limit RTSS refused (out of its range): the game's own limiter keeps that one
    int changed;       // the profile holds a value from this run: put the old one back on exit
    int over;          // polls in a row RTSS's frame rate of the game stayed above the limit
    int fail;          // RTSS_FAIL_*
} RtssState;
#define RTSS_STATE_INIT {-1, -1, 0, 0, RTSS_FAIL_NONE}

// One poll's view.
typedef struct {
    int64_t want;   // the want the poll read
    int current;    // the want is still that one when the poll decides
    int closed;     // rtss_limit_restore ran
    int hooks;      // RTSS's hooks library: 1 loaded, 0 failed to load, -1 not tried (rtss_wants_hooks)
    RtssApp app;
} RtssIn;

typedef struct {
    int act;        // RTSS_DO_*
    int64_t mhz;    // the want decided for
    int holds;      // RTSS holds the game to it: the game's own limiter stands aside
    int why;        // RTSS_WHY_*
    double fps;     // RTSS's frame rate of the game, when the frame count check gave up on it
} RtssOut;

// The hooks library is loaded only on a poll that may write the profile: a limit is wanted, RTSS is not out of it for
// the run, and it runs. A kept-out run never loads it.
static inline int rtss_wants_hooks(const RtssState *s, const RtssIn *in) {
    return in->want >= 0 && !s->fail && in->app.running;
}

// The decision of one poll, in this order: a closed run or no want does nothing; a kept-out want puts back what this
// run wrote (once the hooks are loaded); a failure lasts the run; RTSS not running leaves the profile for its return; a
// library that did not load fails the run; a refused limit stays with the game; a new limit goes into the profile at
// once, and RTSS holds it as soon as it lists the game. On an unchanged limit RTSS's own frame count is checked: more
// than RTSS_CHECK_SLACK times the limit for RTSS_CHECK_POLLS polls in a row puts the profile back for the run.
static inline RtssOut rtss_decide(RtssState *s, const RtssIn *in) {
    int64_t w = in->want;
    RtssOut o = {RTSS_DO_NOTHING, w, 0, RTSS_WHY_NOT_LISTED, 0};
    if (in->closed) return o.why = RTSS_WHY_CLOSED, o;
    if (w == RTSS_UNSET) return o.why = RTSS_WHY_UNSET, o;
    if (w < 0) {
        if (in->hooks == 1 && s->changed) o.act = RTSS_DO_PUT_BACK;
        return o.why = RTSS_WHY_OUT, o;
    }
    if (s->fail) return o.why = RTSS_WHY_FAILED, o;
    if (!in->app.running) return o.why = RTSS_WHY_NOT_RUNNING, o;
    if (in->hooks == 0) return s->fail = RTSS_FAIL_LIBRARY, o.why = RTSS_WHY_FAILED, o;
    if (w == s->refused) return o.why = RTSS_WHY_REFUSED, o;
    o.holds = in->app.listed;
    o.why = o.holds ? RTSS_WHY_HOLDS : RTSS_WHY_NOT_LISTED;
    if (w != s->applied) {   // the result of the write may still undo the hold (rtss_written)
        o.act = RTSS_DO_APPLY;
        s->over = 0;
        return o;
    }
    if (!in->current) return o.holds = 0, o.why = RTSS_WHY_NEWER, o;   // these frames are not the new want's
    double fps = rtss_fps(&in->app);
    if (!in->app.listed || w == 0 || fps <= w / 1000.0 * RTSS_CHECK_SLACK) s->over = 0;
    else if (++s->over == RTSS_CHECK_POLLS) {
        s->fail = RTSS_FAIL_NOT_LIMITING;
        o.act = RTSS_DO_PUT_BACK, o.holds = 0, o.why = RTSS_WHY_FAILED, o.fps = fps;
    }
    return o;
}

// What a write of the limit did (RtssOps.apply).
enum {
    RTSS_WROTE_READONLY = -1,        // the save did not reach the file
    RTSS_WROTE_REFUSED = 0,          // RTSS refused the value; the profile keeps what it held
    RTSS_WROTE_HELD = 1,             // the profile holds the limit
    RTSS_WROTE_REFUSED_CLEARED = 2,  // refused, and the earlier limit of this run was cleared to none
};

static inline void rtss_written(RtssState *s, RtssOut *o, int r) {
    if (r == RTSS_WROTE_READONLY) {
        s->fail = RTSS_FAIL_READONLY, o->holds = 0, o->why = RTSS_WHY_FAILED;
        return;
    }
    s->changed = 1;
    if (r == RTSS_WROTE_HELD) {
        s->applied = o->mhz;
        return;
    }
    if (r == RTSS_WROTE_REFUSED_CLEARED) s->applied = 0;
    s->refused = o->mhz, o->holds = 0, o->why = RTSS_WHY_REFUSED;
}

// The profile was put back: nothing of this run is in it.
static inline void rtss_put_back_done(RtssState *s) { s->changed = 0, s->applied = -1; }

// The profile calls rtss_poll makes. apply writes mhz (first: the first write of the run, which records what to put
// back; clear: on a refusal, clear this run's earlier limit to none) and returns RTSS_WROTE_*; put_back puts the profile
// back as it was before the run (o says why: kept out, or the frame count check gave up).
typedef struct {
    void *ctx;
    int (*apply)(void *ctx, int64_t mhz, int first, int clear);
    void (*put_back)(void *ctx, const RtssOut *o);
} RtssOps;

static inline RtssOut rtss_poll(RtssState *s, const RtssIn *in, const RtssOps *ops) {
    RtssOut o = rtss_decide(s, in);
    if (o.act == RTSS_DO_APPLY) {
        rtss_written(s, &o, ops->apply(ops->ctx, o.mhz, !s->changed, s->applied != 0));
    } else if (o.act == RTSS_DO_PUT_BACK) {
        ops->put_back(ops->ctx, &o);
        rtss_put_back_done(s);
    }
    return o;
}

static inline const char *rtss_fail_text(int fail) {
    return fail == RTSS_FAIL_READONLY       ? "RivaTuner Statistics Server's profile cannot be changed"
           : fail == RTSS_FAIL_NOT_LIMITING ? "RivaTuner Statistics Server does not hold the game to the limit"
                                            : "RivaTuner Statistics Server's hooks library did not load";
}

// Why the game's own limiter paces, for the log line "the game's own limiter (<text>)".
static inline void rtss_why_text(char *out, size_t n, const RtssOut *o, int fail) {
    if (o->why == RTSS_WHY_OUT) snprintf(out, n, "RivaTuner Statistics Server kept out: %s", rtss_out_text(o->mhz));
    else if (o->why == RTSS_WHY_FAILED) snprintf(out, n, "%s", rtss_fail_text(fail));
    else if (o->why == RTSS_WHY_NOT_RUNNING) snprintf(out, n, "RivaTuner Statistics Server is not running");
    else if (o->why == RTSS_WHY_REFUSED) snprintf(out, n, "RivaTuner Statistics Server refused this limit");
    else snprintf(out, n, "RivaTuner Statistics Server has not hooked the game yet");
}

// ---- RTSS's own settings
// RTSS's own [Framerate] settings for the game, as its profile file holds them (rtss_limit.c read_settings): a frame
// rate limit (Limit over LimitDenominator), a frame time limit (LimitTime over LimitTimeDenominator) and scanline sync
// (SyncScanline0 and SyncScanline1), 0 off for each; and the modes beside them, for the log: SyncDisplay, ReflexSleep
// and PassiveWait.
typedef struct {
    int limit, limit_den, time, time_den, scan0, scan1;
    int sync_display, reflex_sleep, passive_wait;
} RtssSettings;

// RTSS holds the game's frames by a setting of its own.
static inline int rtss_settings_limit(const RtssSettings *s) { return s->limit || s->time || s->scan0 || s->scan1; }

// Two limiters: RTSS is kept out of the game's limit, and while it lists the game a setting of its own still holds the
// frames (rtss_settings_limit), beside the game's limiter, the driver's or frame generation's.
static inline int rtss_two(int64_t want, int listed, int own_limit) {
    return want < 0 && want != RTSS_UNSET && listed && own_limit;
}

// "limit L/D, frame time limit T/TD, scanline sync S0/S1", and with modes ", display sync X, Reflex sleep R, passive
// wait P": the values as the file holds them.
static inline void rtss_settings_text(char *out, size_t n, const RtssSettings *s, int modes) {
    int k = snprintf(out, n, "limit %d/%d, frame time limit %d/%d, scanline sync %d/%d", s->limit, s->limit_den, s->time,
                     s->time_den, s->scan0, s->scan1);
    if (modes && k >= 0 && (size_t)k < n)
        snprintf(out + k, n - (size_t)k, ", display sync %d, Reflex sleep %d, passive wait %d", s->sync_display,
                 s->reflex_sleep, s->passive_wait);
}
