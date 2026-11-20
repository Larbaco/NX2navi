// The order of uniform snapshot drops between replay chunks, on the real vk_usnap.c, without a GPU.
//
// A draw that writes guest memory as storage drops the snapshot regions it touches (vk_usnap_drop) from its place in
// the stream on (vk_exec_stream_pos: its group and chunk), and a later uniform binding of that memory then reads the
// live bytes, which hold the GPU's result when the draw runs. The chunks of a group, and with NX2SARIA_REPLAY_PIPE the
// chunks of the groups after it, record at the same time. Two orders are forced here with barriers:
// - early: a later chunk of the same group has dropped the region already when an earlier chunk looks it up. The
//   earlier draw must still bind the snapshot: live memory holds what the game wrote after the submit.
// - stale: an earlier chunk's drop has not run yet when a later chunk looks the region up, so the later draw binds the
//   snapshot and reads the bytes as of the submit instead of the earlier draw's result. This one is known and left
//   open (docs/thread-sanitizer.md "Replay hazards"): the runtime counts the chunks' drops into copied ranges, its
//   precondition, and none happened in the scenes measured. The probe expects the wrong read, so a change that fixes
//   it shows here.
// Each case also runs in the stream's own order (the control, which must read the right value). The value a draw reads
// is the snapshot's bytes when vk_usnap_find hands out the copy, else what live memory holds when the GPU runs the
// draw, in stream order.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/vk_usnap.c"

static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint64_t test_pos;   // the thread's place in the stream: group << 5 | chunk (vk_exec_stream_pos)
static _Thread_local int test_chunk;

void plat_mutex_lock(PlatMutex *m) { (void)m; pthread_mutex_lock(&test_lock); }
void plat_mutex_unlock(PlatMutex *m) { (void)m; pthread_mutex_unlock(&test_lock); }
int vk_exec_shared(void) { return 1; }
uint64_t vk_exec_stream_pos(void) { return test_pos; }
int vk_exec_in_chunk(void) { return test_chunk; }
void vk_exec_usnap_hit(void) {}
int vk_mem_lookup(uint64_t addr, uint64_t size, VkMemSpan *out) { (void)addr; (void)size; (void)out; return 0; }
int vk_dlaa_available(void) { return 0; }
int vk_dlaa_fg_only(void) { return 0; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

#define BASE 0x100000ull
#define OLD 0x11111111u    // guest memory at the submit: the snapshot's copy
#define LATE 0x22222222u   // what the game wrote into the block after the submit, before the GPU ran the draws
#define NEW 0x33333333u    // what the storage-writing draw stores
#define POS(group, chunk) ((uint64_t)(group) << 5 | (uint64_t)(chunk))
#define SUBMITTER(groups_out) POS(groups_out, 31)   // the submitting thread past that many groups

static uint8_t arena_bytes[256];
static Arena arena;

static void fresh(void) {
    static Reg reg[1];
    memset(reg, 0, sizeof reg);
    reg[0].lo = BASE, reg[0].hi = BASE + 0x100;
    U.mode = 1, U.on = 1, U.reg = reg, U.nreg = U.creg = 1, U.nwr = 0, U.chunk_drops = 0;
    uint32_t v = OLD;
    for (int i = 0; i < 64; i++) memcpy(arena_bytes + 4 * i, &v, 4);
    arena.buf = (VkBuffer)(uintptr_t)1, arena.map = arena_bytes, arena.cpu = NULL;
    U.arena = &arena;
}

typedef struct {
    uint64_t pos;
    int writes;              // the draw writes the block as storage (drops it), else binds it as uniforms
    int first;               // takes its step before the other thread's: 1, or after: 0
    uint32_t live_at_gpu;    // live memory when the GPU runs this draw (stream order)
    uint32_t read;           // the value the draw reads
    pthread_barrier_t *b;
} Step;

static void *step_main(void *arg) {
    Step *c = arg;
    test_pos = c->pos, test_chunk = (c->pos & 31) != 31;
    if (!c->first) pthread_barrier_wait(c->b);
    if (c->writes) {
        vk_usnap_drop(BASE, 0x100);
    } else {
        const uint8_t *s = vk_usnap_bytes(BASE + 0x40, 4);   // the uniform binding: the snapshot, or live memory
        if (s) memcpy(&c->read, s, 4);
        else c->read = c->live_at_gpu;
    }
    if (c->first) pthread_barrier_wait(c->b);
    return NULL;
}

// The writer at stream place wpos and the reader at rpos; reader_first chooses who takes its step first. Returns the
// value the reader's draw read.
static uint32_t run(uint64_t wpos, uint64_t rpos, int reader_first, uint32_t live) {
    fresh();
    pthread_barrier_t b;
    pthread_barrier_init(&b, NULL, 2);
    Step w = {wpos, 1, !reader_first, 0, 0, &b}, r = {rpos, 0, reader_first, live, 0, &b};
    pthread_t tw, tr;
    pthread_create(&tw, NULL, step_main, &w);
    pthread_create(&tr, NULL, step_main, &r);
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);
    pthread_barrier_destroy(&b);
    return r.read;
}

