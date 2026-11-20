// Exercise the real uniform snapshot drop, lookup and diagnostic paths concurrently without a GPU. The replay
// stream position, pool lookup, hit accounting and platform lock are stand-ins; this does not test replay order or GPU fences.
// Phase barriers make both threads progress between blocks without ordering their accesses within each block. They do
// not force individual operations to interleave: an empty TSan report is not a proof of every schedule.
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/vk_usnap.c"

static pthread_mutex_t test_drop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t start;
static _Atomic int failures;
static _Thread_local uint64_t test_pos = 64;   // the caller's place in the stream (vk_exec_stream_pos)

// vk_usnap.c takes only drop_lock along the exercised paths. The other runtime boundaries do not need a device.
void plat_mutex_lock(PlatMutex *m) { (void)m; pthread_mutex_lock(&test_drop_lock); }
void plat_mutex_unlock(PlatMutex *m) { (void)m; pthread_mutex_unlock(&test_drop_lock); }
int vk_exec_shared(void) { return 1; }
uint64_t vk_exec_stream_pos(void) { return test_pos; }
int vk_exec_in_chunk(void) { return 0; }
void vk_exec_usnap_hit(void) {}
int vk_mem_lookup(uint64_t addr, uint64_t size, VkMemSpan *out) { (void)addr; (void)size; (void)out; return 0; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

enum { REGIONS = 8192, PHASES = 8, PER_PHASE = REGIONS / PHASES };
#define BASE 0x100000ull
#define STEP 0x100ull

static void *dropper(void *arg) {
    (void)arg;
    for (int phase = 0; phase < PHASES; phase++) {
        pthread_barrier_wait(&start);
        for (int i = phase * PER_PHASE; i < (phase + 1) * PER_PHASE; i++)
            vk_usnap_drop(BASE + (uint64_t)i * STEP, 1);
        pthread_barrier_wait(&start);
    }
    return NULL;
}

static void *reader(void *arg) {
    (void)arg;
    uint64_t last = 0;
    for (int phase = 0; phase < PHASES; phase++) {
        pthread_barrier_wait(&start);
        for (int i = phase * PER_PHASE; i < (phase + 1) * PER_PHASE; i++) {
            uint64_t generation = vk_usnap_gen();
            if (generation < last || generation > REGIONS) atomic_fetch_add(&failures, 1);
            last = generation;
            uint64_t addr = BASE + (uint64_t)i * STEP;
            int why = vk_usnap_why(addr, 1);
            if (why != VK_USNAP_FOUND && why != VK_USNAP_DROPPED) atomic_fetch_add(&failures, 1);
            // This range is outside all regions and writes, so why scans the list while the other thread grows it.
            if (vk_usnap_why(0x100000000ull, 1) != VK_USNAP_NOT_CPU) atomic_fetch_add(&failures, 1);
            VkDescriptorBufferInfo info;
            (void)vk_usnap_find(addr, 1, 1, &info);   // dropped can change between the two separate calls
        }
        pthread_barrier_wait(&start);
        if (vk_usnap_gen() != (uint64_t)(phase + 1) * PER_PHASE) atomic_fetch_add(&failures, 1);
    }
    return NULL;
}

int main(void) {
    static Arena arena;
    U.mode = 1;
    U.on = 1;
    U.reg = calloc(REGIONS, sizeof *U.reg);
    if (!U.reg) return 2;
    U.nreg = U.creg = REGIONS;
    arena.buf = (VkBuffer)(uintptr_t)1;
    U.arena = &arena;
    for (int i = 0; i < REGIONS; i++) U.reg[i].lo = BASE + (uint64_t)i * STEP, U.reg[i].hi = U.reg[i].lo + 0x40;
    VkDescriptorBufferInfo info;
    if (!vk_usnap_find(BASE, 1, 1, &info)) return 2;
    if (pthread_barrier_init(&start, NULL, 2)) return 2;
    pthread_t writer, reader_thread;
    if (pthread_create(&writer, NULL, dropper, NULL) || pthread_create(&reader_thread, NULL, reader, NULL)) return 2;
    pthread_join(writer, NULL);
    pthread_join(reader_thread, NULL);
    pthread_barrier_destroy(&start);
    if (vk_usnap_gen() != REGIONS || U.nwr != REGIONS || vk_usnap_why(BASE, 1) != VK_USNAP_DROPPED ||
        vk_usnap_why(BASE + (REGIONS - 1) * STEP, 1) != VK_USNAP_DROPPED || vk_usnap_find(BASE, 1, 1, &info))
        atomic_fetch_add(&failures, 1);
    // A lookup before the drop's place in the stream may use the snapshot; at or after it, it may not. why keeps its
    // existing diagnostic behaviour and reports any drop, including one later in the stream than the caller.
    test_pos = 63;
    if (!vk_usnap_find(BASE, 1, 1, &info) || vk_usnap_why(BASE, 1) != VK_USNAP_DROPPED)
        atomic_fetch_add(&failures, 1);
    test_pos = 64;
    if (vk_usnap_find(BASE, 1, 1, &info)) atomic_fetch_add(&failures, 1);
    int n = atomic_load(&failures);
    free(U.reg);
    free(U.wr);
    printf("usnap concurrency: %d failures, %d drops and vector entries\n", n, REGIONS);
    return n ? 1 : 0;
}
