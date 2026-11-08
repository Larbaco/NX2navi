"""Where the launcher keeps everything.

An installed launcher (NX2navi-Setup.exe puts it in %LOCALAPPDATA%\\Programs\\NX2navi) is one folder:

    NX2navi.exe              launcher/native/nx2navi.c: starts python\\pythonw.exe on app\\<current>\\launcher\\start.pyw
    python/                  the embeddable Python with tkinter, numpy, lz4 and zstandard (tools/make_launcher.py)
    app/current.txt          the version the stub starts; app/previous.txt the one before it, kept for a rollback
    app/<version>/           one release: launcher/, runtime/, recomp/, tools/, bin/, VERSION, toolchain.lock.json
    pydeps/                  zbic, downloaded from PyPI on this PC (launcher/toolchain.py ensure_zbic)
    toolchain/msys64/ucrt64  MSYS2's clang and libraries, downloaded from the MSYS2 repository on this PC
    toolchain/pkgs/          the downloaded package files, kept while the lock names them
    data/launcher.json       settings; data/logs/ logs; data/cache/ the fonts read from the player's game
    game/                    the built game, unless the player picks another folder

Run from a checkout of the repository (python -m launcher), the app folder is the repository and everything else goes
to build/launcher-dev/, which git ignores.
"""
import os
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]


def _is_release_app(app):
    return app.parent.name == 'app' and (app.parent / 'current.txt').is_file()


def ansi_safe(path):
    """True when the path survives the Windows ANSI code page: the MSYS2 tools and the game's own build read their
    arguments through it, so a folder name outside it breaks the build. Always True elsewhere."""
    if os.name != 'nt':
        return True
    try:
        str(path).encode('mbcs', errors='strict')
        return True
    except (UnicodeEncodeError, LookupError):
        return False


def fixed_drive(drive):
    """True when drive (X:\\) is a fixed disk (GetDriveTypeW DRIVE_FIXED), not a network, removable or optical drive.
    True where that cannot be asked."""
    if os.name != 'nt':
        return True
    try:
        import ctypes
        return ctypes.windll.kernel32.GetDriveTypeW(str(drive)) == 3
    except (AttributeError, OSError):
        return True


class Layout:
    def __init__(self, root=None, app=None):
        self.app = Path(app or APP)
        self.dev = not _is_release_app(self.app)
        env = os.environ.get('NX2NAVI_LAUNCHER_ROOT')
        if root:
            self.root = Path(root)
        elif env:
            self.root = Path(env)
        elif not self.dev:
            self.root = self.app.parents[1]
        else:
            self.root = self.app / 'build' / 'launcher-dev'
        self.apps = self.root / 'app'
        self.python = self.root / 'python'
        self.stub = self.root / 'NX2navi.exe'
        self.pydeps = self.root / 'pydeps'
        self.data = self.root / 'data'
        self.logs = self.data / 'logs'
        self.cache = self.data / 'cache'
        self.downloads = self.data / 'downloads'
        self.settings = self.data / 'launcher.json'
        tool_root = self.root
        if not ansi_safe(tool_root):
            tool_root = Path(os.environ.get('SystemDrive', 'C:') + '\\') / 'NX2navi'
        self.toolchain = tool_root / 'toolchain'
        self.msys_root = self.toolchain / 'msys64'
        self.msys = self.msys_root / 'ucrt64'
        self.pkgs = self.toolchain / 'pkgs'
        self.tools = self.app / 'tools'
        self.make_game = self.tools / 'make_game.py'
        self.lock_file = self.app / 'toolchain.lock.json'
        self.feed_file = self.app / 'feed.json'

    @property
    def version(self):
        v = self.app / 'VERSION'
        return v.read_text(encoding='utf-8').strip() if v.is_file() else '0.0.0'

    def default_game_dir(self):
        """<root>/game, or a Games folder on the fixed drive with the most free space when the root's drive has
        less than 40 GB free or the root's path would break the build."""
        here = self.root / 'game'
        try:
            import shutil
            if shutil.disk_usage(self.root.anchor or self.root).free >= 40e9 and ansi_safe(here):
                return here
        except OSError:
            return here
        best = None
        if os.name == 'nt':
            import shutil
            import string
            for letter in string.ascii_uppercase[2:]:
                drive = Path(f'{letter}:\\')
                if not fixed_drive(drive):   # a mapped network share or a USB stick is no place for 30 GB of build
                    continue
                try:
                    free = shutil.disk_usage(drive).free
                except OSError:
                    continue
                if best is None or free > best[0]:
                    best = (free, drive / 'Games' / 'NX2navi')
        return best[1] if best else here

    def python_for_builds(self):
        """The console python.exe beside the launcher's pythonw.exe (no window: the build starts it with
        CREATE_NO_WINDOW), or this interpreter in a checkout."""
        exe = Path(sys.executable)
        if exe.name.lower() == 'pythonw.exe' and (exe.parent / 'python.exe').is_file():
            return str(exe.parent / 'python.exe')
        return str(exe)

    def ensure(self):
        for d in (self.data, self.logs, self.cache, self.downloads):
            d.mkdir(parents=True, exist_ok=True)
        return self
