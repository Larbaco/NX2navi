// tests/frame_owed_check.py builds this: runtime/vk_frame_owed.h, a frame's bookkeeping in replay order, with one
// presenting thread and three replaying threads that hold the lock for their replays, a few of them for HOLD_MS. Every
// frame's bookkeeping must run once and in frame order, never during a replay, and before the body of any replay that
// took the lock after the frame was presented; the presenting thread must never wait for a replay.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "vk_frame_owed.h"

#define FRAMES 20000
#define REPLAYERS 3
#define HOLD_MS 40
#define PRESENT_MAX_NS 20000000ull   // a present that waited for a long hold would take up to HOLD_MS

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("frame_owed_check: FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static PlatMutex sub_lock = PLAT_MUTEX_INIT;
static FrameOwed FO;
static volatile int32_t in_replay, in_long, stop;
static volatile uint64_t presented, ends, bad_order, during_replay, late, ran_here, long_holds, during_long;
static uint64_t last_end, present_max_ns;

// vk_backend.c frame_end's stand-in, run with the lock held.
static void frame_end(uint64_t frame, void *arg) {
    (void)arg;
    if (__atomic_load_n(&in_replay, __ATOMIC_ACQUIRE)) __atomic_add_fetch(&during_replay, 1, __ATOMIC_RELAXED);
    if (frame != last_end + 1) __atomic_add_fetch(&bad_order, 1, __ATOMIC_RELAXED);
    last_end = frame;
    __atomic_add_fetch(&ends, 1, __ATOMIC_RELAXED);
    for (volatile int i = 0; i < 300; i++) {}
}

// vk_sync.c vk_sync_try_locked with vk_backend.c fo_here
static int try_run(void *arg) {
    (void)arg;
    if (!plat_mutex_trylock(&sub_lock)) return 0;
    fo_run(&FO, frame_end, NULL);
    plat_mutex_unlock(&sub_lock);
    return 1;
}

// vk_sync.c submit: the lock, the owed frames (vk_present_frames_owed), then the replay's body
static void replayer(void *arg) {
    uint64_t r = (uint64_t)(uintptr_t)arg * 0x2545F4914F6CDD1Dull + 3;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        r ^= r << 13, r ^= r >> 7, r ^= r << 17;
        uint64_t p0 = __atomic_load_n(&presented, __ATOMIC_ACQUIRE);
        plat_mutex_lock(&sub_lock);
        if (fo_waiting(&FO)) fo_run(&FO, frame_end, NULL);
        if (FO.done < p0) __atomic_add_fetch(&late, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&in_replay, 1, __ATOMIC_RELEASE);
        if (r % 997 == 0) {
            __atomic_store_n(&in_long, 1, __ATOMIC_RELEASE);
            plat_sleep_ms(HOLD_MS);
            __atomic_store_n(&in_long, 0, __ATOMIC_RELEASE);
            __atomic_add_fetch(&long_holds, 1, __ATOMIC_RELAXED);
        } else {
            for (volatile uint32_t i = 0; i < r % 20000; i++) {}
        }
        __atomic_store_n(&in_replay, 0, __ATOMIC_RELEASE);
        plat_mutex_unlock(&sub_lock);
        for (volatile uint32_t i = 0; i < r % 5000; i++) {}
    }
}

int main(void) {
    // one thread first: the present runs its own frame's bookkeeping when the lock is free, and two presents with the
    // lock held elsewhere leave both frames to the next holder, which runs them in order
    int here = fo_present(&FO, 1, try_run, NULL);
    CHECK(here && ends == 1 && FO.done == 1, "a present with the lock free did not run its own bookkeeping");
    plat_mutex_lock(&sub_lock);
    int a = fo_present(&FO, 2, try_run, NULL), b = fo_present(&FO, 3, try_run, NULL);
    CHECK(!a && !b && ends == 1 && fo_waiting(&FO), "presents past a held lock ran or lost their bookkeeping");
    CHECK(fo_run(&FO, frame_end, NULL) == 2 && ends == 3 && !bad_order && !fo_waiting(&FO),
          "the next holder did not catch up both frames in order");
    CHECK(fo_present(&FO, 2, try_run, NULL) == 0 && ends == 3 && !fo_waiting(&FO), "an older frame number ran again");
    plat_mutex_unlock(&sub_lock);
    last_end = 3, ends = 0;

    PlatThread *t[REPLAYERS];
    for (int i = 0; i < REPLAYERS; i++) t[i] = plat_thread_new(replayer, (void *)(uintptr_t)(i + 1), "replay");
    for (uint64_t f = 4; f < 4 + FRAMES; f++) {
        int lng = __atomic_load_n(&in_long, __ATOMIC_ACQUIRE);
        uint64_t t0 = plat_now_ns();
        if (fo_present(&FO, f, try_run, NULL)) ran_here++;
        uint64_t d = plat_now_ns() - t0;
        if (d > present_max_ns) present_max_ns = d;
        if (lng) during_long++;
        __atomic_store_n(&presented, f, __ATOMIC_RELEASE);
        for (volatile int i = 0; i < 20000; i++) {}
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < REPLAYERS; i++) plat_thread_join(t[i]);
    plat_mutex_lock(&sub_lock);
    fo_run(&FO, frame_end, NULL);
    plat_mutex_unlock(&sub_lock);
    CHECK(ends == FRAMES, "%llu frame ends for %d frames", (unsigned long long)ends, FRAMES);
    CHECK(!bad_order, "%llu frame ends out of order", (unsigned long long)bad_order);
    CHECK(!during_replay, "%llu frame ends ran during a replay", (unsigned long long)during_replay);
    CHECK(!late, "%llu replays began before the bookkeeping of a frame presented earlier", (unsigned long long)late);
    CHECK(present_max_ns < PRESENT_MAX_NS, "a present took %.2f ms", present_max_ns / 1e6);
    CHECK(ran_here < FRAMES, "no present found the lock held, so nothing was owed");
    printf("frame_owed_check: %d frames: %llu ends ran at the present, %llu owed to a replay; %llu replays held the lock "
           "%d ms, %llu presents came during one; the longest present %.3f ms; %s\n", FRAMES,
           (unsigned long long)ran_here, (unsigned long long)(FRAMES - ran_here), (unsigned long long)long_holds,
           HOLD_MS, (unsigned long long)during_long, present_max_ns / 1e6, fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
