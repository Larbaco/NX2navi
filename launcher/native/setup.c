/* NX2navi-Setup.exe: the one-click setup. This program with the launcher's folder appended as a zip.

   It unpacks the zip into %LOCALAPPDATA%\Programs\NX2navi (no administrator rights), adds the Start menu and Desktop
   shortcuts and the entry in Settings > Apps, and starts NX2navi.exe with --from <the setup's folder>, where the
   launcher looks for the player's dump first. A small window in the launcher's colours shows while it unpacks.
   When the folder already holds a newer version than the setup carries, the setup leaves the files alone (the
   launcher updates itself) and only repairs the shortcuts.

   The end of the file is a trailer: "NX2NSET1", the zip's offset and size (u64 little endian) and the version (24
   bytes, NUL padded), written by tools/make_launcher.py. The zip is read here with zlib's inflate, so the setup
   needs nothing from Windows beyond its own DLLs.

   Options: --dest DIR, --no-shortcuts, --no-launch, --quiet (no window; for tests). */
#include "common.h"
#include <stdint.h>
#include <stdlib.h>
#include <zlib.h>

#define TRAILER 48
#define WM_DONE (WM_APP + 1)

typedef struct {
    wchar_t exe[PATHN], dest[PATHN], src_dir[PATHN];
    uint64_t off, size;
    wchar_t version[32];
    int shortcuts, launch, quiet;
    int result;            /* 0 ok, else an error message id */
    wchar_t error[512];
    uint64_t written;
} Setup;

static Setup S;
static HWND g_wnd;

/* Navi as launcher/ui/art.py NAVI draws it: W white, g grey, K black, B and L blue, '.' transparent. */
static const char *const NAVI[21] = {
    "WWWW..............WWWW", "WggWW............WWggW", "WgggWW..........WWgggW", "WgWggWW........WWggWgW",
    "WggWggW........WggWggW", "WgWgWgWW......WWgWgWgW", "WggggWgWKKKKKKWgWggggW", "WWggWggKBBBBBBKggWggWW",
    ".WggggKKBBLLBBKKggggW.", ".WWggKKBBLLLLBBKKggWW.", "..WWgKBBLLWWLLBBKgWW..", "...WWKBBLWWWWLBBKWW...",
    ".....KBBLLWWLLBBK.....", ".....KBBBLLLLBBBK.....", ".....WKKBBBBBBKKW.....", "....WWgKKBBBBKKgWW....",
    "...WWgggKKKKKKgggWW...", "...WggWgW....WgWggW...", "...WgWggW....WggWgW...", "...WWggWW....WWggWW...",
    "....WWWW......WWWW....",
};

static void fail_msg(const wchar_t *fmt, const wchar_t *arg)
{
    swprintf(S.error, 512, fmt, arg ? arg : L"");
    S.result = 1;
}

/* ---------------------------------------------------------------- reading our own file */

typedef struct { HANDLE f; uint64_t base, size; } Src;

static int src_read(Src *s, uint64_t at, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    if (at + n > s->size) return 0;
    li.QuadPart = (LONGLONG)(s->base + at);
    if (!SetFilePointerEx(s->f, li, NULL, FILE_BEGIN)) return 0;
    return ReadFile(s->f, buf, n, &got, NULL) && got == n;
}

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static int read_trailer(void)
{
    unsigned char t[TRAILER];
    HANDLE f = CreateFileW(S.exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size, li;
    DWORD got = 0;
    int ok = GetFileSizeEx(f, &size) && size.QuadPart > TRAILER;
    if (ok) {
        li.QuadPart = size.QuadPart - TRAILER;
        ok = SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, t, TRAILER, &got, NULL) && got == TRAILER &&
             !memcmp(t, "NX2NSET1", 8);
    }
    CloseHandle(f);
    if (!ok) return 0;
    S.off = le64(t + 8);
    S.size = le64(t + 16);
    char v[25];
    memcpy(v, t + 24, 24);
    v[24] = 0;
    MultiByteToWideChar(CP_UTF8, 0, v, -1, S.version, 32);
    return S.off + S.size <= (uint64_t)size.QuadPart;
}

