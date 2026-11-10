// The DualShock 3 reader of the Windows build (input_ds3.h): the controller's HID reports read directly with hid.dll.
// The controller sends nothing until it is told to (a feature report, sent on connect), and SDL and XInput get nothing
// from it without a driver that does this and presents it as a gamepad; with one, SDL or XInput already see it and this
// file is only a second reader of the same pad (input.c lets the SDL pad win then, see pad_read).
//
// hid.dll and setupapi.dll are loaded at run time, so the exe links no extra library and starts without them. A scan
// for the controller runs on a thread of its own every SCAN_MS while none is open: walking every HID device takes
// milliseconds, which the game's poll must never wait for. Everything else (open, read, rumble, close) runs on the
// caller's thread, under input.c's lock.
//
// The report formats are in input_ds3.c. The accelerometer and the single (yaw) gyro are not read: the game's six-axis
// sensor needs all three gyro axes.
#include <windows.h>
#include <setupapi.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "input_ds3.h"
#include "platform/platform.h"

enum {
    VID_SONY = 0x054C, PID_DS3 = 0x0268,
    SCAN_MS = 3000,           // between scans while no controller is open
    OPEN_RETRY_MS = 1000,     // between attempts to open a path the scan found
    SILENT_NS = 2000000000,   // no report this long: not reporting (the read stays queued, a report revives it)
    PATH_MAX_LEN = 512,
};

typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } HidAttr;   // HIDD_ATTRIBUTES
typedef struct { USHORT Usage, UsagePage, InputLen, OutputLen, FeatureLen; USHORT Rest[29]; } HidCaps;   // HIDP_CAPS (64 bytes)

static struct {
    void (__stdcall *GetHidGuid)(GUID *);
    BOOLEAN (__stdcall *GetAttributes)(HANDLE, HidAttr *);
    BOOLEAN (__stdcall *GetProductString)(HANDLE, void *, ULONG);
    BOOLEAN (__stdcall *GetPreparsed)(HANDLE, void **);
    BOOLEAN (__stdcall *FreePreparsed)(void *);
    LONG (__stdcall *GetCaps)(void *, HidCaps *);   // NTSTATUS, 0x110000 on success
    BOOLEAN (__stdcall *SetFeature)(HANDLE, void *, ULONG);
    BOOLEAN (__stdcall *SetOutput)(HANDLE, void *, ULONG);
    HDEVINFO (__stdcall *ClassDevs)(const GUID *, PCSTR, HWND, DWORD);
    BOOL (__stdcall *EnumInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
    BOOL (__stdcall *InterfaceDetail)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD,
                                      PDWORD, PSP_DEVINFO_DATA);
    BOOL (__stdcall *DestroyList)(HDEVINFO);
} api;
static int api_state;   // 0 not tried, 1 loaded, -1 unavailable

static int api_load(void) {
    if (api_state) return api_state > 0;
    api_state = -1;
    HMODULE hid = LoadLibraryA("hid.dll"), sa = LoadLibraryA("setupapi.dll");
    if (!hid || !sa) return 0;
#define LOAD(m, field, name) *(FARPROC *)&api.field = GetProcAddress(m, name)
    LOAD(hid, GetHidGuid, "HidD_GetHidGuid");
    LOAD(hid, GetAttributes, "HidD_GetAttributes");
    LOAD(hid, GetProductString, "HidD_GetProductString");
    LOAD(hid, GetPreparsed, "HidD_GetPreparsedData");
    LOAD(hid, FreePreparsed, "HidD_FreePreparsedData");
    LOAD(hid, GetCaps, "HidP_GetCaps");
    LOAD(hid, SetFeature, "HidD_SetFeature");
    LOAD(hid, SetOutput, "HidD_SetOutputReport");
    LOAD(sa, ClassDevs, "SetupDiGetClassDevsA");
    LOAD(sa, EnumInterfaces, "SetupDiEnumDeviceInterfaces");
    LOAD(sa, InterfaceDetail, "SetupDiGetDeviceInterfaceDetailA");
    LOAD(sa, DestroyList, "SetupDiDestroyDeviceInfoList");
#undef LOAD
    if (!api.GetHidGuid || !api.GetAttributes || !api.GetPreparsed || !api.FreePreparsed || !api.GetCaps ||
        !api.SetFeature || !api.SetOutput || !api.ClassDevs || !api.EnumInterfaces || !api.InterfaceDetail ||
        !api.DestroyList)
        return 0;
    api_state = 1;
    return 1;
}

// ---- the scan (its own thread)
static char found_path[PATH_MAX_LEN];     // written by the scan thread, then found set; read by the poll
static volatile int32_t found;            // found_path holds a controller
static volatile int32_t open_now;         // the poll holds a controller open: the scan rests
static volatile int32_t scan_on;          // NX2NAVI_DS3 is on (default off): the scan runs
static volatile int32_t scan_started;

