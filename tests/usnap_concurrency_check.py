"""Build the real vk_usnap.c concurrency probe on Linux, with optional ThreadSanitizer.

No game files or GPU are needed. The platform lock and replay stream position are test stand-ins, so this checks the actual
snapshot data structure's concurrent drop, find and why operations, not GPU synchronization or chunk stream order.
The Vulkan and VMA headers are needed to compile the runtime's types. Phase barriers require both test threads to
advance, but do not force a particular interleaving. The bench uses Clang 20; pass its path with --cc if needed.

    python tests/usnap_concurrency_check.py --vma-include <vma-dir> --work <scratch-dir>
    python tests/usnap_concurrency_check.py --cc /path/to/clang-20 --vma-include <vma-dir> --work <scratch-dir> --tsan
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', default=shutil.which('clang') or 'clang')
    ap.add_argument('--vma-include', type=Path, default=Path(os.environ.get('VMA_INCLUDE', ROOT / 'toolchain' / 'vma')))
    ap.add_argument('--vulkan-include', type=Path, help='Vulkan headers, when not provided by the system')
    ap.add_argument('--work', type=Path, default=ROOT / 'build' / 'test' / 'usnap-concurrency')
    ap.add_argument('--tsan', action='store_true', help='compile and run with Clang ThreadSanitizer')
    a = ap.parse_args()
    if not sys.platform.startswith('linux'):
        ap.error('this pthread and linker-section harness runs on Linux')
    if not (a.vma_include / 'vk_mem_alloc.h').is_file():
        ap.error('provide --vma-include for the directory holding vk_mem_alloc.h')
    a.work.mkdir(parents=True, exist_ok=True)
    exe = a.work / ('usnap-concurrency-tsan' if a.tsan else 'usnap-concurrency')
    flags = ['-fsanitize=thread', '-fno-omit-frame-pointer'] if a.tsan else []
    includes = ['-I', str(ROOT / 'runtime'), '-I', str(a.vma_include)]
    if a.vulkan_include:
        includes += ['-I', str(a.vulkan_include)]
    subprocess.run([a.cc, '-std=gnu11', '-O1', '-g', '-pthread', '-ffunction-sections', '-fdata-sections', *flags,
                    *includes, str(ROOT / 'tests' / 'usnap_concurrency_check.c'), '-Wl,--gc-sections', '-o', str(exe)],
                   check=True)
    env = dict(os.environ)
    if a.tsan:
        env['TSAN_OPTIONS'] = env.get('TSAN_OPTIONS', '') + ':halt_on_error=1:exitcode=66'
    return subprocess.run([str(exe)], env=env, timeout=180).returncode


if __name__ == '__main__':
    sys.exit(main())
