// runtime/rtss_decide.h: the parts of RivaTuner Statistics Server's frame limiting (rtss_limit.c) that make no system
// call: the shared memory's reader against a fake RTSSSharedMemoryV2, and the worker's decisions poll by poll against a
// fake profile. The cases pin the decisions as they stand, including those a pending change would alter (writing
// before RTSS lists the game, the frame count rule, a write per movie and per switch of low latency). Built and run by
// tests/rtss_limit_check.py on any system; prints each failed check and exits 1 on any.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtss_decide.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// RTSS's own settings: which of them hold the game's frames, and the log's text of them.
static void settings(void) {
    RtssSettings none = {0, 1, 0, 0, 0, 0, 1, 1, 1};
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
    CHECK(!rtss_settings_limit(&none), "display sync, Reflex sleep and passive wait alone hold nothing");
    char text[192];
    s = (RtssSettings){59940, 1000, 16667, 1, -40, 0, 1, 2, 0};
    rtss_settings_text(text, sizeof text, &s, 0);
    CHECK(!strcmp(text, "limit 59940/1000, frame time limit 16667/1, scanline sync -40/0"), "text: %s", text);
    rtss_settings_text(text, sizeof text, &s, 1);
    CHECK(!strcmp(text, "limit 59940/1000, frame time limit 16667/1, scanline sync -40/0, display sync 1, Reflex sleep 2, "
                        "passive wait 0"),
          "text with the modes: %s", text);
    char small[16], mid[70];
    rtss_settings_text(small, sizeof small, &s, 1);
    CHECK(strlen(small) == sizeof small - 1, "a short buffer is cut, not overrun");
    rtss_settings_text(mid, sizeof mid, &s, 1);
    CHECK(strlen(mid) == sizeof mid - 1 && !strncmp(mid, text, sizeof mid - 1), "the modes cut at the buffer's end");
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

// ---- a fake RTSSSharedMemoryV2: the header and application entries written as RTSS lays them out
enum { PID = 4242, ESIZE = 1024, OFF = 0x100 };
static void shm_set(uint8_t *v, uint32_t sig, uint32_t ver, uint32_t esize, uint32_t off, uint32_t n) {
    uint32_t h[5] = {sig, ver, esize, off, n};
    memcpy(v, h, sizeof h);
}
static void app_put(uint8_t *v, uint32_t esize, uint32_t off, uint32_t slot, uint32_t pid, uint32_t t0, uint32_t t1,
                    uint32_t frames) {
    uint8_t *e = v + off + (size_t)slot * esize;
    uint32_t t[3] = {t0, t1, frames};
    memcpy(e, &pid, 4);
    memcpy(e + RTSS_PERIOD_AT, t, sizeof t);
}

// rtss_parse: where the entry is, and that a bad header, a short entry or an array past the memory finds nothing.
static void parse(void) {
    static uint8_t v[65536];
    RtssApp a;
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, ESIZE, OFF, 3);
    app_put(v, ESIZE, OFF, 0, 7, 0, 0, 0);
    app_put(v, ESIZE, OFF, 1, 8, 0, 0, 0);
    app_put(v, ESIZE, OFF, 2, PID, 1000, 2000, 60);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.running && a.listed && a.t0 == 1000 && a.t1 == 2000 && a.frames == 60, "the third entry is ours");
    CHECK(rtss_fps(&a) == 60.0, "60 frames in a second: %f", rtss_fps(&a));
    rtss_parse(v, sizeof v, 99, &a);
    CHECK(a.running && !a.listed && rtss_fps(&a) == 0, "another process: running, not listed");
    shm_set(v, 0xDEAD, 0x0002000E, ESIZE, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(!a.running && !a.listed, "no signature: not running");
    shm_set(v, RTSS_SIGNATURE, 0x00010003, ESIZE, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(!a.running, "a version before 2.0: not running");
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN - 1, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.running && !a.listed, "entries shorter than the period: not read");
    rtss_parse(v, 19, PID, &a);
    CHECK(!a.running, "a memory shorter than the header");
    rtss_parse(NULL, 0, PID, &a);
    CHECK(!a.running, "no memory");
    // entries of the smallest size, ours the last that fits: found; past the memory's end: not read
    memset(v, 0, sizeof v);
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN, OFF, 1000);
    uint32_t fit = (uint32_t)((sizeof v - OFF) / RTSS_ENTRY_MIN) - 1;
    app_put(v, RTSS_ENTRY_MIN, OFF, fit, PID, 1000, 1500, 30);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.listed && a.frames == 30, "the last entry inside the memory is read");
    rtss_parse(v, OFF + (size_t)fit * RTSS_ENTRY_MIN + RTSS_ENTRY_MIN - 1, PID, &a);
    CHECK(a.running && !a.listed, "an entry one byte past the memory is not read");
    // more than RTSS_APPS_MAX entries: the walk stops there
    size_t big = OFF + (size_t)5000 * RTSS_ENTRY_MIN;
    uint8_t *b = calloc(1, big);
    shm_set(b, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN, OFF, 5000);
    app_put(b, RTSS_ENTRY_MIN, OFF, 4500, PID, 1000, 2000, 60);
    rtss_parse(b, big, PID, &a);
    CHECK(a.running && !a.listed, "an entry past %d is not looked at", RTSS_APPS_MAX);
    app_put(b, RTSS_ENTRY_MIN, OFF, 4095, PID, 1000, 2000, 60);
    rtss_parse(b, big, PID, &a);
    CHECK(a.listed, "entry %d is", RTSS_APPS_MAX - 1);
    free(b);
}

