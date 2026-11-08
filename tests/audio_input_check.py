"""Build and run the input-audio regression checks without game files or recording hardware.

Windows: python tests/audio_input_check.py --toolchain C:/msys64/ucrt64
Linux: python tests/audio_input_check.py (clang and SDL3 development files via pkg-config)
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--toolchain', type=Path)
a = ap.parse_args()
out = root / 'build' / 'audio-input-check'
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ)
if a.toolchain:
    env['PATH'] = str(a.toolchain / 'bin') + os.pathsep + env.get('PATH', '')
    cc = str(a.toolchain / 'bin' / 'clang.exe')
    flags = ['-L', str(a.toolchain / 'lib'), '-lSDL3']
else:
    cc = shutil.which('clang') or 'cc'
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
exe = out / ('audio-input-check.exe' if os.name == 'nt' else 'audio-input-check')
subprocess.run([cc, '-std=gnu11', '-O2', '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                '-I', str(root / 'runtime'), str(root / 'tests' / 'audio_input_check.c'),
                '-o', str(exe), *flags], env=env, check=True)
subprocess.run([str(exe)], env=env, timeout=15, check=True)
