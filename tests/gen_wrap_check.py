"""Build and run tests/gen_wrap_check.c: runtime/vk_memory.c's page write generations, extracted verbatim from its
"page write generations" banner to its "dirty census" banner, with the lines gen_next logs as the 32-bit generations
near their wrap (one at each of 0xC0000000, 0xD0000000, 0xE0000000, 0xF0000000 and one at the wrap), and the four
comparisons known to answer wrong across the wrap.

No GPU, no game files, any Python.

Usage: gen_wrap_check.py [--cc <clang>]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
BEGIN = '// ---------------------------------------------------------------- page write generations'
END = '// ---------------------------------------------------------------- dirty census'


def compiler(cc):
    if cc:
        return cc
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', help='the C compiler (default: $CC, MSYS2 UCRT64 clang, clang on PATH)')
    args = ap.parse_args()
    src = (ROOT / 'runtime' / 'vk_memory.c').read_text(encoding='utf-8-sig')
    a, b = src.find(BEGIN), src.find(END)
    if a < 0 or b < a:
        print('gen_wrap_check: FAIL: the section banners are not in runtime/vk_memory.c')
        return 1
    work = ROOT / 'build' / 'test' / 'gen_wrap'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'gen_section.inc').write_text('// Extracted from runtime/vk_memory.c by tests/gen_wrap_check.py.\n' +
                                          src[a:b], encoding='utf-8')
    exe = work / ('gen_wrap_check.exe' if os.name == 'nt' else 'gen_wrap_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(args.cc), '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function',
                           '-Wno-unused-variable', '-I', str(work), str(ROOT / 'tests' / 'gen_wrap_check.c'), '-o',
                           str(exe)], env=env)
    return subprocess.run([str(exe)], env=env, timeout=120).returncode


if __name__ == '__main__':
    sys.exit(main())
