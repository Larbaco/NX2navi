// A frame's bookkeeping at its present (vk_backend.c frame_end: the replay's per-frame state that a present closes), in
// replay order. Header only, so that tests/frame_owed_check.c checks it without a device.
//
// The replay reads and writes that state as it records, under the submission lock (vk_sync.c sub_lock). With the
// replay on the submitting threads the game presents on one thread while another may be replaying, and running the
// bookkeeping then reset the state in the middle of that replay. The presenting thread does not wait for the replay:
// it marks its frame owed and runs the bookkeeping itself only when it gets the lock at once; otherwise the next thread
// to take the lock (vk_sync.c submit) runs every owed frame's bookkeeping, in frame order, before its own replay. A frame
// caught up behind a later present runs as two presents with no submission between them always did.
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t owed;   // the newest presented frame (atomic: the presenting thread raises it, the lock's holder reads it)
    uint64_t done;   // the newest frame whose bookkeeping ran (the lock's holder only)
} FrameOwed;

typedef void (*FrameEnd)(uint64_t frame, void *arg);

// The lock's holder: whether a frame waits for its bookkeeping (one atomic load, for every submission).
static inline int fo_waiting(FrameOwed *o) { return __atomic_load_n(&o->owed, __ATOMIC_ACQUIRE) != o->done; }

// The lock's holder: the bookkeeping of every frame presented since the last run, in frame order; returns how many.
static inline uint64_t fo_run(FrameOwed *o, FrameEnd end, void *arg) {
    uint64_t owed = __atomic_load_n(&o->owed, __ATOMIC_ACQUIRE), n = 0;
    while (o->done < owed) end(++o->done, arg), n++;
    return n;
}

// The presenting thread, frame numbers rising (a smaller one than owed leaves it: that frame's bookkeeping ran or is
// owed already). try_run(arg) takes the lock without waiting, runs fo_run under it and returns 1, or returns 0 when
// another thread holds it. Returns whether the bookkeeping ran here.
static inline int fo_present(FrameOwed *o, uint64_t frame, int (*try_run)(void *), void *arg) {
    uint64_t was = __atomic_load_n(&o->owed, __ATOMIC_RELAXED);
    while (was < frame && !__atomic_compare_exchange_n(&o->owed, &was, frame, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    return try_run(arg);
}
