// runtime/dispatch.c's end of a fatal error (fatal_end), with the platform layer linked and the rest of the runtime as
// stand-ins that write straight to stdout. Built and run by tests/fatal_exit_check.py, which reads what each mode
// printed and its exit code. The exe has a TLS callback that prints a line when the process's libraries get their
// detach call, which ExitProcess (and _Exit, which is ExitProcess in the C runtime) makes and TerminateProcess does not.
//
//   fatal_exit_check fatal   rt_fatal: exit code 70, the FATAL line, no detach line
#include "dispatch.c"

#include <stdio.h>
#include <windows.h>

static void say(const char *s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &w, NULL);
}

static void NTAPI tls_cb(PVOID h, DWORD reason, PVOID reserved) {
    (void)h, (void)reserved;
    if (reason == DLL_PROCESS_DETACH) say("detach: the libraries' detach calls ran\n");
}
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK p_tls_cb = tls_cb;
__thread int tls_var;   // with _tls_used below: the image gets its TLS directory, so the callback is called
extern const IMAGE_TLS_DIRECTORY _tls_used;
const void *volatile keep_tls = &_tls_used;

// ---- the runtime fatal_end reaches
void rt_log_sync(void) {}
void rt_log_raw(const char *p, size_t n) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), p, (DWORD)n, &w, NULL);
}
void rt_vlogf(const char *fmt, va_list ap) {
    char b[1024];
    int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n > 0) rt_log_raw(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}
void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
    rt_log_raw("\n", 1);
}
void kern_free_cores_note(void) {}
void prof_sampler_write(void) {}
void rtss_limit_restore(void) {}
void kern_checkpoint(void) {}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "";
    tls_var = 1;
    if (!strcmp(mode, "fatal")) rt_fatal("the test's fatal error %d", 1);
    printf("usage: fatal_exit_check fatal\n");
    return 2;
}