// The path of the first DualShock 3 among the HID devices that are present, copied into out; 0 when there is none.
static int scan_once(char *out, size_t cap) {
    GUID g;
    api.GetHidGuid(&g);
    HDEVINFO set = api.ClassDevs(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int hit = 0;
    for (DWORD i = 0; !hit; i++) {
        SP_DEVICE_INTERFACE_DATA d;
        memset(&d, 0, sizeof d);
        d.cbSize = sizeof d;
        if (!api.EnumInterfaces(set, NULL, &g, i, &d)) break;
        DWORD need = 0;
        api.InterfaceDetail(set, &d, NULL, 0, &need, NULL);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) || need > 4096) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)HeapAlloc(GetProcessHeap(), 0, need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (api.InterfaceDetail(set, &d, det, need, NULL, NULL) && strlen(det->DevicePath) < cap) {
            // access 0: enough to ask for the ids, and it works on a device another program has open
            HANDLE h = CreateFileA(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HidAttr at = {sizeof at};
                if (api.GetAttributes(h, &at) && at.VendorID == VID_SONY && at.ProductID == PID_DS3) {
                    strcpy(out, det->DevicePath);
                    hit = 1;
                }
                CloseHandle(h);
            }
        }
        HeapFree(GetProcessHeap(), 0, det);
    }
    api.DestroyList(set);
    return hit;
}

static void scan_thread(void *arg) {
    (void)arg;
    for (;;) {
        if (scan_on && !open_now && !found) {
            char p[PATH_MAX_LEN];
            if (scan_once(p, sizeof p)) {
                memcpy(found_path, p, sizeof p);
                plat_xchg32(&found, 1);
            }
        }
        plat_sleep_ms(SCAN_MS);
    }
}

// ---- the open controller (the poll's thread)
static struct {
    HANDLE h;
    HANDLE ev;                       // the read's event, made once
    OVERLAPPED ov;
    int pending;                     // a ReadFile is queued
    uint8_t buf[128];                // its buffer
    unsigned in_len, out_len, feat_len;
    uint8_t pd[PAD_COUNT];           // the newest report
    int32_t raw[4];
    uint64_t last_report;            // plat_now_ns of the newest valid report, 0 none yet
    int reported;                    // logged: a report came in
    uint64_t next_open;
    int motors_on;                   // the controller's motors are driven now
    int sent_left, sent_right;       // the last output report's motors
    uint64_t sent_at, until;         // when it went out; when the rumble ends (plat_now_ns), 0 none
} s;

static void drop(const char *why) {
    if (!s.h) return;
    rt_log("input: DualShock 3 %s", why);
    CancelIo(s.h);
    CloseHandle(s.h);
    memset(&s.h, 0, sizeof s.h);   // h only: the buffers stay where a cancelled read may still write
    s.pending = 0;
    s.last_report = 0;
    s.reported = 0;
    s.motors_on = 0;
    s.sent_left = s.sent_right = 0;
    s.until = 0;
    plat_xchg32(&open_now, 0);
}

// Output report 0x01: motors and the player light. left 0..255 (heavy), right 0 or 1 (light), dur in 10 ms steps.
static int send_output(int left, int right, unsigned dur, int led) {
    uint8_t o[64];
    unsigned n = s.out_len ? s.out_len : 49;
    if (n > sizeof o) n = sizeof o;
    ds3_output_report(o, n, left, right, dur, led);
    return api.SetOutput(s.h, o, n) != 0;
}

