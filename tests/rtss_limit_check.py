"""Build and run the checks of runtime/rtss_limit.c, RivaTuner Statistics Server as the frame limiter.

tests/rtss_limit_check.c builds runtime/rtss_decide.h alone, the parts that make no system call. tests/rtss_worker_check.c
(Windows only, skipped elsewhere) compiles the real rtss_limit.c against a fake RTSS and runs each of its scenarios in a
process of its own.

No GPU, no game files, no RTSS, any Python.

Usage: rtss_limit_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def build(src, exe, env):
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / src), '-o', str(exe)], env=env)


def main():
    work = ROOT / 'build' / 'test' / 'rtss_limit'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    failed = []
    pure = work / ('rtss_limit_check.exe' if os.name == 'nt' else 'rtss_limit_check')
    build('rtss_limit_check.c', pure, env)
    if subprocess.run([str(pure)], env=env).returncode != 0:
        failed.append('rtss_limit_check')
    if os.name != 'nt':
        print('rtss_worker_check: skipped (rtss_limit.c limits only on Windows)')
        return 1 if failed else 0
    exe = work / 'rtss_worker_check.exe'
    build('rtss_worker_check.c', exe, env)
    names = subprocess.run([str(exe), '--list'], env=env, capture_output=True, text=True, check=True).stdout.split()
    for name in names:
        try:
            p = subprocess.run([str(exe), name], env=env, capture_output=True, text=True, timeout=120)
            ok, out = p.returncode == 0, p.stdout + p.stderr
        except subprocess.TimeoutExpired as e:
            ok, out = False, (e.stdout or b'').decode(errors='replace') + '\nTIMEOUT'
        print(f'rtss_worker_check {name}: {"ok" if ok else "FAIL"}')
        if not ok:
            print(out)
            failed.append(name)
    print(f'{len(names) + 1 - len(failed)} of {len(names) + 1} checks ok' + (f'; failed: {", ".join(failed)}' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