// ---- a fake profile behind RtssOps, and one poll as the worker makes it
typedef struct {
    int readonly, no_library, loaded;
    int64_t refuse_above;   // RTSS refuses a limit above this (millihertz); 0 none
    int64_t holds;          // the limit the profile holds from this run; -1 none
    int applies, firsts, clears, put_backs, last_put_back_why;
} Store;
static int f_apply(void *ctx, int64_t mhz, int first, int clear) {
    Store *st = ctx;
    st->applies++, st->firsts += first;
    if (st->readonly) return RTSS_WROTE_READONLY;
    if (st->refuse_above && mhz > st->refuse_above) {
        if (!clear) return RTSS_WROTE_REFUSED;
        st->clears++, st->holds = 0;
        return RTSS_WROTE_REFUSED_CLEARED;
    }
    st->holds = mhz;
    return RTSS_WROTE_HELD;
}
static void f_put_back(void *ctx, const RtssOut *o) {
    Store *st = ctx;
    st->put_backs++, st->holds = -1, st->last_put_back_why = o->why;
}

typedef struct {
    RtssState s;
    Store st;
    RtssOps ops;
    int running, listed;
    uint32_t t0, t1, frames;
} Run;
static void run_init(Run *r) {
    memset(r, 0, sizeof *r);
    r->s = (RtssState)RTSS_STATE_INIT;
    r->st.holds = -1;
    r->ops = (RtssOps){&r->st, f_apply, f_put_back};
    r->running = r->listed = 1, r->t0 = 1000, r->t1 = 2000, r->frames = 60;
}
// One poll for want, as rtss_limit.c worker makes it: the hooks library loads on the first poll that wants it and stays.
static RtssOut step_ex(Run *r, int64_t want, int current, int closed) {
    RtssIn in = {.want = want, .current = current, .closed = closed};
    in.app = (RtssApp){r->running, r->running && r->listed, r->t0, r->t1, r->frames};
    if (r->st.loaded) in.hooks = 1;
    else if (rtss_wants_hooks(&r->s, &in)) in.hooks = r->st.no_library ? 0 : (r->st.loaded = 1);
    else in.hooks = -1;
    return rtss_poll(&r->s, &in, &r->ops);
}
static RtssOut step(Run *r, int64_t want) { return step_ex(r, want, 1, 0); }

