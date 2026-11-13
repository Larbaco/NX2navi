// runtime/vk_memory.c's page write generations across a boundary: the section from its "page write generations" banner
// to its "dirty census" banner and the readers of its tables outside it (cpu_wrote_since, shadow_pool_clean,
// lazy_clean, page_dirty), extracted by tests/gen_wrap_check.py into gen_section.inc, start just below a seed's
// boundary and run across it. Every compare the texture checks, the snapshot path, the VRAM mirrors' lazy fetch, the
// shadow storage and the verify mode make must answer as it does below the boundary. Seeds: 2^32 - 8 (where 32-bit
// generations wrapped), 2^40 - 8 (the low 32 bits pass through 0 again, so a truncation to 32 bits anywhere fails
// there too) and 2^63 - 8. gen_next must log one line when the count reaches 2^32, and no other.
// Stubs: no-op locks; a write watch that reports the pages a check marks as written by the CPU; nothing frozen; no
// pools in the index.
// Usage: gen_wrap_check <seed>. Prints each failed check and "gen_wrap_check: seed ...: N of M checks failed"; exit 1
// on any.
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
    if (!*arr) exit(70);
    *cap = n;
}
#define SB_PAGE 4096ull

// The write watch: pages the CPU wrote since the last query over them (mem_written_pages resets them,
// mem_written_peek does not).
static uint64_t ww_pend[64];
static int nww_pend;
static void cpu_write(uint64_t page) { ww_pend[nww_pend++] = page & ~0xfffull; }
static uint64_t ww_take(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, int reset) {
    uint64_t n = 0;
    for (int i = 0; i < nww_pend && n < max;) {
        if (ww_pend[i] >= addr && ww_pend[i] < addr + size) {
            if (pages) pages[n] = ww_pend[i];
            n++;
            if (reset) {
                ww_pend[i] = ww_pend[--nww_pend];
                continue;
            }
        }
        i++;
    }
    return n;
}
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return ww_take(addr, size, pages, max, 1);
}
uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return ww_take(addr, size, pages, max, 0);
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

// The log: the page generation lines are kept, the others dropped.
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

typedef __typeof__(gen_now) gen_t;   // the width the source gives the generations

static int checks, failed;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(cond)) failed++, printf("FAIL line %d: ", __LINE__), printf(__VA_ARGS__), printf("\n");                 \
    } while (0)

#define TEX 0x100000000ull     // a texture of 4 pages in one 2 MiB region
#define OTHER 0x100200000ull   // a second one in the next region
#define SPIN 0x200000000ull    // an unrelated range other checks hand out generations for
#define LONE 0x100400000ull    // a page written exactly at the boundary

