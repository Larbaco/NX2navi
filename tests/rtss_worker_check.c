// The real runtime/rtss_limit.c against a fake RivaTuner Statistics Server: the Win32 calls it makes go to the fakes
// below (its folder in the registry, its hooks library with the profile calls, the profiles, the shared memory
// RTSSSharedMemoryV2), and its worker thread polls when a scenario says so (plat_sleep_ms on that thread waits for the
// next poll). Each scenario runs in a process of its own, since rtss_limit.c's state lasts the process. Windows only;
// built and run by tests/rtss_limit_check.py. Prints the log and each failed check, and exits 1 on any.
//
// rtss_worker_check <scenario>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LSTATUS stub_RegGetValueA(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
HMODULE stub_LoadLibraryA(LPCSTR);
FARPROC stub_GetProcAddress(HMODULE, LPCSTR);
BOOL stub_FreeLibrary(HMODULE);
HANDLE stub_OpenFileMappingA(DWORD, BOOL, LPCSTR);
LPVOID stub_MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
BOOL stub_UnmapViewOfFile(LPCVOID);
SIZE_T stub_VirtualQuery(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
BOOL stub_CloseHandle(HANDLE);
DWORD stub_GetFileAttributesA(LPCSTR);
DWORD stub_GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DWORD stub_GetCurrentProcessId(void);
#define RegGetValueA stub_RegGetValueA
#define LoadLibraryA stub_LoadLibraryA
#define GetProcAddress stub_GetProcAddress
#define FreeLibrary stub_FreeLibrary
#define OpenFileMappingA stub_OpenFileMappingA
#define MapViewOfFile stub_MapViewOfFile
#define UnmapViewOfFile stub_UnmapViewOfFile
#define VirtualQuery stub_VirtualQuery
#define CloseHandle stub_CloseHandle
#define GetFileAttributesA stub_GetFileAttributesA
#define GetModuleFileNameA stub_GetModuleFileNameA
#define GetCurrentProcessId stub_GetCurrentProcessId

#include "rtss_limit.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the fake profiles: Global, and the game's own Profiles\ocarina.exe.cfg; LoadProfile copies one into the loaded copy
typedef struct { int exists; DWORD limit, denom; } Prof;
static Prof global = {1, 0, 1}, own;
static DWORD loaded_limit, loaded_denom;
static int no_denominator;   // an RTSS that does not know FramerateLimitDenominator
static int readonly;         // SaveProfile does not reach the file (no administrator rights)
static volatile LONG saves, deletes, updates, loads, created, removed;

static void WINAPI f_load(LPCSTR name) {
    (void)name;
    const Prof *p = own.exists ? &own : &global;
    loaded_limit = p->limit, loaded_denom = p->denom;
}
static void WINAPI f_save(LPCSTR name) {
    (void)name;
    saves++;
    if (!readonly) own.exists = 1, own.limit = loaded_limit, own.denom = loaded_denom;
}
static void WINAPI f_delete(LPCSTR name) {
    (void)name;
    deletes++;
    if (!readonly) own.exists = 0;
}
static BOOL WINAPI f_get(LPCSTR prop, LPBYTE data, DWORD size) {
    if (size != 4) return FALSE;
    if (!strcmp(prop, "FramerateLimit")) return memcpy(data, &loaded_limit, 4), TRUE;
    if (!strcmp(prop, "FramerateLimitDenominator") && !no_denominator) return memcpy(data, &loaded_denom, 4), TRUE;
    return FALSE;
}
static BOOL WINAPI f_set(LPCSTR prop, LPBYTE data, DWORD size) {
    if (size != 4) return FALSE;
    if (!strcmp(prop, "FramerateLimit")) return memcpy(&loaded_limit, data, 4), TRUE;
    if (!strcmp(prop, "FramerateLimitDenominator") && !no_denominator) return memcpy(&loaded_denom, data, 4), TRUE;
    return FALSE;
}
static void WINAPI f_update(void) { updates++; }
static DWORD WINAPI f_enum(LPSTR list, DWORD size) { (void)size; list[0] = 0; return 1; }

// ---- the fake shared memory: the header and one application entry, this process's (listed) or another's
static uint8_t shm[65536];
static volatile LONG running = 1;
enum { PID = 4242, ENTRY_AT = 0x100, ENTRY_SIZE = 1024 };
static void app_set(int listed, DWORD flags, DWORD t0, DWORD t1, DWORD frames) {
    DWORD h[5] = {0x52545353u, 0x0002000Eu, ENTRY_SIZE, ENTRY_AT, 1};
    memcpy(shm, h, sizeof h);
    uint8_t *e = shm + ENTRY_AT;
    DWORD pid = listed ? PID : 7;
    DWORD t[3] = {t0, t1, frames};
    memcpy(e, &pid, 4);
    memcpy(e + 4 + MAX_PATH, &flags, 4);
    memcpy(e + 4 + MAX_PATH + 4, t, sizeof t);
}

LSTATUS stub_RegGetValueA(HKEY k, LPCSTR sub, LPCSTR val, DWORD fl, LPDWORD ty, PVOID data, LPDWORD size) {
    (void)k, (void)sub, (void)val, (void)fl, (void)ty;
    snprintf(data, *size, "C:\\FakeRTSS");
    return ERROR_SUCCESS;
}
HMODULE stub_LoadLibraryA(LPCSTR p) {
    (void)p;
    loads++;
    return (HMODULE)(uintptr_t)0x1000;
}
FARPROC stub_GetProcAddress(HMODULE m, LPCSTR n) {
    (void)m;
    if (!strcmp(n, "EnumProfiles")) return (FARPROC)(void *)f_enum;
    if (!strcmp(n, "LoadProfile")) return (FARPROC)(void *)f_load;
    if (!strcmp(n, "SaveProfile")) return (FARPROC)(void *)f_save;
    if (!strcmp(n, "DeleteProfile")) return (FARPROC)(void *)f_delete;
    if (!strcmp(n, "GetProfileProperty")) return (FARPROC)(void *)f_get;
    if (!strcmp(n, "SetProfileProperty")) return (FARPROC)(void *)f_set;
    if (!strcmp(n, "UpdateProfiles")) return (FARPROC)(void *)f_update;
    return NULL;
}
BOOL stub_FreeLibrary(HMODULE m) { (void)m; return TRUE; }
HANDLE stub_OpenFileMappingA(DWORD a, BOOL b, LPCSTR n) {
    (void)a, (void)b, (void)n;
    return running ? (HANDLE)(uintptr_t)0x2000 : NULL;
}
LPVOID stub_MapViewOfFile(HANDLE h, DWORD a, DWORD b, DWORD c, SIZE_T d) {
    (void)h, (void)a, (void)b, (void)c, (void)d;
    return shm;
}
BOOL stub_UnmapViewOfFile(LPCVOID p) { (void)p; return TRUE; }
SIZE_T stub_VirtualQuery(LPCVOID p, PMEMORY_BASIC_INFORMATION mi, SIZE_T n) {
    (void)p;
    memset(mi, 0, n);
    mi->RegionSize = sizeof shm;
    return n;
}
BOOL stub_CloseHandle(HANDLE h) { (void)h; return TRUE; }
DWORD stub_GetFileAttributesA(LPCSTR p) {
    if (strstr(p, "RTSSHooks64.dll")) return FILE_ATTRIBUTE_NORMAL;
    if (strstr(p, "\\Profiles\\ocarina.exe.cfg")) return own.exists ? FILE_ATTRIBUTE_NORMAL : INVALID_FILE_ATTRIBUTES;
    return INVALID_FILE_ATTRIBUTES;
}
DWORD stub_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) {
    (void)m;
    return (DWORD)snprintf(out, n, "C:\\game\\ocarina.exe");
}
DWORD stub_GetCurrentProcessId(void) { return PID; }