static void try_open(void) {
    uint64_t now = plat_now_ns();
    if (now < s.next_open || !plat_cas32(&found, 0, 1)) return;   // nothing found, or too soon
    s.next_open = now + (uint64_t)OPEN_RETRY_MS * 1000000u;
    char path[PATH_MAX_LEN];
    memcpy(path, found_path, sizeof path);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        rt_log("input: DualShock 3 found but not opened (error %lu)", (unsigned long)GetLastError());
        return;
    }
    // A pad that presents itself in another report layout (a driver's generic mode) would read as garbage here
    wchar_t name[128] = {0};
    if (api.GetProductString && api.GetProductString(h, name, sizeof name - sizeof *name)) {
        for (wchar_t *c = name; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        if (!wcsstr(name, L"PLAYSTATION")) {
            rt_log("input: HID device \"%ls\" has the DualShock 3's ids but not its name; left to SDL", name);
            CloseHandle(h);
            return;
        }
    }
    HidCaps caps;
    void *pre = NULL;
    memset(&caps, 0, sizeof caps);
    if (!api.GetPreparsed(h, &pre)) { CloseHandle(h); return; }
    LONG st = api.GetCaps(pre, &caps);
    api.FreePreparsed(pre);
    if (st != 0x110000 || caps.InputLen < 10 || caps.InputLen > sizeof s.buf) {
        rt_log("input: DualShock 3 reports are not as expected (input %u bytes); left to SDL", caps.InputLen);
        CloseHandle(h);
        return;
    }
    s.h = h;
    s.in_len = caps.InputLen, s.out_len = caps.OutputLen, s.feat_len = caps.FeatureLen;
    if (!s.ev) s.ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    s.pending = 0;
    plat_xchg32(&open_now, 1);
    // Reports start only after this: feature report 0xF4, "start sending"
    uint8_t en[64] = {0xF4, 0x42, 0x03, 0x00, 0x00};
    unsigned fl = s.feat_len > 5 && s.feat_len <= sizeof en ? s.feat_len : 5;
    int ok = api.SetFeature(h, en, fl) != 0;
    if (!ok && fl != 5) ok = api.SetFeature(h, en, 5) != 0;
    rt_log("input: DualShock 3 opened (reports %u bytes, start command %s)", s.in_len, ok ? "accepted" : "refused");
    send_output(0, 0, 0, 1);   // player 1's light: shows that the controller is ours
}

static void parse(const uint8_t *r, unsigned n) {
    if (!ds3_decode(r, n, s.pd, s.raw)) return;
    s.last_report = plat_now_ns();
    if (!s.reported) s.reported = 1, rt_log("input: DualShock 3 is reporting");
}

// Reads every report that came in since the last poll; the newest wins.
static void pump(void) {
    for (int guard = 0; guard < 64 && s.h; guard++) {
        if (!s.pending) {
            memset(&s.ov, 0, sizeof s.ov);
            s.ov.hEvent = s.ev;
            ResetEvent(s.ev);
            if (!ReadFile(s.h, s.buf, s.in_len, NULL, &s.ov) && GetLastError() != ERROR_IO_PENDING) {
                drop("disconnected");
                return;
            }
            s.pending = 1;
        }
        DWORD n = 0;
        if (!GetOverlappedResult(s.h, &s.ov, &n, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) return;
            drop("disconnected");
            return;
        }
        s.pending = 0;
        parse(s.buf, n);
    }
}

// Ends a rumble whose time is up (the controller stops by itself after the duration it was given; this is the same
// moment from our side, so the next rumble starts from a known state).
static void rumble_expire(void) {
    if (s.until && plat_now_ns() >= s.until) {
        s.until = 0;
        if (s.motors_on) s.motors_on = 0, s.sent_left = s.sent_right = 0, send_output(0, 0, 0, 1);
    }
}

int ds3_read(uint8_t pd[PAD_COUNT], int32_t raw[4]) {
    static unsigned seen = ~0u;
    unsigned ver = cfg_version();
    if (ver != seen) {   // live: the setting is read again when one changed
        seen = ver;
        int on = cfg_bool("NX2NAVI_DS3");   // off by default (cfg.c metas): the reader opens the pad read-write
        if (!on && s.h) drop("closed (ds3 = 0)");
        plat_xchg32(&scan_on, on);
    }
    if (!scan_on || !api_load()) return 0;
    if (!scan_started) {
        scan_started = 1;
        plat_thread_spawn(scan_thread, NULL, "ds3-scan", 1u << 18);
    }
    if (!s.h) try_open();
    if (!s.h) return 0;
    pump();
    rumble_expire();
    if (!s.h || !s.last_report || plat_now_ns() - s.last_report > SILENT_NS) return 0;
    memcpy(pd, s.pd, PAD_COUNT);
    memcpy(raw, s.raw, sizeof s.raw);
    return 1;
}

int ds3_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    if (!s.h || !s.last_report) return 0;
    int left = low >> 8, right = high >= 0x1000;
    if (!ms) left = right = 0;
    uint64_t now = plat_now_ns();
    // Resend when a motor changed or the controller's own timer is about to run out; the game renews a rumble every
    // 50 ms, which is more often than the controller needs
    unsigned dur = ms / 10 + 5 > 255 ? 255 : ms / 10 + 5;
    int changed = left != s.sent_left || right != s.sent_right;
    if (!changed && now - s.sent_at < (uint64_t)ms * 500000u) {
        s.until = ms ? now + (uint64_t)ms * 1000000u : 0;
        return 1;
    }
    if (!send_output(left, right, ms ? dur : 0, 1)) return 0;
    s.sent_left = left, s.sent_right = right, s.sent_at = now;
    s.motors_on = left || right;
    s.until = ms && s.motors_on ? now + (uint64_t)ms * 1000000u : 0;
    return 1;
}
