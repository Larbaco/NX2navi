// runtime/ipc.c with runtime/kernel.c linked: what a CloseHandle racing a request does to the session and the
// transfer memory the request uses, with the platform layer linked and the rest of the runtime stubbed. Built and run
// by tests/ipc_session_check.py; prints each failed check and exits 1 on any.
//   session  a request whose handler blocks while another thread closes the session's only handle: the session's
//            close callback runs once, after the handler returned, never while it runs
//   tmem     kern_handle_copy hands out the transfer memory's fields while the handle is open, and 0 after its close
#include "ipc.c"

#include <stdarg.h>

static int fails;
#define T_CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the runtime ipc.c and kernel.c reach
unsigned rt_trace;
uint64_t rt_guest_start_ns;
volatile uint32_t prof_replay_tid, prof_main_tid;
const int rt_game_version = 111;
int mem_readable(uint64_t a, uint64_t n) { (void)a, (void)n; return 0; }
int mem_host_readable(uint64_t a, uint64_t n) { (void)a, (void)n; return 0; }
const char *cfg_get(const char *name) { (void)name; return NULL; }
const char *cfg_value(const char *name) { (void)name; return "0"; }
int cfg_bool(const char *name) { (void)name; return 0; }
int rt_ab(void) { return 0; }
int rt_knob(const char *name, const char *def) { (void)name; return strcmp(def, "0") != 0; }
int64_t frame_step_world_us(void) { return -1; }
const char *ldr_describe(uint64_t a, char *buf, int n) { snprintf(buf, (size_t)n, "%#llx", (unsigned long long)a); return buf; }
uint64_t ldr_symbol(const char *name) { (void)name; return 0; }
void ldr_load(const char *exefs_dir) { (void)exefs_dir; }
const RtLoaded *ldr_module(int i) { (void)i; return NULL; }
uint64_t ldr_plt_stub(const char *module, const char *name) { (void)module, (void)name; return 0; }
void perf_main_thread(void) {}
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    (void)s, (void)name, (void)count, (void)limit, (void)bytes;
}
int rt_linked_modules(RtModule *out, int max) { (void)out, (void)max; return 0; }
const RtGuestLayout *mem_layout(void) { return NULL; }
const RtConfig *rt_config(void) { return NULL; }
RtStats rt_stats(void) { return (RtStats){0, 0, 0}; }
void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    void *p = plat_aligned_alloc(size, align < 64 ? 64 : align);
    if (!p) rt_fatal("out of memory");
    memset(p, 0, size);
    return p;
}
void mem_kernel_free(void *p, uint64_t size) { (void)size; plat_aligned_free(p); }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_logf(const char *fmt, ...) { (void)fmt; }
void rt_log_raw(const char *p, size_t n) { (void)p, (void)n; }
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
void rt_dispatch(Cpu *c) { (void)c; rt_fatal("rt_dispatch in the test"); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)pc;
    if (!ipc_svc(c, imm)) kern_svc(c, imm);
}
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) { (void)c, (void)fn, (void)n; rt_fatal("rt_call in the test"); }
int ov_handheld(void) { return 0; }
long audren_renderers_live(void) { return 0; }
void audren_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) { (void)s, (void)rq, (void)rs; }
void audio_dmg_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) { (void)s, (void)rq, (void)rs; }
void audio_sysmix_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) { (void)s, (void)rq, (void)rs; }
void nfp_user_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) { (void)s, (void)rq, (void)rs; }

// ---- a service whose command 7 blocks until released, and whose state its close callback frees
typedef struct { volatile int alive, entered, release, done, closes, closed_while_running; } Svc;

static void blocking_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    Svc *v = s->state;
    if (rq->cmd == 7) {
        __atomic_store_n(&v->entered, 1, __ATOMIC_SEQ_CST);
        while (!__atomic_load_n(&v->release, __ATOMIC_SEQ_CST)) plat_wait_ns(200000);
        T_CHECK(v->alive, "the service's state is still alive when the handler resumes");
    }
    __atomic_store_n(&v->done, 1, __ATOMIC_SEQ_CST);
    rs->result = 0;
}

static void svc_close(void *state) {
    Svc *v = state;
    if (!__atomic_load_n(&v->done, __ATOMIC_SEQ_CST)) v->closed_while_running = 1;
    v->alive = 0;
    v->closes++;
}

typedef struct { Cpu cpu; uint32_t msg[IPC_MSG_BYTES / 4]; uint32_t h, result, kernel; } Client;

static void client_main(void *arg) {
    Client *cl = arg;
    cl->cpu.tpidrro = (uint64_t)(uintptr_t)cl->msg;
    Reply r;
    cl->kernel = cmd(&cl->cpu, cl->h, 7, NULL, 0, &r);
    cl->result = r.result;
}

static void session_check(void) {
    long live0 = ipc_sessions_live();
    static Svc v;
    v.alive = 1;
    static Client cl;
    cl.h = ipc_session_new("blocking", blocking_handler, &v, svc_close);
    void *t = plat_thread_new(client_main, &cl, "client");
    while (!__atomic_load_n(&v.entered, __ATOMIC_SEQ_CST)) plat_wait_ns(200000);
    kern_handle_close(cl.h);   // the session's only handle, closed while its request runs
    plat_wait_ns(50000000);
    T_CHECK(v.closes == 0, "the close callback waits for the running request (ran %d times)", v.closes);
    T_CHECK(v.alive, "the service's state outlives the CloseHandle while a request runs");
    __atomic_store_n(&v.release, 1, __ATOMIC_SEQ_CST);
    plat_thread_join(t);
    T_CHECK(cl.kernel == 0 && cl.result == 0, "the request is answered (kernel %#x, result %#x)", cl.kernel, cl.result);
    T_CHECK(v.closes == 1, "the close callback runs once after the request (ran %d times)", v.closes);
    T_CHECK(!v.closed_while_running, "the close callback never runs while the handler does");
    T_CHECK(ipc_sessions_live() == live0, "the session is freed (%ld live, %ld before)", ipc_sessions_live(), live0);
    // A request on an open session leaves it open; the CloseHandle afterwards frees it.
    static Svc w;
    w.alive = 1, w.release = 1;
    static Client c2;
    c2.h = ipc_session_new("open", blocking_handler, &w, svc_close);
    client_main(&c2);
    T_CHECK(c2.kernel == 0 && w.closes == 0, "a request leaves an open session open (closes %d)", w.closes);
    kern_handle_close(c2.h);
    T_CHECK(w.closes == 1 && ipc_sessions_live() == live0, "its CloseHandle frees it (closes %d)", w.closes);
}

static void tmem_check(void) {
    IpcTransferMemory *t = malloc(sizeof *t);
    *t = (IpcTransferMemory){0x80000000ull, 0x10000, 3};
    uint32_t h = kern_handle_new(KERN_TMEM, t);
    IpcTransferMemory copy = {0};
    T_CHECK(kern_handle_copy(h, KERN_TMEM, &copy, sizeof copy) && copy.addr == 0x80000000ull && copy.size == 0x10000,
            "the transfer memory is copied while its handle is open");
    T_CHECK(!kern_handle_copy(h, KERN_SESSION, &copy, sizeof copy), "a handle of another kind copies nothing");
    kern_handle_close(h);
    T_CHECK(!kern_handle_copy(h, KERN_TMEM, &copy, sizeof copy), "a closed transfer memory handle copies nothing");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    session_check();
    tmem_check();
    printf(fails ? "ipc_session_check: %d failed\n" : "ipc_session_check: ok\n", fails);
    return fails ? 1 : 0;
}
