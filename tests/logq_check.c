// runtime/logq.c: log lines queued for the log thread, with a sink of the test's own that can be slow or held.
// Built and run by tests/logq_check.py; prints each failed check and exits 1 on any.
#include "logq.c"

#include <stdarg.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the sink: everything written, in the order it arrived; each call can take sink_ms, or wait while held is set
#define OUT_CAP (64u << 20)
static char *out;
static volatile size_t out_n;
static volatile int32_t sink_ms, held, sink_calls, in_sink;
static void sink(const char *p, size_t n) {
    plat_xchg32(&in_sink, 1);
    while (__atomic_load_n(&held, __ATOMIC_ACQUIRE)) plat_sleep_ms(1);
    int ms = __atomic_load_n(&sink_ms, __ATOMIC_ACQUIRE);
    if (ms) plat_sleep_ms((uint32_t)ms);
    size_t at = __atomic_fetch_add(&out_n, n, __ATOMIC_SEQ_CST);
    if (at + n <= OUT_CAP) memcpy(out + at, p, n);
    __atomic_add_fetch(&sink_calls, 1, __ATOMIC_SEQ_CST);
    plat_xchg32(&in_sink, 0);
}

// ---- the runtime logq.c reaches: its line about slow writes comes through here
void rt_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    logq_write(buf, (size_t)n);
}

// ---- producers: thread t writes lines "t <t> <seq> <padding>\n"
enum { THREADS = 8 };
typedef struct { int id, lines, pad; uint64_t worst_ns; } Prod;
static void producer(void *arg) {
    Prod *p = arg;
    char line[512];
    for (int i = 0; i < p->lines; i++) {
        int n = snprintf(line, sizeof line, "t %d %d %.*s\n", p->id, i, p->pad,
                         "................................................................................................"
                         "................................................................................................"
                         "................................................................................................");
        uint64_t t0 = plat_now_ns();
        logq_write(line, (size_t)n);
        uint64_t d = plat_now_ns() - t0;
        if (d > p->worst_ns) p->worst_ns = d;
    }
}

// Waits until the sink has taken everything written so far.
static void settle(size_t want) {
    for (int i = 0; i < 20000 && __atomic_load_n(&out_n, __ATOMIC_SEQ_CST) < want; i++) plat_sleep_ms(1);
}

// Every line of the output from at on is whole, and each thread's lines come in their order. Returns the lines seen;
// the slow-write lines ("log: ...") are counted in *notes.
static int check_lines(size_t at, int *next, int *notes) {
    int lines = 0;
    size_t end = __atomic_load_n(&out_n, __ATOMIC_SEQ_CST);
    while (at < end) {
        const char *nlp = memchr(out + at, '\n', end - at);
        if (!nlp) { CHECK(0, "the output ends inside a line"); break; }
        int t, seq;
        if (!strncmp(out + at, "log: ", 5)) {
            ++*notes;
        } else if (sscanf(out + at, "t %d %d", &t, &seq) == 2 && t >= 0 && t < THREADS) {
            if (seq != next[t]) CHECK(0, "thread %d: line %d came where %d was due", t, seq, next[t]);
            next[t] = seq + 1;
            lines++;
        } else {
            CHECK(0, "a torn line at byte %zu: %.40s", at, out + at);
        }
        at = (size_t)(nlp - out) + 1;
    }
    return lines;
}

