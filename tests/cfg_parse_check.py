"""Build and run tests/cfg_parse_check.c: runtime/cfg.c's reading of nx2navi.ini and the environment, against stand-ins
for what it links with. The values stay as written; what is checked is what the log says about them:

- values outside their rows' sets (cfg.c check_value): a 0|1 switch with another value, a number outside its range, a
  word that is none of the menu's choices, and a ';' or '#' comment kept in a value, each named with its line; none
  for a file whose values fit, nor for a list with numbers in it (the menu's presets); at most 16 lines, then a count.

No game files.

Usage: cfg_parse_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


CLEAN = '''[frame rate]
pace_smooth = 1
fps_limit = 144
; a comment line
# another
[display]
sharpness = 80
color_gamma = 1.1
vk_present = FIFO
render_scale = 300
dlss_preset = k
pad_layout = xbox_prompts
monitor = 2
'''

BAD = '''[frame rate]
pace_smooth = 0 ; off
fullscreen = false
sharpness = 250
color_gamma = 2
vk_present = vsync
fps_limit = 144 # my monitor
sharpness_typo = 3
'''


def run(exe, env, case, text, work, extra_env=None):
    ini = work / f'{case}_{len(text)}.ini'
    ini.write_text(text, encoding='utf-8')
    e = dict(env)
    for k in list(e):
        if k.upper().startswith('NX2NAVI_'):
            del e[k]
    e.update(extra_env or {})
    p = subprocess.run([str(exe), case, str(ini)], env=e, capture_output=True, text=True, errors='replace', timeout=60)
    if p.returncode:
        print(p.stdout, p.stderr)
    return p.returncode, p.stdout


def main():
    work = ROOT / 'build' / 'test' / 'cfg_parse'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('cfg_parse_check.exe' if os.name == 'nt' else 'cfg_parse_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', '-I', str(rt),
                           str(ROOT / 'tests' / 'cfg_parse_check.c'), str(rt / 'cfg.c'), '-o', str(exe)], env=env)
    bad = 0

    def check(ok, what, out=''):
        nonlocal bad
        print(f'{what}: {"ok" if ok else "FAIL"}', flush=True)
        if not ok:
            bad += 1
            print(out)

    # values that fit: no line about them
    rc, out = run(exe, env, 'load', CLEAN, work)
    lines = [l for l in out.splitlines() if l.startswith('log: cfg:') and '=' in l and 'line' in l]
    check(rc == 0 and not lines, 'a file whose values fit logs nothing about them', out)

    # values that do not: one line each, with the line number
    rc, out = run(exe, env, 'load', BAD, work, {'NX2NAVI_SHARPNESS': '500', 'NX2NAVI_VK_PRESENT': 'fifo'})
    want = [
        "line 2: pace_smooth = '0 ; off' is not 0 or 1; a ';' or '#' after the value is part of it",
        "line 3: fullscreen = 'false' is not 0 or 1",
        "line 4: sharpness = '250' is not a number in 0..100",
        "line 5: color_gamma = '2' is not a number in 0.8..1.2",
        "line 6: vk_present = 'vsync' is none of the menu's choices mailbox|fifo|immediate",
        "line 7: fps_limit = '144 # my monitor' a ';' or '#' after the value is part of it",
        "the environment's NX2NAVI_SHARPNESS = '500' is not a number in 0..100",
    ]
    for w in want:
        check(rc == 0 and w in out, f'logged: {w}', out if w not in out else '')
    check('NX2NAVI_VK_PRESENT' not in out, 'an environment value that fits is not logged', out)
    check("pace_smooth '0 ; off'" in out, 'the value itself is read as written', out)

    # at most 16 lines, then a count
    many = '\n'.join(f'sharpness = {200 + i}' if i % 2 else f'fullscreen = no{i}' for i in range(2)) + '\n'
    many = ''.join(f'[s{i}]\nsharpness = {200 + i}\n' for i in range(1)) + ''.join(
        f'{k} = 7\n' for k in ('fullscreen', 'pace_smooth', 'vk_present', 'sharpness', 'color_gamma', 'perf_overlay',
                               'upscaler', 'standin_filter', 'dlss', 'dlss_preset', 'framegen', 'aa', 'aspect',
                               'texture_detail', 'color_preset', 'device_profile', 'mods_rebuild', 'operation_mode',
                               'text_speed', 'sprint_mode'))
    rc, out = run(exe, env, 'load', many, work)
    listed = sum(1 for l in out.splitlines() if l.startswith('log: cfg: nx2navi.ini line') and "= '7'" in l)
    check(rc == 0 and listed == 16 and 'more values outside their settings' in out,
          f'20 values outside their sets: {listed} lines and a count', out)
    print(f'cfg_parse_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
