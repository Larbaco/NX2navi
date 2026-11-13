// runtime/perf_census.h: the part of a frame an address-space walk overlapped moves to the frame's census part, from
// its replay part first and then from its unattributed part, never more than the overlap and never past the frame.
// Built and run by tests/census_charge_check.py; prints each failed check and exits 1 on any failure.
#include <stdio.h>
#include <stdlib.h>

#include "perf_census.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000000ull

typedef struct { uint64_t replay, other, census, moved; } Parts;

// A frame of d ms ending at now ms, with replay / unattributed / census parts as given, and a walk w0..w1 ms (0: none
// seen; w1 0: still running).
static Parts charge(uint64_t now, uint64_t d, uint64_t w0, uint64_t w1, uint64_t replay, uint64_t other,
                    uint64_t census) {
    Parts p = {replay * MS, other * MS, census * MS, 0};
    p.moved = census_charge(now * MS, d * MS, w0 * MS, w1 * MS, &p.replay, &p.other, &p.census);
    return p;
}

int main(void) {
    // no walk seen, and a walk that ended before the frame or started after it
    Parts p = charge(1000, 40, 0, 0, 30, 10, 0);
    CHECK(p.moved == 0 && p.replay == 30 * MS && p.other == 10 * MS, "no walk: moved %llu", (unsigned long long)p.moved);
    p = charge(1000, 40, 800, 950, 30, 10, 0);
    CHECK(p.moved == 0 && p.census == 0, "a walk before the frame: moved %llu", (unsigned long long)p.moved);
    p = charge(1000, 40, 1000, 0, 30, 10, 0);
    CHECK(p.moved == 0, "a walk that starts as the frame ends: moved %llu", (unsigned long long)p.moved);
    // a walk that ended 10 ms into the frame: 10 ms from the replay
    p = charge(1000, 40, 900, 970, 30, 10, 0);
    CHECK(p.moved == 10 * MS && p.replay == 20 * MS && p.other == 10 * MS && p.census == 10 * MS,
          "partial overlap: replay %llu other %llu census %llu", (unsigned long long)p.replay, (unsigned long long)p.other,
          (unsigned long long)p.census);
    // a walk inside the frame, longer than the replay part: the replay's 15 ms, then 10 ms of the 20 unattributed
    p = charge(1000, 60, 950, 975, 15, 20, 0);
    CHECK(p.moved == 25 * MS && p.replay == 0 && p.other == 10 * MS && p.census == 25 * MS,
          "a walk inside the frame: replay %llu other %llu census %llu", (unsigned long long)p.replay,
          (unsigned long long)p.other, (unsigned long long)p.census);
    // a walk still running (w1 0, or the end of the walk before it): it reaches now
    p = charge(1000, 40, 985, 0, 30, 10, 0);
    CHECK(p.moved == 15 * MS && p.replay == 15 * MS, "a running walk: moved %llu", (unsigned long long)p.moved);
    p = charge(1000, 40, 985, 700, 30, 10, 0);
    CHECK(p.moved == 15 * MS && p.replay == 15 * MS, "a running walk with the last one's end: moved %llu",
          (unsigned long long)p.moved);
    // the overlap caps the charge; the parts cap it too (nothing taken from the other parts of the frame)
    p = charge(1000, 100, 900, 0, 5, 3, 0);
    CHECK(p.moved == 8 * MS && p.replay == 0 && p.other == 0 && p.census == 8 * MS, "parts smaller than the overlap: "
          "moved %llu", (unsigned long long)p.moved);
    // never past the frame: census_check's own walk already took 35 of its 40 ms
    p = charge(1000, 40, 900, 0, 30, 10, 35);
    CHECK(p.moved == 5 * MS && p.census == 40 * MS && p.replay == 25 * MS, "census at the frame's length: moved %llu, "
          "census %llu", (unsigned long long)p.moved, (unsigned long long)p.census);
    // random frames: the census part never passes the frame, the moved time never passes the overlap, and the sum of
    // the three parts is kept
    srand(7);
    for (int i = 0; i < 100000; i++) {
        uint64_t now = 1000 + (uint64_t)(rand() % 1000), d = 1 + (uint64_t)(rand() % 300);
        uint64_t w0 = (uint64_t)(rand() % 2000), w1 = rand() % 3 ? w0 + (uint64_t)(rand() % 400) : 0;
        uint64_t census = (uint64_t)(rand() % (int)d), replay = (uint64_t)(rand() % 300), other = (uint64_t)(rand() % 300);
        p = charge(now, d, w0, w1, replay, other, census);
        uint64_t lo = now - d > w0 ? now - d : w0, hi = w1 && w1 < now ? w1 : now;
        uint64_t ov = w0 && hi > lo ? (hi - lo) * MS : 0;
        int ok = p.census <= d * MS && p.moved <= ov && p.replay + p.other + p.census == (replay + other + census) * MS &&
                 p.census == census * MS + p.moved && (p.other == other * MS || p.replay == 0);
        CHECK(ok, "case %d: now %llu d %llu walk %llu-%llu: moved %llu of overlap %llu, census %llu", i,
              (unsigned long long)now, (unsigned long long)d, (unsigned long long)w0, (unsigned long long)w1,
              (unsigned long long)p.moved, (unsigned long long)ov, (unsigned long long)p.census);
        if (!ok) break;
    }
    if (fails) return 1;
    printf("census_charge_check: all checks passed\n");
    return 0;
}
