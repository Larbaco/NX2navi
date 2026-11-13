// runtime/vk_memory.c's page write generations (the section from its "page write generations" banner to its "dirty
// census" banner, extracted by tests/gen_wrap_check.py into gen_section.inc): the lines gen_next logs as the 32-bit
// generations near their wrap, and what the comparisons answer across it. Prints each failed check and exits 1 on any.
// Stubs: no write-watch (mem_written_pages reports no page), nothing frozen, no pools.
//
// The comparisons across the wrap are wrong today (AUDIT F-8-01: the generations have no wrap handling; its fix waits
// for approval). The check expects the four wrong answers and says so: the change that handles the wrap turns them
// into failures here, and flips the expectations with it.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_ADDRESS_LIMIT (1ull << 39)
typedef struct { int x; } PlatMutex;
#define PLAT_MUTEX_INIT {0}
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static void plat_mutex_lock_shared(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; }
static uint64_t qpc(void) { return 0; }
static int rt_trace;
#define RT_TRACE_PERF 1
static void plat_add64(volatile int64_t *p, int64_t v) { *p += v; }
static int64_t plat_xchg64(volatile int64_t *p, int64_t v) { int64_t o = *p; *p = v; return o; }
static struct { uint64_t frames; } gfx_counters;
static uint64_t fake_now, rt_guest_start_ns;
static uint64_t kern_now_ns(void) { return fake_now; }
static void grow(void **arr, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 64;
    *arr = realloc(*arr, (size_t)n * elem);
    *cap = n;
}
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    (void)addr, (void)size, (void)pages, (void)max;
    return 0;
}
int mem_frozen(uint64_t a) { (void)a; return 0; }
int mem_watched(uint64_t addr, uint64_t size) { (void)addr, (void)size; return 1; }
int nvn_gpu_only(uint64_t a) { (void)a; return 0; }
static const char *cfg_get(const char *k) { (void)k; return NULL; }
typedef struct { uintptr_t lo, hi, base; void *buf; } pidx_ent;   // snap_widen's pool lookup: no pools
typedef struct { int n; } pidx;
static pidx pool_index;
static PlatMutex pool_lock;
static const pidx_ent *pidx_find(const pidx *x, uintptr_t a, uint64_t l) { (void)x, (void)a, (void)l; return NULL; }
enum { POOL_IMPORT = 1 };
typedef struct { int kind; void *buf; uint64_t import_lo, import_hi; } PoolRec;

// The log: the generation lines are kept, the others dropped.
static char gen_lines[16][512];
static int n_gen_lines;
static void rt_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strncmp(line, "mem: page generations", 21)) return;
    if (n_gen_lines < 16) memcpy(gen_lines[n_gen_lines], line, sizeof line);
    n_gen_lines++;
}
static void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(70);
}

#include "gen_section.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define TEX 0x100000000ull     // a texture of 4 pages in one 2 MiB region
#define OTHER 0x100200000ull   // a second one in the next region

// Hands out generations with vk_guest_written from just below `to` until gen_now reaches `to` + 3.
static void walk_to(uint32_t to) {
    gen_now = to - 4;
    for (int i = 0; i < 7; i++) vk_guest_written(0x300000000ull, 0x1000);
}

int main(void) {
    // ---- the lines: one at each of 0xC0000000, 0xD0000000, 0xE0000000, 0xF0000000 and at the wrap, no other
    rt_guest_start_ns = 1000000000ull;
    fake_now = rt_guest_start_ns + 3600ull * 1000000000ull;   // an hour in
    gfx_counters.frames = 216000;                             // at 60 fps
    walk_to(0x10000000u);   // below the first mark: nothing
    walk_to(0xb0000000u);
    CHECK(n_gen_lines == 0, "%d lines before 0xC0000000", n_gen_lines);
    static const uint32_t marks[] = {0xc0000000u, 0xd0000000u, 0xe0000000u, 0xf0000000u};
    for (int k = 0; k < 4; k++) {
        int was = n_gen_lines;
        walk_to(marks[k]);
        char want[32];
        snprintf(want, sizeof want, ": %u of 4294967295", marks[k]);
        CHECK(n_gen_lines == was + 1 && strstr(gen_lines[was], want), "the mark at %#x gave %d lines (%s)", marks[k],
              n_gen_lines - was, n_gen_lines > was ? gen_lines[was] : "none");
    }
    // 0xC0000000 in an hour of 216,000 frames: about 14,913 a frame, and a quarter of the range left: 20 min
    CHECK(n_gen_lines >= 1 && strstr(gen_lines[0], "(14913 a frame)") && strstr(gen_lines[0], "in about 0.3 h"),
          "the first line's rate or hours: %s", n_gen_lines ? gen_lines[0] : "none");
    int was = n_gen_lines;
    walk_to(0);   // across the wrap
    CHECK(n_gen_lines == was + 1 && strstr(gen_lines[was], "ran out at frame 216000 after 1.0 h"),
          "the wrap gave %d lines (%s)", n_gen_lines - was, n_gen_lines > was ? gen_lines[was] : "none");
    walk_to(0x20000000u);
    CHECK(n_gen_lines == 5, "%d lines in all, not 5", n_gen_lines);
    for (int i = 0; i < n_gen_lines && i < 16; i++) printf("  %s\n", gen_lines[i]);

    // ---- the comparisons across the wrap: the four answers known to be wrong (expected, see the top)
    int known = 0;
    gen_now = 999;
    vk_guest_written(TEX, 0x4000);
    vk_guest_written(OTHER, 0x4000);
    uint32_t w1, w2, now;
    vk_mem_changed(TEX, 0x4000, 0, &w1);
    vk_mem_changed(OTHER, 0x4000, 0, &w2);
    gen_now = UINT32_MAX - 6;
    CHECK(vk_mem_changed(TEX, 0x4000, w1, &w1) == 0, "an unwritten texture read as written before the wrap");
    vk_mem_changed(OTHER, 0x4000, w2, &w2);
    for (int i = 0; i < 10; i++) vk_mem_changed(0x200000000ull, 0x1000, 0, &now);   // other checks use generations
    vk_guest_written(TEX, 0x1000);   // the CPU writes the texture after the wrap
    known += vk_mem_changed(TEX, 0x4000, w1, &now) != 1;   // 1. the write is missed
    vk_mem_changed(OTHER, 0x4000, w2, &w2);
    known += vk_mem_changed(OTHER, 0x4000, w2, &now) == 1;   // 2. an unwritten texture reads as written at every check
    w2 = now;
    known += vk_mem_changed_snap(OTHER, 0x4000, w2, 7, &now) == 1;   // 3. the same through the snapshot path
    gen_now = UINT32_MAX;
    vk_guest_written(TEX + 0x2000, 0x1000);   // 4. a write stamped at the wrap gets 0, which reads as never stamped
    known += vk_mem_changed(TEX + 0x2000, 0x1000, 0, &now) != 1;
    CHECK(known == 4, "%d of the 4 known wrong answers across the wrap remain: if the wrap is handled now, flip these "
          "expectations", known);
    printf("gen_wrap_check: across the wrap %d of 4 comparisons still answer wrong (known, expected until the wrap "
           "is handled)\n", known);

    if (fails) return printf("gen_wrap_check: %d checks failed\n", fails), 1;
    printf("gen_wrap_check: all checks passed\n");
    return 0;
}
