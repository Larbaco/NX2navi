"""The crash report's test for a module another program put into the game (runtime/main.c foreign_hook): the Vulkan
layers, hooks and overlays of recorders, overlays and capture tools are named as theirs; the game's own libraries and
the drivers are not, nor the validation layer the game loads itself (NX2NAVI_VK_VALIDATION). Builds foreign_hook, cut
out of main.c, into a test program. No game files.

Usage: python tests/foreign_hook_check.py [path to clang]"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
source = (ROOT / 'runtime' / 'main.c').read_text(encoding='utf-8')

FOREIGN = ['RTSSHooks64.dll', 'graphics-hook64.dll', 'GameOverlayRenderer64.dll', 'ReShade64.dll',
           'EOSOverlayVkLayer-Win64.dll', 'VkLayer_steam_fossilize64.dll', 'VkLayer_obs64.dll', 'obs-vulkan64.dll',
           'nvspcap64.dll']
OWN = ['VkLayer_khronos_validation.dll', 'amdvlk64.dll', 'nvoglv64.dll', 'nvngx_dlss.dll', 'ucrtbase.dll', 'SDL3.dll',
       'amd_fidelityfx_vk.dll', 'ntdll.dll', 'nx2navi_dlssg.dll', '']


def compiler():
    if len(sys.argv) > 1:
        return sys.argv[1]
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    names = FOREIGN + OWN
    src = ('#include <ctype.h>\n#include <stdio.h>\n#include <string.h>\n' + function('static int foreign_hook(')
           + '\nint main(int argc, char **argv) {\n    for (int i = 1; i < argc; i++) printf("%d\\n", foreign_hook(argv[i]));\n'
           '    return 0;\n}\n')
    work = ROOT / 'build' / 'test' / 'foreign_hook'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'foreign_hook_check.c').write_text(src, encoding='utf-8')
    exe = work / ('foreign_hook_check.exe' if os.name == 'nt' else 'foreign_hook_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', str(work / 'foreign_hook_check.c'), '-o', str(exe)], env=env)
    got = subprocess.run([str(exe), *names], env=env, capture_output=True, text=True, timeout=60).stdout.split()
    bad = 0
    for name, g in zip(names, got):
        want = '1' if name in FOREIGN else '0'
        if g != want:
            bad += 1
        print(f'{name or "(outside every module)"}: {"another program" if g == "1" else "not foreign"}'
              f'{"" if g == want else " FAIL"}')
    if len(got) != len(names):
        bad += 1
    print(f'foreign_hook_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