// RTSS's injection delay: it runs but has not hooked the game for 30 polls. The limit goes into the profile at the
// first poll, and RTSS holds the game once it lists it, with no second write.
static void injection_delay(void) {
    Run r;
    run_init(&r);
    r.listed = 0;
    RtssOut o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_NOT_LISTED, "written at once, not held yet");
    CHECK(r.st.firsts == 1 && r.s.applied == 60000 && r.s.changed, "the first write of the run");
    for (int i = 0; i < 30; i++) o = step(&r, 60000);
    CHECK(r.st.applies == 1 && !o.holds && o.why == RTSS_WHY_NOT_LISTED, "no write while not listed");
    r.listed = 1;
    o = step(&r, 60000);
    CHECK(o.holds && o.why == RTSS_WHY_HOLDS && r.st.applies == 1, "held once listed, no second write");
}

// RTSS quits while it limits, and comes back: the profile stays for its return.
static void quit(void) {
    Run r;
    run_init(&r);
    step(&r, 60000);
    r.running = 0;
    RtssOut o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_RUNNING && o.act == RTSS_DO_NOTHING, "not running: the game limits");
    CHECK(!r.st.put_backs && r.st.holds == 60000, "the profile stays");
    r.running = 1;
    o = step(&r, 60000);
    CHECK(o.holds && r.st.applies == 1, "back: held with no new write");
}

// Profiles under Program Files without administrator rights: out for the run.
static void readonly(void) {
    Run r;
    run_init(&r);
    r.st.readonly = 1;
    RtssOut o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_READONLY, "failed");
    CHECK(!r.s.changed, "nothing of the run in the profile");
    for (int i = 0; i < 20; i++) o = step(&r, i < 10 ? 60000 : 144000);
    CHECK(r.st.applies == 1 && o.why == RTSS_WHY_FAILED, "no write after it");
    char t[128];
    rtss_why_text(t, sizeof t, &o, r.s.fail);
    CHECK(!strcmp(t, "RivaTuner Statistics Server's profile cannot be changed"), "%s", t);
}

// A limit out of RTSS's range: refused and kept by the game; the earlier limit of this run is cleared to none; a limit
// in range goes to RTSS at once.
static void refused(void) {
    Run r;
    run_init(&r);
    r.st.refuse_above = 200000;
    step(&r, 60000);
    RtssOut o = step(&r, 240000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_REFUSED, "refused");
    CHECK(r.s.refused == 240000 && r.s.applied == 0 && r.st.clears == 1 && r.st.holds == 0, "the 60 cleared to none");
    for (int i = 0; i < 5; i++) o = step(&r, 240000);
    CHECK(r.st.applies == 2 && o.why == RTSS_WHY_REFUSED && !o.holds, "not written again");
    o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_APPLY && o.holds && r.st.holds == 144000, "144 at once");
    CHECK(r.st.firsts == 1, "one first write in the run (%d)", r.st.firsts);
    o = step(&r, 240000);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_REFUSED, "the refused limit stays with the game");
    // refused as the run's first write: the profile is the run's from then on
    run_init(&r);
    r.st.refuse_above = 200000;
    o = step(&r, 240000);
    CHECK(o.why == RTSS_WHY_REFUSED && r.s.changed && r.st.clears == 1, "a first refusal clears too, as it stands");
}

