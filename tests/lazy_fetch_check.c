// runtime/memory.c's lazy write-back (memory.c "Lazy write-back"), built with the Windows platform layer and its real
// fault handler, the rest of the runtime stubbed. The back end's fetch is a stand-in that waits (the GPU copy and the
// wait for it) and then writes a marker over the first 64 bytes of the copy, opening the page for it, as the GPU's
// copy writes past the pages' protection; the back end's clean test may wait too. Built and run by
// tests/lazy_fetch_check.py, one scenario per process (the argument), so an access violation passed to the crash
// handler shows as the exit code. Prints each failed check and exits 1 on any.
#include "memory.c"

#include <stdarg.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the runtime memory.c and the platform layer reach
unsigned rt_trace;
uint64_t rt_guest_start_ns;
const unsigned long long rt_app_pool_bytes = 0xCD500000ull;
const RtLayoutEntry rt_layout[] = {{"main", MOD_MAIN_BASE, MOD_MAIN_SIZE}};
const int rt_layout_count = 1;
const char *cfg_get(const char *name) { (void)name; return NULL; }
const char *cfg_value(const char *name) { (void)name; return "0"; }
int cfg_bool(const char *name) {
    if (!strcmp(name, "NX2NAVI_WB_LAZY")) return 1;
    if (!strcmp(name, "NX2NAVI_WB_LAZY_CHUNK")) return getenv("CHECK_CHUNK") != NULL;
    return 0;
}
void cfg_init(void) {}
int cfg_setenv(const char *name, const char *value) { (void)name, (void)value; return 0; }
int rt_knob(const char *name, const char *def) { (void)name; return strcmp(def, "0") != 0; }
int ldr_count(void) { return 0; }
const RtLoaded *ldr_module(int i) { (void)i; return NULL; }
uint64_t perf_mark_begin(void) { return 0; }
void perf_mark_end(int kind, uint64_t t0, uint64_t n) { (void)kind, (void)t0, (void)n; }
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    (void)s, (void)name, (void)count, (void)limit, (void)bytes;
}
uint64_t res_heap_extra_bytes(void) { return 0; }
static char last_log[4096];
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    printf("  log: %s\n", last_log);
}
void rt_logf(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
    _Exit(3);
}

// ---- the back end's stand-ins
static uint64_t heap;
static volatile uint64_t fetch_ns = 300000000ull, clean_ns;
static volatile int fetch_calls;
static volatile uint8_t marker = 0xA0;
static void fake_fetch(uint64_t h0, uint64_t h1, uint64_t off, uint64_t size) {
    (void)h0, (void)size;
    __atomic_add_fetch(&fetch_calls, 1, __ATOMIC_SEQ_CST);
    plat_wait_ns(fetch_ns);
    uint64_t a = h1 + off;   // the GPU's copy writes past the protection; here the page is opened for it
    plat_mem_protect(a & ~(PAGE - 1), PAGE, PLAT_PROT_RW);
    for (int j = 0; j < 64; j++) ((volatile uint8_t *)(uintptr_t)a)[j] = marker;
    plat_mem_protect(a & ~(PAGE - 1), PAGE, PLAT_PROT_NONE);
}
static int fake_clean(uint64_t lo, uint64_t hi, void *ctx) {
    (void)lo, (void)hi, (void)ctx;
    if (clean_ns) plat_wait_ns(clean_ns);   // GetWriteWatch while the driver pins memory
    return 1;
}

// the range [a, a + n) of the heap, as written back from buffer h0 into the heap's own (h1 = heap, offset a - heap)
static uint64_t arm_buf(uint64_t a, uint64_t n, uint64_t h0) { return mem_lazy_arm(a, n, h0, heap, a - heap, NULL); }
// the replay leaves its writes to [a, a + n) in the mirror h0: the armed parts there are stale (a touch fetches them)
static int stale(uint64_t a, uint64_t n, uint64_t h0) {
    uint64_t out[16][2];
    return mem_lazy_skip(a, n, h0, -(int64_t)heap, out, 16);
}
static double ms(uint64_t t0) { return (plat_now_ns() - t0) / 1e6; }
// the number after "what" in the last log line, -1 when it is not there
static double logged(const char *what) {
    const char *p = strstr(last_log, what);
    return p ? strtod(p + strlen(what), NULL) : -1;
}

