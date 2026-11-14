"""Check tools/build_dlssg.py's export check: it reads the exports runtime/dlssg/nx2_sl.h declares and the ones a DLL
has, and names those the DLL lacks. A DLL built from older sources would otherwise ship: the runtime takes a missing
optional export (nx2sl_reflex2, nx2sl_frames_max) for an older DLL and leaves its feature out without an error.

Builds a small DLL with two of three declared exports with MSYS2 UCRT64's clang (no MSVC, no Streamline SDK, no game
files) and checks that the third is the one reported; then that nx2_sl.h's own declarations are read in full.

Usage: dlssg_exports_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
sys.path.insert(0, str(ROOT / 'tools'))
import build_dlssg  # noqa: E402

HEADER = '''typedef int (*PFN_nx2sl_version)(void);
typedef void (*PFN_nx2sl_reflex)(int mode);
typedef int ( * PFN_nx2sl_reflex2 )(int mode, unsigned frame_limit_us);
'''
SOURCE = '''__declspec(dllexport) int nx2sl_version(void) { return 3; }
__declspec(dllexport) void nx2sl_reflex(int mode) { (void)mode; }
int nx2sl_not_exported(void) { return 0; }
'''


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/wb_lazy_check.py picks it)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    if os.name != 'nt':
        print('dlssg_exports_check: Windows only (it builds a DLL)')
        return 0
    work = ROOT / 'build' / 'test' / 'dlssg_exports'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'two.h').write_text(HEADER, encoding='utf-8')
    (work / 'two.c').write_text(SOURCE, encoding='utf-8')
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    dll = work / 'two.dll'
    subprocess.check_call([compiler(), '-shared', '-O1', str(work / 'two.c'), '-o', str(dll)], env=env)
    fails = 0

    def expect(what, got, want):
        nonlocal fails
        if got != want:
            print(f'FAIL {what}: {got!r}, not {want!r}')
            fails += 1

    expect('declared in the test header', build_dlssg.declared_exports(work / 'two.h'),
           {'nx2sl_version', 'nx2sl_reflex', 'nx2sl_reflex2'})
    expect('exported by the test DLL', build_dlssg.pe_exports(dll), {'nx2sl_version', 'nx2sl_reflex'})
    expect('missing from the test DLL', build_dlssg.missing_exports(dll, work / 'two.h'), ['nx2sl_reflex2'])
    declared = build_dlssg.declared_exports()
    for name in ('nx2sl_version', 'nx2sl_init', 'nx2sl_reflex', 'nx2sl_reflex2', 'nx2sl_frames_max', 'nx2sl_shutdown'):
        expect(f'{name} declared in nx2_sl.h', name in declared, True)
    # the header names its version and log types too, which are no exports of their own
    expect('types that are no functions', {'nx2sl_' + t for t in ('Image', 'Camera', 'Log')} & declared, set())
    print(f'dlssg_exports_check: {"ok" if not fails else f"{fails} failed"} ({len(declared)} exports in nx2_sl.h)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