// RTSS's own frame count: more than 1.2 times the limit for six polls in a row on an unchanged limit, and the
// profile goes back for the run. The poll that writes the limit does not count; 1.15 times is not caught; a period
// RTSS no longer moves still counts; no limit and not listed never do.
static void frame_count(void) {
    Run r;
    run_init(&r);
    r.frames = 144;
    RtssOut o = step(&r, 60000);
    for (int i = 0; i < 5; i++) o = step(&r, 60000);
    CHECK(o.holds && !r.s.fail && r.s.over == 5, "five polls over: still held");
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_PUT_BACK && !o.holds && o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_NOT_LIMITING,
          "the sixth gives up");
    CHECK(o.fps == 144.0 && r.st.put_backs == 1 && r.st.last_put_back_why == RTSS_WHY_FAILED && !r.s.changed,
          "put back, with the measured rate");
    o = step(&r, 30000);
    CHECK(o.why == RTSS_WHY_FAILED && r.st.applies == 1, "out for the run");
    run_init(&r);
    r.frames = 69;
    for (int i = 0; i < 40; i++) o = step(&r, 60000);
    CHECK(o.holds && !r.s.fail, "69 fps at 60 is not caught");
    r.frames = 73;
    for (int i = 0; i < 6; i++) o = step(&r, 60000);
    CHECK(r.s.fail == RTSS_FAIL_NOT_LIMITING, "73 fps at 60 is");
    run_init(&r);
    r.frames = 72;   // 1.2 times the limit exactly
    for (int i = 0; i < 40; i++) o = step(&r, 60000);
    CHECK(o.holds && !r.s.fail, "72 fps at 60 is not");
    run_init(&r);
    r.frames = 700;
    for (int i = 0; i < 20; i++) o = step(&r, 0);
    CHECK(o.holds && !r.s.fail, "no limit: nothing to check");
    run_init(&r);
    r.frames = 144, r.listed = 0;
    for (int i = 0; i < 20; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.s.over, "not listed: no count");
    r.listed = 1;
    for (int i = 0; i < 3; i++) step(&r, 60000);
    r.listed = 0;
    step(&r, 60000);
    CHECK(r.s.over == 0, "a poll not listed starts the count again");
    run_init(&r);
    r.frames = 144;   // t0 and t1 never move: the same period every poll
    for (int i = 0; i < 7; i++) step(&r, 60000);
    CHECK(r.s.fail == RTSS_FAIL_NOT_LIMITING, "a period RTSS no longer moves still counts, as it stands");
    // a want that changed while the poll ran: nothing decided, the count waits
    run_init(&r);
    r.frames = 144;
    step(&r, 60000);
    step(&r, 60000);
    o = step_ex(&r, 60000, 0, 0);
    CHECK(o.why == RTSS_WHY_NEWER && !o.holds && o.act == RTSS_DO_NOTHING && r.s.over == 1, "a newer want: no count");
}

// Kept out and let in again: the first kept-out poll puts back what the run wrote, once; a limit afterwards is
// written at once and is the run's first write again.
static void kept_out(void) {
    Run r;
    run_init(&r);
    step(&r, 60000);
    RtssOut o = step(&r, RTSS_OUT_FRAMEGEN);
    CHECK(o.act == RTSS_DO_PUT_BACK && o.why == RTSS_WHY_OUT && !o.holds && r.st.holds == -1, "put back");
    CHECK(r.st.last_put_back_why == RTSS_WHY_OUT, "the put back says why");
    o = step(&r, RTSS_OUT_FRAMEGEN);
    CHECK(o.act == RTSS_DO_NOTHING && r.st.put_backs == 1, "once");
    o = step(&r, RTSS_OUT_LOWLAT);
    CHECK(o.act == RTSS_DO_NOTHING && o.mhz == RTSS_OUT_LOWLAT, "a second reason: nothing more");
    char t[128];
    rtss_why_text(t, sizeof t, &o, r.s.fail);
    CHECK(!strcmp(t, "RivaTuner Statistics Server kept out: the driver's low latency"), "%s", t);
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && o.holds && r.st.firsts == 2, "written at once, the run's first write again");
    // kept out from the start: the hooks library is never wanted, and nothing is written or put back
    run_init(&r);
    for (int i = 0; i < 10; i++) o = step(&r, RTSS_OUT_SETTING);
    CHECK(!r.st.loaded && !r.st.applies && !r.st.put_backs && o.why == RTSS_WHY_OUT, "read only");
}

// Low latency switching on and off at every poll: each switch writes or puts back, as it stands.
static void churn(void) {
    Run r;
    run_init(&r);
    step(&r, 60000);
    for (int i = 0; i < 20; i++) step(&r, i & 1 ? 60000 : RTSS_OUT_LOWLAT);
    CHECK(r.st.applies == 11 && r.st.put_backs == 10, "%d writes, %d put backs", r.st.applies, r.st.put_backs);
}

