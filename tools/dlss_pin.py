"""NVIDIA's DLSS library (nvngx_dlss.dll) for the releases: tools/make_launcher.py and tools/make_kit.py put it in the
release's bin/ beside AMD's FSR 3.1 library, and tools/make_game.py copies it beside the game, where NGX loads it
(runtime/vk_dlaa.c). Without it DLSS runs only where the NVIDIA driver supplies the model itself.

The file is NVIDIA's own build from the DLSS SDK repository at a pinned tag, unmodified and checked against its size
and SHA-256. NVIDIA's RTX SDKs License lets an application ship it in object form; the notice written beside it names
the version and the licence that covers it.
"""
from pathlib import Path

NAME = 'nvngx_dlss.dll'
NOTICE = 'nvngx_dlss-NOTICE.txt'   # beside the DLL; make_game.py reads its sha256 line
VERSION = '310.9.1'
URL = f'https://raw.githubusercontent.com/NVIDIA/DLSS/v{VERSION}/lib/Windows_x86_64/rel/{NAME}'
SIZE = 58956912
SHA256 = '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'
LICENSE_URL = f'https://github.com/NVIDIA/DLSS/blob/v{VERSION}/LICENSE.txt'


def notice_text():
    return (f'{NAME} is NVIDIA DLSS {VERSION}, unmodified, as published in NVIDIA\'s DLSS SDK\n'
            f'(https://github.com/NVIDIA/DLSS, tag v{VERSION}). Copyright NVIDIA Corporation.\n'
            '\n'
            'It is not part of NX2navi and NX2navi\'s licence does not cover it. It is distributed under the\n'
            f'NVIDIA RTX SDKs License, and its use is subject to that licence: {LICENSE_URL}\n'
            'NVIDIA licenses DLSS only for use on systems with NVIDIA GPUs.\n'
            '\n'
            f'sha256 {SHA256}\n')


def sha256_file(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def resolve(local, cache_dir):
    """The pinned DLL: local when given (it must be the pinned file), else the copy in cache_dir, downloaded when
    missing (launcher/net.py: resumed, checked against the size and SHA-256)."""
    if local:
        local = Path(local)
        if not local.is_file():
            raise SystemExit(f'{local} does not exist')
        if local.stat().st_size != SIZE or sha256_file(local) != SHA256:
            raise SystemExit(f'{local} is not NVIDIA DLSS {VERSION} (size or SHA-256 differ); a newer DLSS needs '
                             'VERSION, SIZE and SHA256 in tools/dlss_pin.py changed first')
        return local
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from launcher import net
    dest = Path(cache_dir) / f'nvngx_dlss-{VERSION}.dll'
    if not (dest.is_file() and sha256_file(dest) == SHA256):
        print(f'download {URL}', flush=True)
    net.download(URL, dest, sha256=SHA256, size=SIZE)
    return dest


def place(src, bin_dir):
    """The DLL and its notice into a release's bin/."""
    import shutil
    bin_dir = Path(bin_dir)
    shutil.copy2(src, bin_dir / NAME)
    (bin_dir / NOTICE).write_text(notice_text(), encoding='utf-8')
