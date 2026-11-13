"""Build and run runtime/perf.c's rt_knob (cut out of the file with its warning table) with stand-ins for the settings and
the log: 0, 1 and ab read as before and log nothing; a value such as "off", "false" or "0 ; a note" still reads as 1
and is logged once per switch, however often the switch is read.

No GPU, no game files, any Python.

Usage: rt_knob_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

HARNESS = r'''
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *vals[][2] = {{"NX2NAVI_A", "0"}, {"NX2NAVI_B", "1"}, {"NX2NAVI_C", "ab"}, {"NX2NAVI_D", "off"},
                                {"NX2NAVI_E", "false"}, {"NX2NAVI_F", "0 ; a note"}, {"NX2NAVI_G", ""}};
static const char *cfg_get(const char *key) {
    for (size_t i = 0; i < sizeof vals / sizeof *vals; i++)
        if (!strcmp(vals[i][0], key)) return vals[i][1];
    return NULL;
}
static int logs;
static char last[256];
static void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last, sizeof last, fmt, ap);
    va_end(ap);
    logs++;
}

%s

static int fails;
static void expect(const char *what, int got, int want) {
    if (got != want) printf("FAIL %%s: %%d, expected %%d\n", what, got, want), fails++;
}

int main(void) {
    for (int round = 0; round < 3; round++) {
        expect("0", rt_knob("A", "1"), 0);
        expect("1", rt_knob("B", "0"), 1);
        expect("ab", rt_knob("C", "0"), 2);
        expect("unset, default 1", rt_knob("H", "1"), 1);
        expect("unset, default 0", rt_knob("H2", "0"), 0);
        expect("empty, default 0", rt_knob("G", "0"), 0);
    }
    expect("lines for 0, 1, ab and defaults", logs, 0);
    expect("off", rt_knob("D", "0"), 1);
    expect("a line for off", logs, 1);
    if (strcmp(last, "cfg: NX2NAVI_D = 'off' is not 0, 1 or ab: read as 1"))
        printf("FAIL the line: %%s\n", last), fails++;
    for (int i = 0; i < 5; i++) rt_knob("D", "0");
    expect("off read again", logs, 1);
    expect("false", rt_knob("E", "1"), 1);
    expect("0 with a note", rt_knob("F", "0"), 1);
    expect("a line each", logs, 3);
    if (!strstr(last, "'0 ; a note'")) printf("FAIL the line: %%s\n", last), fails++;
    for (int i = 0; i < 3; i++) rt_knob("E", "1"), rt_knob("F", "0");
    expect("each once", logs, 3);
    if (fails) return 1;
    printf("rt_knob_check: all checks passed\n");
    return 0;
}
'''


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def cut():
    """perf.c from the warning table's #define to the end of rt_knob."""
    src = (ROOT / 'runtime' / 'perf.c').read_text(encoding='utf-8')
    m = re.search(r'#define KNOB_WARNED .*?\nint rt_knob\(const char \*name, const char \*def\) \{\n.*?\n\}\n', src, re.S)
    assert m, 'runtime/perf.c: the rt_knob block (KNOB_WARNED ... rt_knob) was not found'
    return m.group(0)


def main():
    work = ROOT / 'build' / 'test' / 'rt_knob'
    work.mkdir(parents=True, exist_ok=True)
    c = work / 'rt_knob_check.c'
    c.write_text('// Made by tests/rt_knob_check.py from runtime/perf.c.\n' + HARNESS % cut(), encoding='utf-8')
    exe = work / ('rt_knob_check.exe' if os.name == 'nt' else 'rt_knob_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', str(c), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