// A movie under RTSS: the want follows the movie's rate, and each start and end is a write.
static void movie(void) {
    Run r;
    run_init(&r);
    step(&r, 144000);
    RtssOut o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && o.holds, "the movie's 60 written");
    o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_APPLY && o.holds && r.st.applies == 3, "144 written again at its end");
}

// No want yet, and a closed run: nothing at all. The hooks library that does not load fails the run.
static void closed_and_library(void) {
    Run r;
    run_init(&r);
    RtssOut o = step(&r, RTSS_UNSET);
    CHECK(o.why == RTSS_WHY_UNSET && o.act == RTSS_DO_NOTHING && !r.st.loaded, "no want");
    step(&r, 60000);
    o = step_ex(&r, 144000, 1, 1);
    CHECK(o.why == RTSS_WHY_CLOSED && o.act == RTSS_DO_NOTHING && r.st.holds == 60000, "closed: nothing written");
    o = step_ex(&r, RTSS_OUT_FRAMEGEN, 1, 1);
    CHECK(o.act == RTSS_DO_NOTHING && !r.st.put_backs, "closed: nothing put back");
    run_init(&r);
    r.st.no_library = 1;
    o = step(&r, 60000);
    CHECK(o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_LIBRARY && !r.st.applies, "the library did not load");
    char t[128];
    rtss_why_text(t, sizeof t, &o, r.s.fail);
    CHECK(!strcmp(t, "RivaTuner Statistics Server's hooks library did not load"), "%s", t);
    r.st.no_library = 0;
    o = step(&r, 60000);
    CHECK(o.why == RTSS_WHY_FAILED && !r.st.loaded, "not tried again");
    // not running: the library is not wanted until RTSS runs
    run_init(&r);
    r.running = 0;
    o = step(&r, 60000);
    CHECK(!r.st.loaded && o.why == RTSS_WHY_NOT_RUNNING, "not loaded while RTSS does not run");
    rtss_why_text(t, sizeof t, &o, r.s.fail);
    CHECK(!strcmp(t, "RivaTuner Statistics Server is not running"), "%s", t);
}

// Two limiters: RTSS out of the game's limit (kept out, or a profile that cannot be written or a hooks library that did
// not load), listed and a limit of RTSS's own; never for a limit the game hands RTSS.
static void two(void) {
    CHECK(rtss_two(RTSS_OUT_SETTING, 0, 1, 1) && rtss_two(RTSS_OUT_FRAMEGEN, 0, 1, 1) &&
              rtss_two(RTSS_OUT_LOWLAT, 0, 1, 1),
          "every reason");
    CHECK(!rtss_two(RTSS_OUT_SETTING, 0, 0, 1), "not listed: RTSS holds nothing");
    CHECK(!rtss_two(RTSS_OUT_SETTING, 0, 1, 0), "no limit of its own");
    CHECK(!rtss_two(60000, 0, 1, 1) && !rtss_two(0, 0, 1, 1), "the game's limit in RTSS: one limiter");
    CHECK(!rtss_two(RTSS_UNSET, 0, 1, 1) && !rtss_two(RTSS_UNSET, RTSS_FAIL_READONLY, 1, 1), "no want yet");
    CHECK(rtss_two(60000, RTSS_FAIL_READONLY, 1, 1) && rtss_two(0, RTSS_FAIL_LIBRARY, 1, 1),
          "out for the run: the profile cannot be written, or the hooks library did not load");
    CHECK(!rtss_two(60000, RTSS_FAIL_READONLY, 0, 1) && !rtss_two(60000, RTSS_FAIL_LIBRARY, 1, 0),
          "out for the run, but not listed or no limit of its own");
    CHECK(!rtss_two(60000, RTSS_FAIL_NOT_LIMITING, 1, 1), "RTSS seen not to hold the game: not counted");
}

int main(void) {
    settings();
    reasons();
    parse();
    injection_delay();
    quit();
    readonly();
    refused();
    frame_count();
    kept_out();
    churn();
    movie();
    closed_and_library();
    two();
    printf("rtss_limit_check: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