// ---- threads that touch guest memory, as the game's CPU code does
typedef struct { uint64_t at; int write; volatile int done; uint8_t got; uint64_t end_ns; double took; } Touch;
static void toucher(void *arg) {
    Touch *t = arg;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)t->at;
    uint64_t t0 = plat_now_ns();
    if (t->write) *p = 0x77;
    t->got = *p;
    t->end_ns = plat_now_ns();
    t->took = (t->end_ns - t0) / 1e6;
    __atomic_store_n(&t->done, 1, __ATOMIC_SEQ_CST);
}
static void *touch(Touch *t, uint64_t a, int write) {
    t->at = a, t->write = write, t->done = 0;
    return plat_thread_new(toucher, t, "toucher");
}

static volatile int spin_stop;
static volatile uint64_t spin_reads;
static void spinner(void *arg) {
    volatile uint8_t *p = arg;
    while (!__atomic_load_n(&spin_stop, __ATOMIC_SEQ_CST)) (void)*p, spin_reads++;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *what = argc > 1 ? argv[1] : "";
    mem_init();
    if (mem_set_heap_size(256ull << 20, &heap) != R_OK || !heap) return printf("SetHeapSize failed\n"), 2;
    if (!mem_lazy_enable(fake_fetch, fake_clean)) return printf("the lazy write-back is off\n"), 2;
    memset((void *)(uintptr_t)heap, 0x11, 256ull << 20);
    const uint64_t K64 = 0x10000;

    if (!strcmp(what, "arm_race") || !strcmp(what, "arm_race_slow")) {
        // A thread reads a page over and over while the completion thread's arm (vk_memory.c vk_gpu_written_back)
        // makes its range inaccessible and a release gives it back: no read may reach the crash handler. The slow one
        // has the clean test take 100 ms, as GetWriteWatch can while the driver pins memory, with the pages
        // inaccessible all along.
        uint64_t t = heap + (4ull << 20), r = t >> FRZ_SHIFT;
        void *th = plat_thread_new(spinner, (void *)(uintptr_t)(t + 0x100), "spinner");
        plat_wait_ns(20000000);
        int slow = !strcmp(what, "arm_race_slow");
        if (slow) clean_ns = 100000000;
        uint64_t armed = 0;
        for (int i = 0; i < (slow ? 1 : 20000); i++) {
            plat_mutex_lock(&frz_lock);   // the hold a touch leaves, so that every turn arms again
            lz_hot[r] = 0;
            plat_mutex_unlock(&frz_lock);
            armed += arm_buf(t, K64, 1) != 0;
            mem_lazy_release(t, K64);
        }
        spin_stop = 1;
        plat_thread_join(th);
        CHECK(armed, "nothing was armed");
        printf("  %llu arms, %llu reads\n", (unsigned long long)armed, (unsigned long long)spin_reads);
    } else if (!strcmp(what, "report")) {
        // The report line gives the longest fetch since the last line, and the longest wait of a fault for frz_lock;
        // the next line starts from nothing.
        uint64_t s1 = heap + (80ull << 20), s2 = heap + (84ull << 20), s3 = heap + (88ull << 20);
        CHECK(arm_buf(s1, K64, 1) == K64 && arm_buf(s2, K64, 1) == K64 && arm_buf(s3, K64, 1) == K64, "arms");
        CHECK(stale(s1, K64, 1) == 1 && stale(s2, K64, 1) == 1 && stale(s3, K64, 1) == 1, "stale marks");
        Touch a;
        fetch_ns = 30000000ull;
        plat_thread_join(touch(&a, s1, 0));
        fetch_ns = 120000000ull;
        plat_thread_join(touch(&a, s2, 0));
        fetch_ns = 10000000ull;
        plat_mutex_lock(&frz_lock);   // another thread's thaw, freeze or arm, 60 ms long
        void *th = touch(&a, s3, 1);
        plat_wait_ns(60000000);
        plat_mutex_unlock(&frz_lock);
        plat_thread_join(th);
        CHECK(fetch_calls == 3 && a.got == 0x77, "%d fetches, the write read back %#x", fetch_calls, a.got);
        mem_lazy_report(1, 0);
        double longest = logged("the longest "), waited = logged("faults waited up to ");
        CHECK(longest >= 119 && longest < 200, "the longest fetch read %.2f ms", longest);
        CHECK(waited >= 40 && waited < 200, "the longest wait for the lock read %.2f ms", waited);
        mem_lazy_report(2, 0);
        CHECK(logged("the longest ") == 0 && logged("faults waited up to ") == 0, "the next line did not start over");
    } else {
        printf("unknown scenario %s\n", what);
        return 2;
    }
    if (fails) return printf("%s: %d checks failed\n", what, fails), 1;
    printf("%s: all checks passed\n", what);
    return 0;
}