static uint64_t last_now;      // every *now handed back, in order: must grow
static void seen(gen_t now, const char *what) {
    CHECK((uint64_t)now > last_now, "%s: generation %llu after %llu: not newer", what, (unsigned long long)now,
          (unsigned long long)last_now);
    last_now = now;
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 0) : (1ull << 32) - 8;
    uint64_t edge = seed + 8;   // the boundary under test: 2^32 for the first seed
    gen_t w_tex, w_oth, now, d;
    rt_guest_start_ns = 1000000000ull;
    fake_now = rt_guest_start_ns + 3600ull * 1000000000ull;   // an hour in
    gfx_counters.frames = 216000;                             // at 60 fps
    gen_now = (gen_t)seed;
    CHECK((uint64_t)gen_now == seed, "the seed %#llx does not fit the generations (%u bits)", (unsigned long long)seed,
          (unsigned)(8 * sizeof(gen_t)));
    if (failed) {   // the generations cannot reach the boundary: nothing below can run
        printf("gen_wrap_check: seed %#llx: 1 of 1 checks failed\n", (unsigned long long)seed);
        return 1;
    }
    last_now = (uint64_t)gen_now;

    // Before the boundary: both textures loaded (written) and checked.
    vk_guest_written(TEX, 0x4000);
    vk_guest_written(OTHER, 0x4000);
    CHECK(vk_mem_changed(TEX, 0x4000, 0, &w_tex) == 1, "TEX: its load is not seen");
    seen(w_tex, "TEX first check");
    CHECK(vk_mem_changed(OTHER, 0x4000, 0, &w_oth) == 1, "OTHER: its load is not seen");
    seen(w_oth, "OTHER first check");
    uint64_t pre = (uint64_t)gen_now;   // a generation before the boundary: what a mirror or shadow last synced to

    // Other checks hand out generations up to edge - 1; the page written next is stamped exactly at edge.
    while ((uint64_t)gen_now + 1 < edge) {
        vk_mem_changed(SPIN, 0x1000, 0, &d);
        seen(d, "spin");
    }
    vk_guest_written(LONE, 0x1000);
    gen_t *ls = gen_slot(LONE, 0);
    CHECK(ls && (uint64_t)*ls == edge, "the page written at the boundary is stamped %llu, not %llu",
          ls ? (unsigned long long)*ls : 0ull, (unsigned long long)edge);
    CHECK(vk_mem_changed(LONE, 0x1000, 0, &now) == 1, "the page written at the boundary reads as never written");
    seen(now, "LONE");
    CHECK(vk_mem_changed(LONE, 0x1000, (gen_t)(edge - 1), &now) == 1, "the boundary's write is older than edge - 1");
    seen(now, "LONE again");
    for (int i = 0; i < 6; i++) {
        vk_mem_changed(SPIN, 0x1000, 0, &d);
        seen(d, "spin past");
    }

    // After the boundary: a CPU write found by the write watch and one by a host path are both seen.
    cpu_write(TEX);
    CHECK(vk_mem_changed(TEX, 0x4000, w_tex, &now) == 1, "TEX: a CPU write after the boundary is missed");
    seen(now, "TEX after a CPU write");
    w_tex = now;
    vk_guest_written(TEX + 0x1000, 4);
    CHECK(vk_mem_changed(TEX, 0x4000, w_tex, &now) == 1, "TEX: a host write after the boundary is missed");
    seen(now, "TEX after a host write");
    w_tex = now;
    CHECK(vk_mem_changed(TEX, 0x4000, w_tex, &now) == 0, "TEX: no write since its check, but it reads as written");
    seen(now, "TEX unwritten");
    w_tex = now;

    // A texture nobody wrote since before the boundary stays unchanged, check after check.
    CHECK(vk_mem_changed(OTHER, 0x4000, w_oth, &now) == 0,
          "OTHER: unwritten since before the boundary, reads as written");
    seen(now, "OTHER 1");
    w_oth = now;
    CHECK(vk_mem_changed(OTHER, 0x4000, w_oth, &now) == 0, "OTHER: unwritten, reads as written at the next check too");
    seen(now, "OTHER 2");
    w_oth = now;

    // The snapshot path (draw bindings and the texture pool's check) and the region check (reg_max).
    uint32_t ep = 7;
    CHECK(vk_mem_changed_snap(OTHER, 0x4000, w_oth, ep, &now) == 0, "OTHER via the snapshot: reads as written");
    seen(now, "OTHER snap");
    CHECK(vk_mem_unchanged_why_locked(OTHER, 0x4000, w_oth, ep) == 0, "OTHER's region check answers %d, not 0",
          vk_mem_unchanged_why_locked(OTHER, 0x4000, w_oth, ep));
    cpu_write(OTHER + 0x2000);
    ep = 8;
    CHECK(vk_mem_changed_snap(OTHER, 0x4000, w_oth, ep, &now) == 1, "OTHER via the snapshot: a CPU write is missed");
    seen(now, "OTHER snap written");
    CHECK(vk_mem_unchanged_why_locked(OTHER, 0x4000, w_oth, ep) == 3,
          "OTHER's region check answers %d, not 3 (written)",
          vk_mem_unchanged_why_locked(OTHER, 0x4000, w_oth, ep));
    CHECK(vk_mem_unchanged_why_locked(OTHER, 0x4000, now, ep) == 0, "OTHER's region check after its check answers %d",
          vk_mem_unchanged_why_locked(OTHER, 0x4000, now, ep));

    // The CPU-side tables (what mirror_sync, lazy_clean and cpu_wrote_since read) hold the same stamps.
    gen_t *gs = gen_slot(TEX + 0x1000, 0), *cs = slot_in(cgen_chunk, TEX + 0x1000, 0);
    CHECK(gs && cs && *gs == *cs && (uint64_t)*cs > edge, "TEX page 1: texture stamp %llu, CPU-side stamp %llu",
          gs ? (unsigned long long)*gs : 0ull, cs ? (unsigned long long)*cs : 0ull);
    CHECK((uint64_t)creg_max[TEX >> REG_SHIFT] > edge && reg_max[TEX >> REG_SHIFT] == creg_max[TEX >> REG_SHIFT],
          "TEX's region: newest stamps %llu / %llu", (unsigned long long)reg_max[TEX >> REG_SHIFT],
          (unsigned long long)creg_max[TEX >> REG_SHIFT]);

    // A VRAM mirror and a shadow copy synced before the boundary, TEX written after it.
    PoolRec p = {.kind = POOL_IMPORT, .import_lo = TEX, .import_hi = TEX + 0x4000};
    p.mgen = (__typeof__(p.mgen))pre, p.sgen = (__typeof__(p.sgen))pre;
    CHECK((uint64_t)p.mgen == pre && (uint64_t)p.sgen == pre, "PoolRec's mgen and sgen do not hold %llu",
          (unsigned long long)pre);
    CHECK(!lazy_clean(TEX, TEX + 0x4000, &p), "lazy_clean: the mirror lacks a CPU write after the boundary, yet clean");
    CHECK(cpu_wrote_since(TEX + 0x1000, p.sgen), "cpu_wrote_since: a CPU write after the boundary is missed");
    CHECK(page_dirty(TEX + 0x1000, p.mgen), "page_dirty: a CPU write after the boundary is missed");
    vk_mem_changed_snap(TEX, 0x4000, w_tex, ep, &now);   // the region gets this epoch's snapshot
    seen(now, "TEX snap");
    CHECK(!shadow_pool_clean(&p, ep, 1), "shadow_pool_clean (CPU-side tables): a write after the boundary is missed");
    CHECK(!shadow_pool_clean(&p, ep, 0), "shadow_pool_clean: a write after the boundary is missed");
    // Synced again now: nothing is newer.
    p.mgen = (__typeof__(p.mgen))gen_now, p.sgen = (__typeof__(p.sgen))gen_now;
    CHECK(lazy_clean(TEX, TEX + 0x4000, &p), "lazy_clean: synced after every write, yet not clean");
    CHECK(!cpu_wrote_since(TEX + 0x1000, p.sgen), "cpu_wrote_since: nothing written since the sync, yet written");
    CHECK(!page_dirty(TEX + 0x1000, p.mgen), "page_dirty: nothing written since the sync, yet dirty");
    CHECK(shadow_pool_clean(&p, ep, 1) && shadow_pool_clean(&p, ep, 0), "shadow_pool_clean: synced, yet not clean");

    // The log: one line where the count reached 2^32, none at any other boundary.
    int reached = seed < (1ull << 32) && (uint64_t)gen_now >= (1ull << 32);
    CHECK(n_gen_lines == reached, "%d page generation lines, not %d (first: %s)", n_gen_lines, reached,
          n_gen_lines ? gen_lines[0] : "none");
    if (reached && n_gen_lines)
        CHECK(strstr(gen_lines[0], "reached 4294967296 at frame 216000 after 1.0 h of play (19884 a frame)"),
              "the line at 2^32: %s", gen_lines[0]);
    for (int i = 0; i < n_gen_lines && i < 16; i++) {
        CHECK(!strstr(gen_lines[i], "run out") && !strstr(gen_lines[i], "ran out at"), "a line of running out: %s",
              gen_lines[i]);
        printf("  %s\n", gen_lines[i]);
    }

    printf("gen_wrap_check: seed %#llx, %u-bit generations, gen_now %#llx: %d of %d checks failed\n",
           (unsigned long long)seed, (unsigned)(8 * sizeof(gen_t)), (unsigned long long)gen_now, failed, checks);
    return failed ? 1 : 0;
}
