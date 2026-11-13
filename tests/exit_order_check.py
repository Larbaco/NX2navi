"""The orderly end of the process (runtime/gfx_present.c rt_exit_begin and rt_exit_steps): the steps run in one order,
with what a kill of the process must not lose (a save Commit in flight, RivaTuner Statistics Server's profile, the
controller's rumble, the settings, the frame-time summary) before the slow pipeline save and the final save flush
last; it runs once, and a second caller on another thread stays where it is. The frame-time summary of the exit
(runtime/perf.c ft_stop) waits for a present thread's update under way and keeps later ones out. Builds those
functions into a test program against stand-ins, and checks statically that every orderly end goes through them.
No game files.

Usage: python tests/exit_order_check.py [path to clang]"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if len(sys.argv) > 1:
        return sys.argv[1]
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def calls_in_order(body, names):
    """The position of each name's first call in body (comments left out), or None for a name that is missing."""
    code = strip_comments(body)
    return [m.start() if (m := re.search(r'\b' + re.escape(n) + r'\s*\(', code)) else None for n in names]


STEPS = ['fs_save_wait', 'rtss_limit_restore', 'input_rumble_hd_stop', 'cfg_save', 'vk_pipeline_prewarm_marker_clear',
         'prof_write', 'vk_mem_exit_log', 'logq_flush', 'vk_pipeline_save_exit', 'fs_save_flush']

HARNESS = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }
static void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
enum { RT_EXIT_SETTINGS = 1 };
static char seen[1024];
#define STEP(name) static void name(void) { strcat(seen, #name " "); }
STEP(fs_save_wait) STEP(rtss_limit_restore) STEP(input_rumble_hd_stop) STEP(vk_pipeline_prewarm_marker_clear)
STEP(prof_write) STEP(vk_mem_exit_log) STEP(logq_flush) STEP(vk_pipeline_save_exit) STEP(fs_save_flush)
static int cfg_save(void) { strcat(seen, "cfg_save "); return 1; }
static int ending;
static void rt_log_ending(void) { ending++; }
'''

MAIN = r'''
static volatile LONG second_back = -1;
static DWORD WINAPI second(void *arg) { (void)arg; second_back = rt_exit_begin(); return 0; }

static volatile LONG updating;
static DWORD WINAPI present(void *arg) {   // one present thread update that takes ms milliseconds
    int ms = (int)(intptr_t)arg;
    if (!ft_enter()) { ft_leave(); return 0; }
    updating = 1;
    Sleep(ms);
    updating = 0;
    ft_leave();
    return 0;
}

