"""Build and run tests/ds3_check.c: runtime/input_ds3.c's input report decoding and output report layout.

Usage: ds3_check.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import MSYS_BIN, compiler  # noqa: E402


def main():
    work = ROOT / 'build' / 'test' / 'ds3'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('ds3_check.exe' if os.name == 'nt' else 'ds3_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'ds3_check.c'), str(ROOT / 'runtime' / 'input_ds3.c'), '-o', str(exe)],
                          env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
