// The lifetime of pool records and VRAM mirrors against a reader on another thread, on the real vk_memory.c, without
// a GPU. Each case runs in a child process built with AddressSanitizer, so a use after free ends that child with a
// report instead of passing unseen.
//
// - fini: the replay resolved an address of a pool (vk_mem_resolve) and uses the record after the game's thread
//   finalized the pool (vk_pool_fini) in between. A resolved pool is marked used, and pool_fini then keeps the record
//   until the submission being recorded completes. The control clears the mark first, as if the protocol did not hold:
//   the record goes at once and the reader's use is a use after free.
// - evict: a replay worker uses mirrors while another evicts the idle ones (mirror_evict, under use_lock as
//   mirror_create calls it while chunks record). A mirror the submission used is not idle, and the lock serializes the
//   rest, so a use never gets a mirror half taken down. The control breaks both (any mirror may go, the use takes the
//   unlocked path): a use that passed the state check then reads the buffer the eviction just cleared.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../runtime/vk_memory.c"

// A sanitizer report ends the child with 66, a wrong state with 1.
const char *__asan_default_options(void) { return "exitcode=66"; }

// ---- the runtime around vk_memory.c
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static pthread_mutex_t init_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t *rw_of(PlatMutex *m) {
    pthread_rwlock_t *l = __atomic_load_n((pthread_rwlock_t **)&m->p, __ATOMIC_ACQUIRE);
    if (l) return l;
    pthread_mutex_lock(&init_m);
    if (!(l = m->p)) {
        l = malloc(sizeof *l);
        pthread_rwlock_init(l, NULL);
        __atomic_store_n((pthread_rwlock_t **)&m->p, l, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&init_m);
    return l;
}
void plat_mutex_lock(PlatMutex *m) { pthread_rwlock_wrlock(rw_of(m)); }
void plat_mutex_unlock(PlatMutex *m) { pthread_rwlock_unlock(rw_of(m)); }
void plat_mutex_lock_shared(PlatMutex *m) { pthread_rwlock_rdlock(rw_of(m)); }
void plat_mutex_unlock_shared(PlatMutex *m) { pthread_rwlock_unlock(rw_of(m)); }
static _Thread_local int tid_self = 1;   // nonzero per thread for the recursive lock
static int tid_next = 1;
static uint32_t tid(void) {
    if (tid_self == 1) tid_self = 1 + __atomic_add_fetch(&tid_next, 1, __ATOMIC_RELAXED);
    return (uint32_t)tid_self;
}
void plat_rmutex_lock(PlatRMutex *m) {
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == tid()) { m->depth++; return; }
    plat_mutex_lock(&m->m);
    m->owner = tid(), m->depth = 1;
}
void plat_rmutex_unlock(PlatRMutex *m) {
    if (--m->depth) return;
    m->owner = 0;
    plat_mutex_unlock(&m->m);
}
int plat_rmutex_trylock(PlatRMutex *m) { plat_rmutex_lock(m); return 1; }
void plat_once(PlatOnce *o, void (*fn)(void *arg), void *arg) {
    if (__atomic_exchange_n(&o->state, 1, __ATOMIC_ACQ_REL) == 0) fn(arg);
}
uint64_t plat_now_ns(void) { return now_ns(); }
uint64_t kern_now_ns(void) { return now_ns(); }
int cfg_bool(const char *name) { (void)name; return 0; }
const char *cfg_get(const char *name) { (void)name; return NULL; }
int cfg_int(const char *name) { (void)name; return 0; }
int rt_knob(const char *name, const char *def) { (void)name; return def ? atoi(def) : 0; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
    abort();
}
unsigned rt_trace;
uint64_t rt_guest_start_ns;
GfxStats gfx_counters;
int vk_sub_in_frame;
volatile int32_t vk_lost;
PFN_vkGetDeviceProcAddr vk_dpa;
int mem_frozen(uint64_t a) { (void)a; return 0; }
void mem_lazy_release(const uint64_t (*rg)[3], int n) { (void)rg, (void)n; }
void mem_thaw(uint64_t a, uint64_t n) { (void)a, (void)n; }
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    (void)addr, (void)size, (void)pages, (void)max;
    return 0;
}
uint64_t perf_mark_begin(void) { return 0; }
void perf_mark_end(int kind, uint64_t t0, uint64_t bytes) { (void)kind, (void)t0, (void)bytes; }
int plat_mem_dmabuf(uint64_t addr, uint64_t n) { (void)addr, (void)n; return 0; }
void plat_mem_dmabuf_done(uint64_t addr, uint64_t n) { (void)addr, (void)n; }
void rec_check_handle(int kind, uint64_t handle, int destroy, uint64_t tl, uint64_t gpu) {
    (void)kind, (void)handle, (void)destroy, (void)tl, (void)gpu;
}
void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) { (void)cb, (void)kind, (void)n; }
void vk_device_lost_check(VkResult r, const char *where) { (void)r, (void)where; }
uint64_t vk_exec_recording_fence(void) { return 0; }
void vk_gpa_clear(uint64_t lo, uint64_t hi) { (void)lo, (void)hi; }
uint32_t vk_gpa_map(uint64_t lo, uint64_t hi, VkBuffer buf, uint64_t buf_lo) { (void)lo, (void)hi, (void)buf, (void)buf_lo; return 0; }
uint32_t vk_gpa_remap(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo) {
    (void)lo, (void)hi, (void)buf, (void)buf_lo;
    return old_rec;
}
uint32_t vk_gpa_remap_plain(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo) {
    (void)lo, (void)hi, (void)buf, (void)buf_lo;
    return old_rec;
}
void vk_gpa_unmap(uint64_t lo, uint64_t hi, uint32_t rec, uint32_t (*cover)(uint64_t page_addr)) {
    (void)lo, (void)hi, (void)rec, (void)cover;
}
int vk_gpa_wanted(void) { return 0; }
void vk_gtl_note(int kind, uint64_t sub_ns, uint64_t start_tick, uint64_t end_tick) {
    (void)kind, (void)sub_ns, (void)start_tick, (void)end_tick;
}
uint64_t vk_headroom_release(void) { return 0; }
void vk_queue_lock(void) {}
void vk_queue_unlock(void) {}
static uint64_t tl_issued = 10, tl_done = 10;   // the timeline: submissions issued, and done on the "GPU"
uint64_t vk_sync_issued(void) { return __atomic_load_n(&tl_issued, __ATOMIC_ACQUIRE); }
uint64_t vk_sync_fence_done(void) { return __atomic_load_n(&tl_done, __ATOMIC_ACQUIRE); }
int vk_sync_fence_wait(uint64_t value, uint64_t timeout_ns) { (void)timeout_ns; return value <= vk_sync_fence_done(); }
// completions run when the harness moves tl_done on (complete_to)
typedef struct { uint64_t fence; void (*fn)(void *); void *arg; } Comp;
static Comp comps[256];
static int ncomps;
static pthread_mutex_t comp_m = PTHREAD_MUTEX_INITIALIZER;
void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) {
    pthread_mutex_lock(&comp_m);
    if (ncomps < 256) comps[ncomps++] = (Comp){fence, fn, arg};
    pthread_mutex_unlock(&comp_m);
}
void vk_texstat_time(int kind, uint64_t t0) { (void)kind, (void)t0; }
void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc) { (void)lo, (void)size, (void)alloc; }
void vk_texture_want_room(uint64_t bytes) { (void)bytes; }
static uint64_t next_handle = 0x1000;
static uint64_t destroyed[4096];
static int ndestroyed;
static pthread_mutex_t handle_m = PTHREAD_MUTEX_INITIALIZER;
static void note_destroy(uint64_t h) {
    if (!h) return;
    pthread_mutex_lock(&handle_m);
    if (ndestroyed < 4096) destroyed[ndestroyed++] = h;
    pthread_mutex_unlock(&handle_m);
}
static int was_destroyed(uint64_t h) {
    pthread_mutex_lock(&handle_m);
    int d = 0;
    for (int i = 0; i < ndestroyed && !d; i++) d = destroyed[i] == h;
    pthread_mutex_unlock(&handle_m);
    return d;
}
static VkBuffer new_handle(void) { return (VkBuffer)(uintptr_t)__atomic_add_fetch(&next_handle, 0x10, __ATOMIC_RELAXED); }
VkResult vk_vma_create_buffer(VmaAllocator vma, const VkBufferCreateInfo *bci, const VmaAllocationCreateInfo *ai,
                              VkBuffer *buf, VmaAllocation *a, VmaAllocationInfo *info, const char *file) {
    (void)vma, (void)bci, (void)ai, (void)file;
    *buf = new_handle();
    *a = (VmaAllocation)(uintptr_t)1;
    if (info) memset(info, 0, sizeof *info);
    return VK_SUCCESS;
}
void vk_vma_destroy_buffer(VmaAllocator vma, VkBuffer buf, VmaAllocation a) { (void)vma, (void)a, note_destroy((uint64_t)(uintptr_t)buf); }
void vk_vma_destroy_image(VmaAllocator vma, VkImage img, VmaAllocation a) { (void)vma, (void)img, (void)a; }
void vk_vma_free_memory(VmaAllocator vma, VmaAllocation a) { (void)vma, (void)a; }
VkResult vmaFlushAllocation(VmaAllocator vma, VmaAllocation a, VkDeviceSize off, VkDeviceSize size) {
    (void)vma, (void)a, (void)off, (void)size;
    return VK_SUCCESS;
}
void vmaGetHeapBudgets(VmaAllocator vma, VmaBudget *b) {
    (void)vma;
    memset(b, 0, sizeof *b * VK_MAX_MEMORY_HEAPS);
    for (int i = 0; i < VK_MAX_MEMORY_HEAPS; i++) b[i].budget = 64ull << 30;
}
void rc_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool qp, uint32_t q) {
    (void)cb, (void)st, (void)qp, (void)q;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a,
                                                VkDeviceMemory *m) {
    (void)d, (void)i, (void)a;
    *m = (VkDeviceMemory)(uintptr_t)new_handle();
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {
    (void)d, (void)b, (void)m, (void)o;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *i, const VkAllocationCallbacks *a, VkBuffer *b) {
    (void)d, (void)i, (void)a;
    *b = new_handle();
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d, (void)a, note_destroy((uint64_t)(uintptr_t)b); }
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d, (void)i, (void)a; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d, (void)v, (void)a; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) { (void)cb; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d, (void)m, (void)a; }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r) {
    (void)d, (void)b;
    memset(r, 0, sizeof *r);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice d, VkFence f) { (void)d, (void)f; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties2 *m) {
    (void)p;
    memset(&m->memoryProperties, 0, sizeof m->memoryProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d, VkQueryPool q, uint32_t f, uint32_t n, size_t sz, void *data,
                                                     VkDeviceSize st, VkQueryResultFlags fl) {
    (void)d, (void)q, (void)f, (void)n, (void)sz, (void)data, (void)st, (void)fl;
    return VK_NOT_READY;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice d, VkSemaphore s, uint64_t *v) {
    (void)d, (void)s;
    *v = vk_sync_fence_done();
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    (void)q, (void)n, (void)s, (void)f;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue q) { (void)q; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) {
    (void)d, (void)n, (void)f, (void)all, (void)t;
    return VK_SUCCESS;
}
static VkCtx ctx;
VkCtx *vk_ctx(void) { return &ctx; }
static _Thread_local int shared_here;   // vk_exec_shared: this thread records a chunk beside others
int vk_exec_shared(void) { return shared_here; }

// The GPU reaches value v: the completions up to it run, then a collect frees what they retired.
static void complete_to(uint64_t v) {
    __atomic_store_n(&tl_done, v, __ATOMIC_RELEASE);
    for (;;) {
        Comp c = {0};
        pthread_mutex_lock(&comp_m);
        for (int i = 0; i < ncomps; i++)
            if (comps[i].fence <= v) { c = comps[i], comps[i] = comps[--ncomps]; break; }
        pthread_mutex_unlock(&comp_m);
        if (!c.fn) break;
        c.fn(c.arg);
    }
    vk_mem_collect();
}

// ---- the cases
#define BASE 0x200000000ull   // inside the guest address range the write watch covers (SNAP_REGIONS)
#define SIZE 0x200000ull

static PoolRec *make_pool(pool_t *key, uint64_t lo) {
    PoolRec *r = calloc(1, sizeof *r);
    r->key = key, r->kind = POOL_IMPORT, r->lo = r->import_lo = lo, r->size = SIZE, r->import_hi = lo + SIZE;
    r->buf = new_handle(), r->mem = (VkDeviceMemory)(uintptr_t)new_handle();
    key->mem = (void *)(uintptr_t)lo, key->size = SIZE;
    plat_mutex_lock(&pool_lock);
    index_put(r);
    plat_mutex_unlock(&pool_lock);
    return r;
}

// Mirror state as mirror_create leaves it, without its copies: on the list, synced at submission `at`.
static void give_mirror(PoolRec *r, uint32_t at) {
    plat_mutex_lock(&pool_lock);
    r->mbuf = new_handle(), r->malloc_ = (VmaAllocation)(uintptr_t)1, r->twin = 1;
    if (nm == mcap) grow((void **)&mlist, &mcap, sizeof *mlist);
    mlist[nm++] = r;
    r->acc_sub = at, r->wb_sub = 0, r->mstate = MS_ON;
    plat_mutex_unlock(&pool_lock);
}

static pthread_barrier_t bar;
static pool_t fini_key;
static int fini_clear_used;
static VkMemSpan fini_sp;

static void *fini_reader(void *arg) {
    (void)arg;
    if (!vk_mem_resolve(BASE + 0x100, 64, &fini_sp) || !fini_sp.pool) rt_fatal("the pool did not resolve");
    pthread_barrier_wait(&bar);   // the replay holds the record; the game finalizes the pool
    pthread_barrier_wait(&bar);
    // the rest of the binding, as bind_range does after its resolve: the mirror use and the twin
    VK_MEM_USE(fini_sp);
    volatile VkBuffer twin = vk_mem_guest_twin(&fini_sp);
    (void)twin;
    VkMemSpan again;
    return (void *)(intptr_t)vk_mem_resolve(BASE + 0x100, 64, &again);   // the index no longer has it
}

static int case_fini(int clear_used) {
    vk_mirror_on = 1;   // the use reads the record's mirror state
    PoolRec *r = make_pool(&fini_key, BASE);
    fini_clear_used = clear_used;
    pthread_barrier_init(&bar, NULL, 2);
    pthread_t t;
    pthread_create(&t, NULL, fini_reader, NULL);
    pthread_barrier_wait(&bar);
    if (clear_used) r->used = 0;   // the control: a record held without the used mark
    vk_pool_fini(&fini_key);
    pthread_barrier_wait(&bar);
    void *found;
    pthread_join(t, &found);
    // the record must still be there now, and go once the submission being recorded completes
    int gone_early = was_destroyed((uint64_t)(uintptr_t)fini_sp.buffer);
    complete_to(vk_sync_issued() + 1);
    int gone_after = was_destroyed((uint64_t)(uintptr_t)fini_sp.buffer);
    printf("fini%s: reader done, found again %d, buffer destroyed before the submission completed %d, after %d\n",
           clear_used ? " (control)" : "", (int)(intptr_t)found, gone_early, gone_after);
    return !found && !gone_early && gone_after ? 0 : 1;
}

// evict: submissions one after another (msub moves on by 200 between them, with no chunk recording, as at
// vk_mem_mirror_begin). In each, a worker uses pool A at every draw and pool B in every other submission, through
// vk_mem_use as a chunk does (use_lock on the slow path), while the other worker evicts the idle mirrors under
// use_lock (mirror_create's call). An evicted pool's next use makes its mirror again (the real mirror_create). Every
// use must get a buffer that is not destroyed while its submission records. The control: any mirror may go
// (min_idle 0) and the uses take the unlocked path; the "submission" then never ends.
static PoolRec *ev_a, *ev_b;
static volatile int ev_stop;
static uint64_t ev_uses, ev_null, ev_dead;
static int ev_control, ev_phase_b;

static void ev_use(PoolRec *r) {
    VkMemSpan sp = {.pool = r, .buffer = r->buf};
    vk_mem_use(&sp);
    __atomic_add_fetch(&ev_uses, 1, __ATOMIC_RELAXED);
    if (!sp.buffer) __atomic_add_fetch(&ev_null, 1, __ATOMIC_RELAXED);
    else if (was_destroyed((uint64_t)(uintptr_t)sp.buffer)) __atomic_add_fetch(&ev_dead, 1, __ATOMIC_RELAXED);
}

static void *ev_user(void *arg) {
    (void)arg;
    shared_here = !ev_control;
    if (ev_control) {
        while (!__atomic_load_n(&ev_stop, __ATOMIC_ACQUIRE)) ev_use(ev_a);
        return NULL;
    }
    for (;;) {
        pthread_barrier_wait(&bar);   // a submission begins
        if (__atomic_load_n(&ev_stop, __ATOMIC_ACQUIRE)) return NULL;
        for (int i = 0; i < 20000; i++) {
            ev_use(ev_a);
            if (ev_phase_b && i % 64 == 32) ev_use(ev_b);
        }
        pthread_barrier_wait(&bar);   // its chunks are done
    }
}

static int case_evict(int control) {
    vk_mirror_on = 1;
    evict_unlocked_on = 0;
    mirror_evict_on = 1;
    msub = 1000;
    static pool_t ka, kb;
    ev_a = make_pool(&ka, BASE + 4 * SIZE);
    ev_b = make_pool(&kb, BASE + 8 * SIZE);
    give_mirror(ev_a, msub);
    give_mirror(ev_b, msub);
    ev_control = control;
    pthread_barrier_init(&bar, NULL, 2);
    pthread_t t;
    pthread_create(&t, NULL, ev_user, NULL);
    shared_here = 1;
    uint64_t evicted = 0, rounds = 0, subs = 0;
    uint64_t t0 = now_ns();
    if (control) {
        while (now_ns() - t0 < 1500000000ull) {
            plat_mutex_lock(&use_lock);
            evicted += mirror_evict(1, 0, "test") ? 1 : 0;
            if (ev_a->mstate != MS_ON) give_mirror(ev_a, msub);
            plat_mutex_unlock(&use_lock);
            rounds++;
        }
    } else {
        for (int k = 0; k < 400; k++, subs++) {
            msub += 200;   // between submissions: no chunk records
            ev_phase_b = k & 1;
            pthread_barrier_wait(&bar);
            for (int j = 0; j < 200; j++) {   // evictions while the chunks record
                plat_mutex_lock(&use_lock);
                if (mirror_evict(2 * SIZE, MIRROR_IDLE, "test")) evicted++;
                plat_mutex_unlock(&use_lock);
                rounds++;
            }
            pthread_barrier_wait(&bar);
            __atomic_add_fetch(&tl_issued, 1, __ATOMIC_RELEASE);   // the submission goes to the GPU and completes
            complete_to(vk_sync_issued() + 1);
            plat_mutex_lock(&pool_lock);   // the next submission's copies: none in this harness
            npend = 0;
            plat_mutex_unlock(&pool_lock);
        }
    }
    __atomic_store_n(&ev_stop, 1, __ATOMIC_RELEASE);
    if (!control) pthread_barrier_wait(&bar);
    pthread_join(t, NULL);
    printf("evict%s: %llu submissions, %llu eviction calls, %llu evicted something, %llu uses; uses that got no buffer "
           "%llu, a destroyed one %llu\n", control ? " (control)" : "", (unsigned long long)subs,
           (unsigned long long)rounds, (unsigned long long)evicted, (unsigned long long)ev_uses,
           (unsigned long long)ev_null, (unsigned long long)ev_dead);
    if (!control && !evicted) return 2;   // the case must evict for its result to mean anything
    return ev_null || ev_dead ? 1 : 0;
}

static int child(int (*fn)(int), int arg, const char *name) {
    fflush(stdout);
    pid_t p = fork();
    if (!p) {
        int r = fn(arg);
        fflush(stdout);
        _exit(r);
    }
    int st;
    waitpid(p, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    printf("  %s: %s\n", name, code == 0 ? "held" : code == 1 ? "FAILED (wrong state)" : code == 2 ? "INCONCLUSIVE (nothing evicted)" : "FAILED (sanitizer or crash)");
    return code;
}

int main(void) {
    ctx.vma = (VmaAllocator)(uintptr_t)1;
    ctx.timeline = (VkSemaphore)(uintptr_t)new_handle();
    int a = child(case_fini, 0, "fini, the protocol as it is");
    int b = child(case_fini, 1, "fini, control: used mark cleared");
    int c = child(case_evict, 0, "evict, the protocol as it is");
    int d = child(case_evict, 1, "evict, control: no idle rule, unlocked use");
    printf("vkmem lifetime: protocol %s; controls %s\n", !a && !c ? "held" : "FAILED",
           b && d ? "reproduced the failures" : "did not reproduce");
    return !a && !c && b && d ? 0 : 1;
}