/* ---------------------------------------------------------------- unzip */

static int make_dirs(wchar_t *path)   /* every folder above path (path is a file) */
{
    for (wchar_t *p = path + 3; *p; p++) {
        if (*p == L'\\') {
            *p = 0;
            if (!is_dir(path) && !CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                *p = L'\\';
                return 0;
            }
            *p = L'\\';
        }
    }
    return 1;
}

static int safe_name(const char *name)
{
    if (!name[0] || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) return 0;
    for (const char *p = name; *p; p++)
        if ((p == name || p[-1] == '/' || p[-1] == '\\') && p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\\' || !p[2]))
            return 0;
    return 1;
}

static int extract_entry(Src *z, uint64_t local, uint32_t csize, uint32_t usize, uint16_t method, uint32_t crc,
                         const wchar_t *target)
{
    unsigned char lh[30];
    if (!src_read(z, local, lh, 30) || le32(lh) != 0x04034b50) return 0;
    uint64_t data = local + 30 + le16(lh + 26) + le16(lh + 28);
    HANDLE out = CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return 0;
    static unsigned char in[1 << 16], buf[1 << 17];
    uLong check = crc32(0L, Z_NULL, 0);
    int ok = 1;
    uint64_t left = csize, pos = data, total = 0;
    z_stream st;
    memset(&st, 0, sizeof st);
    if (method == 8 && inflateInit2(&st, -MAX_WBITS) != Z_OK) ok = 0;
    while (ok && (left || (method == 8 && st.avail_in))) {
        DWORD n = (DWORD)(left < sizeof in ? left : sizeof in);
        if (n && !src_read(z, pos, in, n)) { ok = 0; break; }
        pos += n;
        left -= n;
        if (method == 0) {
            DWORD put;
            check = crc32(check, in, n);
            ok = WriteFile(out, in, n, &put, NULL) && put == n;
            total += n;
            continue;
        }
        st.next_in = in;
        st.avail_in = n;
        do {
            st.next_out = buf;
            st.avail_out = sizeof buf;
            int r = inflate(&st, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END && !(r == Z_BUF_ERROR && st.avail_in == 0)) { ok = 0; break; }
            DWORD have = (DWORD)(sizeof buf - st.avail_out), put;
            if (have) {
                check = crc32(check, buf, have);
                if (!WriteFile(out, buf, have, &put, NULL) || put != have) { ok = 0; break; }
                total += have;
            }
            if (r == Z_STREAM_END) { left = 0; st.avail_in = 0; break; }
        } while (st.avail_out == 0);
    }
    if (method == 8) inflateEnd(&st);
    CloseHandle(out);
    S.written += total;
    return ok && total == usize && check == crc;
}

static int unzip(void)
{
    Src z;
    unsigned char eocd[22];
    z.f = CreateFileW(S.exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (z.f == INVALID_HANDLE_VALUE) { fail_msg(L"The setup cannot read itself.", NULL); return 0; }
    z.base = S.off;
    z.size = S.size;
    /* the end-of-central-directory record: the zip has no comment, so it is the last 22 bytes */
    if (!src_read(&z, z.size - 22, eocd, 22) || le32(eocd) != 0x06054b50) {
        CloseHandle(z.f);
        fail_msg(L"The setup is damaged. Download it again.", NULL);
        return 0;
    }
    uint32_t count = le16(eocd + 10), cd_size = le32(eocd + 12), cd_off = le32(eocd + 16);
    unsigned char *cd = malloc(cd_size);
    if (!cd || !src_read(&z, cd_off, cd, cd_size)) {
        free(cd);
        CloseHandle(z.f);
        fail_msg(L"The setup is damaged. Download it again.", NULL);
        return 0;
    }
    wchar_t target[PATHN], wname[PATHN];
    char name[PATHN];
    uint32_t at = 0;
    int ok = 1;
    for (uint32_t i = 0; ok && i < count; i++) {
        if (at + 46 > cd_size || le32(cd + at) != 0x02014b50) { ok = 0; break; }
        uint16_t method = le16(cd + at + 10), nlen = le16(cd + at + 28), xlen = le16(cd + at + 30),
                 clen = le16(cd + at + 32);
        uint32_t crc = le32(cd + at + 16), csize = le32(cd + at + 20), usize = le32(cd + at + 24),
                 local = le32(cd + at + 42);
        if (nlen >= PATHN || at + 46 + nlen > cd_size || (method != 0 && method != 8)) { ok = 0; break; }
        memcpy(name, cd + at + 46, nlen);
        name[nlen] = 0;
        at += 46 + nlen + xlen + clen;
        if (!safe_name(name)) { ok = 0; break; }
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, PATHN);
        for (wchar_t *p = wname; *p; p++)
            if (*p == L'/') *p = L'\\';
        swprintf(target, PATHN, L"%ls\\%ls", S.dest, wname);
        size_t tl = wcslen(target);
        if (tl && target[tl - 1] == L'\\') {
            target[tl - 1] = 0;
            ok = make_dirs(wcscat(target, L"\\")) && (is_dir(target) || CreateDirectoryW(target, NULL));
            continue;
        }
        if (!make_dirs(target)) { ok = 0; break; }
        if (!extract_entry(&z, local, csize, usize, method, crc, target)) {
            fail_msg(L"%ls could not be written. Close NX2navi if it is open, then run the setup again.", target);
            free(cd);
            CloseHandle(z.f);
            return 0;
        }
        if (g_wnd && (i & 31) == 0) InvalidateRect(g_wnd, NULL, FALSE);
    }
    free(cd);
    CloseHandle(z.f);
    if (!ok) fail_msg(L"The setup is damaged. Download it again.", NULL);
    return ok;
}

/* ---------------------------------------------------------------- the work */

/* The installed release ver can start: its launcher (launcher/native/nx2navi.c release_ok) and the Python it runs on. */
static int installed_ok(const wchar_t *dest, const wchar_t *ver)
{
    wchar_t p[PATHN];
    swprintf(p, PATHN, L"%ls\\app\\%ls\\launcher\\start.pyw", dest, ver);
    if (!ver[0] || !path_exists(p)) return 0;
    swprintf(p, PATHN, L"%ls\\python\\pythonw.exe", dest);
    return path_exists(p);
}

static DWORD WINAPI work(void *arg)
{
    wchar_t cur[PATHN], have[64];
    (void)arg;
    swprintf(cur, PATHN, L"%ls\\app\\current.txt", S.dest);
    /* A newer launcher (one that updated itself) stays, unless it cannot start: the stub sends the player to this
       setup to repair just that, and the setup's own release then replaces it. */
    int newer_installed = read_line(cur, have, 64) && version_cmp(have, S.version) > 0 && installed_ok(S.dest, have);
    wchar_t probe[PATHN];
    swprintf(probe, PATHN, L"%ls\\x", S.dest);
    if (!make_dirs(probe)) {
        fail_msg(L"The folder %ls cannot be created.", S.dest);
    } else if (!newer_installed) {
        unzip();
    }
    if (!S.result) {
        if (S.shortcuts) {
            set_shortcut(S.dest, 0, 1);
            set_shortcut(S.dest, 1, 1);
        }
        write_uninstall_entry(S.dest, newer_installed ? have : S.version, (DWORD)(S.written / 1024 + 1));
    }
    if (g_wnd) PostMessageW(g_wnd, WM_DONE, 0, 0);
    return 0;
}

/* ---------------------------------------------------------------- the window */

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        HBRUSH bg = CreateSolidBrush(RGB(0x0c, 0x2e, 0x1b)), edge = CreateSolidBrush(RGB(0xa9, 0x87, 0x4f));
        FillRect(dc, &r, edge);
        RECT in = {r.left + 2, r.top + 2, r.right - 2, r.bottom - 2};
        FillRect(dc, &in, bg);
        /* Navi, the launcher's emblem: the artwork's 22 x 21 pixels, 4 x 4 screen pixels each */
        for (int y = 0; y < 21; y++)
            for (int x = 0; x < 22; x++) {
                COLORREF col;
                switch (NAVI[y][x]) {
                case 'W': col = RGB(255, 255, 255); break;
                case 'g': col = RGB(207, 216, 220); break;
                case 'K': col = RGB(0, 0, 0); break;
                case 'B': col = RGB(2, 136, 209); break;
                case 'L': col = RGB(79, 195, 247); break;
                default: continue;
                }
                HBRUSH b = CreateSolidBrush(col);
                RECT px = {16 + 4 * x, 18 + 4 * y, 20 + 4 * x, 22 + 4 * y};
                FillRect(dc, &px, b);
                DeleteObject(b);
            }
        HFONT big = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT small = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0xf3, 0xec, 0xdc));
        HGDIOBJ of = SelectObject(dc, big);
        TextOutW(dc, 120, 30, APP_NAME, (int)wcslen(APP_NAME));
        SelectObject(dc, small);
        SetTextColor(dc, RGB(0xe9, 0xdc, 0xc0));
        wchar_t line[96];
        swprintf(line, 96, L"Installing version %ls\x2026  %llu MB", S.version, (unsigned long long)(S.written >> 20));
        TextOutW(dc, 120, 66, line, (int)wcslen(line));
        SelectObject(dc, of);
        DeleteObject(big);
        DeleteObject(small);
        DeleteObject(bg);
        DeleteObject(edge);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_TIMER:
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_DONE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static void run_with_window(HINSTANCE inst)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_WAIT);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"NX2naviSetup";
    RegisterClassW(&wc);
    int w = 440, h = 120;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 3;
    g_wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"NX2navi Setup", WS_POPUP | WS_VISIBLE, x, y, w, h,
                            NULL, NULL, inst, NULL);
    SetTimer(g_wnd, 1, 200, NULL);
    HANDLE t = CreateThread(NULL, 0, work, NULL, 0, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;
    GetModuleFileNameW(NULL, S.exe, PATHN);
    exe_dir(S.src_dir, PATHN);
    S.shortcuts = S.launch = 1;
    S.dest[0] = 0;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--dest") && i + 1 < argc) {
            GetFullPathNameW(argv[++i], PATHN, S.dest, NULL);
        } else if (!wcscmp(argv[i], L"--no-shortcuts")) S.shortcuts = 0;
        else if (!wcscmp(argv[i], L"--no-launch")) S.launch = 0;
        else if (!wcscmp(argv[i], L"--quiet")) S.quiet = 1;
    }
    if (argv) LocalFree(argv);
    if (!S.dest[0]) {
        wchar_t base[PATHN];
        if (!known_folder(&FOLDERID_UserProgramFiles, base, PATHN)) {
            // 0 when unset; PATHN or more when it does not fit (base is then left unwritten); room for \Programs
            DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", base, PATHN);
            if (!got || got >= PATHN - 16) return 1;
            wcscat(base, L"\\Programs");
        }
        swprintf(S.dest, PATHN, L"%ls\\%ls", base, APP_NAME);
    }
    size_t dl = wcslen(S.dest);
    while (dl > 3 && (S.dest[dl - 1] == L'\\' || S.dest[dl - 1] == L'/')) S.dest[--dl] = 0;
    if (!read_trailer()) {
        MessageBoxW(NULL, L"This setup is incomplete. Download NX2navi-Setup.exe again.", APP_NAME, MB_ICONERROR);
        return 1;
    }
    if (S.quiet) work(NULL);
    else run_with_window(inst);
    if (S.result) {
        if (!S.quiet) MessageBoxW(NULL, S.error, APP_NAME, MB_OK | MB_ICONERROR);
        return 2;
    }
    if (S.launch) {
        static wchar_t cmd[PATHN * 3];
        wchar_t exe[PATHN];
        swprintf(exe, PATHN, L"%ls\\NX2navi.exe", S.dest);
        swprintf(cmd, PATHN * 3, L"\"%ls\" --from \"%ls\"", exe, S.src_dir);
        STARTUPINFOW si = {.cb = sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, S.dest, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return 0;
}