// ---- host.h and platform.h, as far as rtss_limit.c uses them
static char last_line[512];
void rt_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, " created;")) created++;
    if (strstr(line, " removed,")) removed++;
    snprintf(last_line, sizeof last_line, "%s", line);
    printf("    log: %s\n", line);
}
int rt_knob(const char *name, const char *def) { (void)name, (void)def; return 0; }

static DWORD worker_tid;
static HANDLE tick, done, at_lock, go;
static volatile LONG pause_at_lock;   // the worker's next plat_mutex_lock stops before it takes the lock

void plat_mutex_lock(PlatMutex *m) {
    if (GetCurrentThreadId() == worker_tid && InterlockedExchange(&pause_at_lock, 0)) {
        SetEvent(at_lock);
        WaitForSingleObject(go, INFINITE);
    }
    AcquireSRWLockExclusive((PSRWLOCK)m);
}
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) != 0; }

static PlatThreadFn worker_fn;
static volatile LONG spawned;
static DWORD WINAPI worker_main(void *arg) {
    worker_tid = GetCurrentThreadId();
    worker_fn(arg);
    return 0;
}
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    (void)name, (void)stack;
    worker_fn = fn;
    spawned = 1;
    return CreateThread(NULL, 0, worker_main, arg, 0, NULL) != NULL;
}
void plat_sleep_ms(uint32_t ms) {
    if (GetCurrentThreadId() != worker_tid) {
        Sleep(ms);
        return;
    }
    SetEvent(done);                      // a poll ended
    WaitForSingleObject(tick, INFINITE);   // the next one starts when the scenario says
}