int main(int argc, char **argv) {
    int fails = 0;
    rt_exit_steps(RT_EXIT_SETTINGS);
    printf("window close, guest ExitProcess: %s\n", seen);
    if (strcmp(seen, argv[1])) printf("FAIL: want %s\n", argv[1]), fails++;
    seen[0] = 0;
    rt_exit_steps(0);
    printf("--seconds: %s\n", seen);
    if (strcmp(seen, argv[2])) printf("FAIL: want %s\n", argv[2]), fails++;

    int first = rt_exit_begin(), again = rt_exit_begin();
    HANDLE t = CreateThread(NULL, 0, second, NULL, 0, NULL);
    int parked = WaitForSingleObject(t, 300) == WAIT_TIMEOUT && second_back == -1;
    printf("once: first caller %d, its own thread again %d, another thread %s; the log told of the end %d time(s)\n",
           first, again, parked ? "stays in rt_exit_begin" : "came back", ending);
    if (first != 1 || again != 0 || !parked || ending != 1) printf("FAIL: the end must run once\n"), fails++;

    // an update under way: ft_stop waits for it; after ft_stop no update starts
    HANDLE p = CreateThread(NULL, 0, present, (void *)(intptr_t)20, 0, NULL);
    while (!updating) Sleep(1);
    ULONGLONG t0 = GetTickCount64();
    ft_stop();
    ULONGLONG waited = GetTickCount64() - t0;
    int busy_after = updating;
    WaitForSingleObject(p, INFINITE);
    int later = ft_enter();
    ft_leave();
    printf("frame-time summary: ft_stop waited %llu ms for the update under way (%s), a later update %s\n",
           (unsigned long long)waited, busy_after ? "still running: FAIL" : "done",
           later ? "still added: FAIL" : "kept out");
    if (busy_after || later) fails++;
    // an update that does not end: ft_stop gives up after about 50 ms
    __atomic_store_n(&ft_ending, 0, __ATOMIC_SEQ_CST);
    p = CreateThread(NULL, 0, present, (void *)(intptr_t)2000, 0, NULL);
    while (!updating) Sleep(1);
    t0 = GetTickCount64();
    ft_stop();
    waited = GetTickCount64() - t0;
    printf("frame-time summary: ft_stop with an update that hangs came back after %llu ms\n", (unsigned long long)waited);
    if (waited > 1000) printf("FAIL: ft_stop must be bounded\n"), fails++;
    printf("exit_order_check: %s\n", fails ? "FAILED" : "ok");
    fflush(stdout);
    ExitProcess(fails ? 1 : 0);   // the parked thread never returns
}
'''


def static_checks():
    bad = 0
    gp = (RT / 'gfx_present.c').read_text(encoding='utf-8')
    svc = (RT / 'svc.c').read_text(encoding='utf-8')
    boot = (RT / 'boot.c').read_text(encoding='utf-8')
    perf = (RT / 'perf.c').read_text(encoding='utf-8')
    exit07 = svc[svc.index('case 0x07:'):]
    exit07 = exit07[:exit07.index('svc_fatal(')]
    paths = [('gfx_present.c gfx_window_closed', function(gp, 'void gfx_window_closed(void)'), 'rt_exit_steps(RT_EXIT_SETTINGS)'),
             ('svc.c ExitProcess (0x07)', exit07, 'rt_exit_steps(RT_EXIT_SETTINGS)'),
             ('boot.c rt_frame_presented (--seconds)', function(boot, 'void rt_frame_presented('), 'rt_exit_steps(0)')]
    for name, body, steps in paths:
        code = strip_comments(body)
        a, b, c = code.find('rt_exit_begin()'), code.find(steps), code.rfind('plat_exit(')
        own = [s for s in STEPS if re.search(r'\b' + s + r'\s*\(', code)]
        ok = 0 <= a < b < c and not own
        print(f'{name}: {"rt_exit_begin, then " + steps + ", then plat_exit" if ok else "FAIL"}'
              + (f'; calls exit steps of its own: {", ".join(own)}' if own else ''), flush=True)
        bad += not ok
    steps = function(gp, 'void rt_exit_steps(unsigned what)')
    pos = calls_in_order(steps, ['rtss_limit_restore', 'vk_pipeline_save_exit'])
    ok = None not in pos and pos[0] < pos[1]
    print(f'rt_exit_steps: RivaTuner\'s profile put back before the pipeline save: {"ok" if ok else "FAIL"}')
    bad += not ok
    flush = strip_comments(function(perf, 'void perf_summary_flush(void)'))
    present = strip_comments(function(perf, 'void perf_present(uint64_t frame)'))
    e, f, s, l = (present.find(x) for x in ('ft_enter()', 'ft_frame(', 'session_report(', 'ft_leave()'))
    stop, report = flush.find('ft_stop();'), flush.find('ft_report(')
    ok = 0 <= stop < report and 0 <= e < f < s < l
    print(f'perf.c: perf_summary_flush calls ft_stop before ft_report, perf_present updates inside ft_enter/ft_leave: '
          f'{"ok" if ok else "FAIL"}')
    bad += not ok
    return bad


def main():
    sys.stdout.reconfigure(line_buffering=True)
    bad = static_checks()
    if os.name != 'nt':
        print('exit_order_check: the program part runs on Windows only')
        return 1 if bad else 0
    gp = (RT / 'gfx_present.c').read_text(encoding='utf-8')
    perf = (RT / 'perf.c').read_text(encoding='utf-8')
    tid = re.search(r'static volatile uint32_t exit_tid;[^\n]*', gp).group(0)
    ftvars = re.search(r'static volatile int32_t ft_ending, ft_busy;', perf).group(0)
    src = '\n'.join([HARNESS, tid, function(gp, 'int rt_exit_begin(void)'), function(gp, 'void rt_exit_steps(unsigned what)'),
                     ftvars, function(perf, 'static int ft_enter(void)'), function(perf, 'static void ft_leave(void)'),
                     function(perf, 'static void ft_stop(void)'), MAIN])
    work = ROOT / 'build' / 'test' / 'exit_order'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'exit_order_check.c').write_text(src, encoding='utf-8')
    exe = work / 'exit_order_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', str(work / 'exit_order_check.c'),
                           '-o', str(exe)], env=env)
    want_all = ' '.join(STEPS) + ' '
    want_seconds = ' '.join(s for s in STEPS if s != 'cfg_save') + ' '
    rc = subprocess.run([str(exe), want_all, want_seconds], env=env, timeout=60).returncode
    print(f'exit_order_check, the static checks and the program: {"FAILED" if bad or rc else "ok"}')
    return 1 if bad or rc else 0


if __name__ == '__main__':
    sys.exit(main())