static int failures;

// expect_right: the read must be the stream order's value; else the case is the known open one and must read wrong.
static void check(const char *name, uint32_t got, uint32_t want, int expect_right) {
    int right = got == want;
    int ok = right == expect_right;
    printf("%-58s read %#010x, the stream's order needs %#010x: %s%s\n", name, got, want, right ? "right" : "wrong",
           ok ? "" : expect_right ? "  FAILED" : "  (no longer wrong: update the probe and docs/thread-sanitizer.md)");
    failures += !ok;
}

int main(void) {
    // early: the writer is later in the stream (chunk 1 of group 5, the reader chunk 0 of the same group); the reader's
    // draw runs before the write, and live memory then holds the game's later bytes: the right value is the snapshot.
    check("early, same group: control (read ran first)", run(POS(5, 1), POS(5, 0), 1, LATE), OLD, 1);
    check("early, same group: later chunk's drop ran first", run(POS(5, 1), POS(5, 0), 0, LATE), OLD, 1);
    check("early, later group's drop ran first", run(POS(6, 0), POS(5, 0), 0, LATE), OLD, 1);
    check("early, the submitting thread's drop past the group", run(SUBMITTER(5), POS(5, 3), 0, LATE), OLD, 1);
    // stale: the writer is earlier in the stream; the reader's draw runs after the write on the GPU, so the right
    // value is NEW. The controls run the drop first; the forced orders are the known open case.
    check("stale, same group: control (write ran first)", run(POS(5, 0), POS(5, 1), 0, NEW), NEW, 1);
    check("stale, next group (REPLAY_PIPE): control", run(POS(5, 0), POS(6, 0), 0, NEW), NEW, 1);
    check("stale, submitting thread behind a group: control", run(POS(5, 0), SUBMITTER(5), 0, NEW), NEW, 1);
    check("stale, same group: earlier chunk's drop not run yet", run(POS(5, 0), POS(5, 1), 1, NEW), NEW, 0);
    check("stale, next group: earlier group's drop not run yet", run(POS(5, 0), POS(6, 0), 1, NEW), NEW, 0);
    check("stale, submitting thread behind: drop not run yet", run(POS(5, 0), SUBMITTER(5), 1, NEW), NEW, 0);
    // the chunks' drops into copied ranges are counted (the stale case's precondition); the submitting thread's are not
    run(POS(5, 0), POS(5, 1), 0, NEW);
    int counted = (int)U.chunk_drops;
    run(SUBMITTER(5), POS(6, 0), 0, NEW);
    int counted_submitter = (int)U.chunk_drops;
    printf("chunk drops into copied ranges counted: %d for a chunk's drop, %d for the submitting thread's\n", counted,
           counted_submitter);
    failures += counted != 1 || counted_submitter != 0;
    printf("usnap order: %d unexpected results\n", failures);
    return failures ? 1 : 0;
}