// ---- the scenarios' steps
static int wait_done(void) { return WaitForSingleObject(done, 5000) == WAIT_OBJECT_0; }
static void poll(void) {   // one more poll of the worker, waited for
    SetEvent(tick);
    CHECK(wait_done(), "the worker did not finish a poll");
}
static void ask(int64_t mhz) {   // a want at a present; the first one that starts the worker waits for its first poll
    int was = spawned;
    rtss_limit_want(mhz);
    if (spawned && !was) CHECK(wait_done(), "the worker did not finish its first poll");
}
static void show(const char *what) {
    printf("  %s: profile %s %lu/%lu; active %d; state %d; saves %ld deletes %ld updates %ld\n", what,
           own.exists ? "exists," : "absent,", own.limit, own.denom, rtss_limit_active(), rtss_limit_state(), saves,
           deletes, updates);
}

// The exit puts the profile back (rtss_limit_restore) and the game presents on until the process ends: a want after it
// never reaches the profile, and the pacer paces again.
static void restore_race(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    show("playing at 60");
    CHECK(own.exists && own.limit == 60, "the limit went into the profile");
    rtss_limit_restore();
    show("after rtss_limit_restore");
    CHECK(!own.exists, "the profile the game created is gone after the restore");
    CHECK(!rtss_limit_active() && !rtss_limit_state(), "the game's limiter paces once the profile is back");
    ask(60000);   // frame_step_cap_ns at a present after the restore
    poll();
    poll();
    show("two polls later");
    CHECK(!own.exists, "the profile stays as it was put back");
    CHECK(created == 1, "the profile was created %ld times", created);
    CHECK(!rtss_limit_active() && !rtss_limit_state(), "the game's limiter paces until the end");
}

// The same with the player's own profile: it keeps the player's limit.
static void restore_player(void) {
    own = (Prof){1, 30, 1};
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    CHECK(own.limit == 60, "the game's limit went into the profile");
    rtss_limit_restore();
    CHECK(own.exists && own.limit == 30, "the player's limit is back after the restore");
    ask(144000);
    poll();
    show("a want and a poll after the restore");
    CHECK(own.exists && own.limit == 30 && own.denom == 1, "the profile keeps the player's limit");
}

// A want the worker read before the restore and takes the lock with after it: it must not reach the profile.
static void restore_inflight(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    pause_at_lock = 1;
    rtss_limit_want(144000);
    SetEvent(tick);
    CHECK(WaitForSingleObject(at_lock, 5000) == WAIT_OBJECT_0, "the worker did not reach its lock");
    rtss_limit_restore();   // the worker read 144000 and waits for the lock
    SetEvent(go);
    CHECK(wait_done(), "the worker did not finish the poll");
    show("the poll that read its want before the restore");
    CHECK(!own.exists, "the profile stays as it was put back");
    CHECK(created == 1, "the profile was created %ld times", created);
}

// An exit before the first want, and one while RTSS was kept out: a want after it does not start the worker.
static void restore_first(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    rtss_limit_restore();
    ask(60000);
    if (spawned) poll();
    show("a want after an exit before the first one");
    CHECK(!own.exists && !created, "no profile written after the exit");
    CHECK(!rtss_limit_active(), "the game's limiter paces");
}
static void restore_kept_out(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(-1);
    rtss_limit_restore();
    ask(60000);
    if (spawned) poll();
    show("a want after an exit while RTSS was kept out");
    CHECK(!own.exists && !created, "no profile written after the exit");
    CHECK(!rtss_limit_active(), "the game's limiter paces");
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"restore_race", restore_race},   {"restore_player", restore_player}, {"restore_inflight", restore_inflight},
    {"restore_first", restore_first}, {"restore_kept_out", restore_kept_out},
};

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    tick = CreateEventA(NULL, FALSE, FALSE, NULL), done = CreateEventA(NULL, FALSE, FALSE, NULL);
    at_lock = CreateEventA(NULL, FALSE, FALSE, NULL), go = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; i++) printf("%s\n", scenarios[i].name);
        return 0;
    }
    for (size_t i = 0; argc == 2 && i < sizeof scenarios / sizeof *scenarios; i++) {
        if (strcmp(argv[1], scenarios[i].name)) continue;
        printf("scenario %s\n", argv[1]);
        scenarios[i].run();
        return fails != 0;
    }
    fprintf(stderr, "usage: rtss_worker_check --list | <scenario>\n");
    return 2;
}