static size_t run(Prod *pr, int n, int lines, int pad) {
    PlatThread *th[THREADS];
    size_t bytes = 0;
    for (int i = 0; i < n; i++) {
        pr[i] = (Prod){.id = i, .lines = lines, .pad = pad};
        th[i] = plat_thread_new(producer, &pr[i], "producer");
        if (!th[i]) { printf("FATAL: no thread\n"); exit(3); }
    }
    for (int i = 0; i < n; i++) plat_thread_join(th[i]);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < lines; k++) bytes += (size_t)snprintf(NULL, 0, "t %d %d %.*s\n", i, k, pad, "");
    return bytes + (size_t)n * (size_t)lines * (size_t)pad;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    out = malloc(OUT_CAP);
    if (!out) return 3;
    Prod pr[THREADS];
    int next[THREADS] = {0}, notes = 0;
    logq_init(sink);

    // 1. Before the queue is on, a line is written by the thread that logs it, before logq_write returns.
    logq_write("t 0 0 \n", 7);
    CHECK(out_n == 7 && sink_calls == 1, "a line without the queue: %zu bytes in %d writes", (size_t)out_n, (int)sink_calls);
    size_t at = 0;
    CHECK(check_lines(at, next, &notes) == 1, "the first line");
    at = out_n;
    for (int i = 0; i < THREADS; i++) next[i] = 0;

    // 2. With the queue: eight threads, 20,000 lines each; every line arrives whole and each thread's in order.
    CHECK(logq_async(), "the log thread did not start");
    size_t want = at + run(pr, THREADS, 20000, 40);
    settle(want);
    CHECK(out_n == want, "%zu bytes written, %zu queued", (size_t)out_n - at, want - at);
    CHECK(check_lines(at, next, &notes) == THREADS * 20000, "lines lost or doubled");
    CHECK(sink_calls < THREADS * 20000 / 2, "%d writes for %d lines: the queue did not batch", (int)sink_calls, THREADS * 20000);
    at = out_n;
    for (int i = 0; i < THREADS; i++) next[i] = 0;

    // 3. A slow destination (30 ms a write) does not hold the threads that log: 100 lines, a millisecond apart.
    __atomic_store_n(&sink_ms, 30, __ATOMIC_RELEASE);
    uint64_t worst = 0;
    for (int i = 0; i < 100; i++) {
        char line[64];
        int n = snprintf(line, sizeof line, "t 0 %d \n", i);
        uint64_t t0 = plat_now_ns();
        logq_write(line, (size_t)n);
        uint64_t d = plat_now_ns() - t0;
        if (d > worst) worst = d;
        want += (size_t)n;
        plat_sleep_ms(1);
    }
    CHECK(worst < 15000000, "a line waited %.1f ms for a 30 ms write", worst / 1e6);
    settle(want);
    __atomic_store_n(&sink_ms, 0, __ATOMIC_RELEASE);
    settle(want);
    plat_sleep_ms(50);   // the log thread's line about the slow writes
    CHECK(check_lines(at, next, &notes) == 100, "lines lost behind a slow write");
    CHECK(notes == 1, "%d lines about slow writes within ten seconds, want 1", notes);
    want = at = out_n;
    for (int i = 0; i < THREADS; i++) next[i] = 0;

    // 4. A destination that stops while more is logged than the queue holds: the threads wait, nothing is lost.
    __atomic_store_n(&held, 1, __ATOMIC_RELEASE);
    logq_write("t 0 0 \n", 7);   // the log thread takes it and stops in the sink
    for (int i = 0; i < 5000 && !__atomic_load_n(&in_sink, __ATOMIC_ACQUIRE); i++) plat_sleep_ms(1);
    CHECK(in_sink, "the log thread did not reach the sink");
    PlatThread *th[THREADS];
    for (int i = 0; i < THREADS; i++) {
        pr[i] = (Prod){.id = i, .lines = 2000, .pad = 250};   // 8 x 2000 x 260 bytes: four times the queue
        th[i] = plat_thread_new(producer, &pr[i], "producer");
    }
    plat_sleep_ms(200);
    size_t stuck = out_n;
    __atomic_store_n(&held, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < THREADS; i++) plat_thread_join(th[i]);
    plat_sleep_ms(200);
    CHECK(stuck == at, "%zu bytes were written while the destination was stopped", stuck - at);
    at += 7;   // past the line that stopped the log thread
    CHECK(check_lines(at, next, &notes) == THREADS * 2000, "lines lost at a full queue");
    at = out_n;
    for (int i = 0; i < THREADS; i++) next[i] = 0;

    // 5. logq_sync: what is queued is written before it returns, and later lines are written by their own thread.
    __atomic_store_n(&sink_ms, 20, __ATOMIC_RELEASE);
    for (int i = 0; i < 50; i++) {
        char line[64];
        int n = snprintf(line, sizeof line, "t 1 %d \n", i);
        logq_write(line, (size_t)n);
    }
    logq_sync();
    __atomic_store_n(&sink_ms, 0, __ATOMIC_RELEASE);
    CHECK(check_lines(at, next, &notes) == 50, "logq_sync returned before the queued lines were written");
    at = out_n;
    int calls = sink_calls;
    logq_write("t 2 0 \n", 7);
    CHECK(out_n == at + 7 && sink_calls == calls + 1, "a line after logq_sync was not written at once");
    logq_sync();   // a second call does nothing
    CHECK(check_lines(at, next, &notes) == 1, "the line after logq_sync");

    printf("logq_check: %s (%zu bytes in %d writes)\n", fails ? "FAILED" : "ok", (size_t)out_n, (int)sink_calls);
    return fails ? 1 : 0;
}
