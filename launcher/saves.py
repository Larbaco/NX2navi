"""Importing a save into the built game, and the saves folder of the game folder.

The game keeps its save in <game>/profile/save/<account>/ (runtime/fs_host.c storage_init). The folder is named after
the local account in <game>/profile/account.uid: 16 bytes, two little-endian 64-bit halves, the low one first; the
name is the high half then the low half, 16 hex digits each (runtime/ov_system.c load_uid). A game that never ran has
no account yet; the import makes one as the game would. Saves are committed through <game>/profile/save-tx/, where
a finished commit (ready-<n>) is newer than save/<account>: an import clears that folder, so the game loads the
imported save.

<game>/saves is the player's side. Import Saves takes a save put there first (its files, or the folder that holds
them); otherwise the player picks a folder. Before the save is replaced it is copied to saves/backup <time>, and a
save taken from saves/ moves to saves/imported <time>, so nothing is lost and nothing is imported twice.

A save folder can come from a Switch save backup (the save's files), an emulator (nested folders named by user and
title ids, gone down while each holds only one folder), another NX2navi game folder or its profile folder (the
account folder inside).
"""
import os
import re
import shutil
import stat
import time
from pathlib import Path


def _rmtree(path):
    """Delete a folder, read-only files in it too (shutil.rmtree stops at them on Windows); what still fails stays."""
    def writable(func, p, _exc):
        try:
            os.chmod(p, stat.S_IREAD | stat.S_IWRITE)
            func(p)
        except OSError:
            pass
    if Path(path).exists():
        shutil.rmtree(path, onexc=writable)

README = '''NX2navi saves folder

To import a save, put it here and select Import Saves in the NX2navi launcher (next to Game Folder):
- the files of a Switch save backup, or the folder that holds them
- an emulator's save folder for the game
- the profile folder (or the whole game folder) of another NX2navi install

You can also select Import Saves and choose the folder instead.

Before a save is imported, the game's current save is copied here into a folder named "backup" and the date. An
imported save moves into a folder named "imported" and the date. The game itself keeps its save in profile\\save.
'''
HEX32 = re.compile(r'^[0-9a-fA-F]{32}$')
MAX_FILES, MAX_BYTES = 4000, 1 << 30   # a save is a few files of a few MB; more is a folder picked by mistake
OURS = re.compile(r'^(backup|imported) \d{4}-\d\d-\d\d')   # what this module writes into saves/


class SaveError(Exception):
    pass


def saves_dir(game):
    return Path(game) / 'saves'


def ensure_saves_dir(game):
    """<game>/saves with its README.txt."""
    d = saves_dir(game)
    d.mkdir(parents=True, exist_ok=True)
    readme = d / 'README.txt'
    if not readme.is_file():
        readme.write_text(README, encoding='utf-8', newline='\r\n')
    return d


def account_dir(game):
    """The game's save folder for its local account, making the account as the game does when it has none."""
    prof = Path(game) / 'profile'
    f = prof / 'account.uid'
    data = f.read_bytes() if f.is_file() else b''
    if not data:
        data = bytes(16)
        while data == bytes(16):
            data = os.urandom(16)
        prof.mkdir(parents=True, exist_ok=True)
        tmp = f.with_name('account.uid.tmp')
        tmp.write_bytes(data)
        os.replace(tmp, f)
    if len(data) != 16 or data == bytes(16):
        raise SaveError(f'{f} is not an account file of the game')
    lo, hi = int.from_bytes(data[:8], 'little'), int.from_bytes(data[8:], 'little')
    return prof / 'save' / f'{hi:016x}{lo:016x}'


def _fresh(folder, name):
    """folder/name, or folder/"name (2)" and so on when that is taken (two imports in one second)."""
    p, n = folder / name, 2
    while p.exists():
        p, n = folder / f'{name} ({n})', n + 1
    return p


def _entries(folder, skip=()):
    return [e for e in Path(folder).iterdir() if e.name not in skip and not e.name.startswith('.')]


def find_save(folder):
    """(source folder, top-level names to leave out) of the save in folder, or (None, ()) when it holds none."""
    p = Path(folder)
    if not p.is_dir():
        return None, ()
    for base in (p / 'profile' / 'save', p / 'save'):   # another game folder, or its profile folder
        if base.is_dir():
            ready = _ready(base.parent / 'save-tx')   # a finished commit there is newer than its account folder
            if ready is not None and _has_file(ready):
                return ready, ()
            accounts = [d for d in base.iterdir() if d.is_dir() and HEX32.match(d.name)]
            if accounts:
                return max(accounts, key=lambda d: d.stat().st_mtime), ()
    for _ in range(8):   # nested id folders: go down while the folder holds a single folder and no file
        entries = _entries(p)
        dirs = [e for e in entries if e.is_dir()]
        if len(entries) != 1 or len(dirs) != 1:
            break
        p = dirs[0]
    return (p, ()) if _has_file(p) else (None, ())


def waiting(game):
    """(source, names to leave out) of a save put into <game>/saves, or (None, ())."""
    d = saves_dir(game)
    if not d.is_dir():
        return None, ()
    skip = {'README.txt'} | {e.name for e in d.iterdir() if e.is_dir() and OURS.match(e.name)}
    entries = _entries(d, skip)
    if not entries:
        return None, ()
    if len(entries) == 1 and entries[0].is_dir():
        return find_save(entries[0])
    return (d, tuple(sorted(skip))) if any(e.is_file() or _has_file(e) for e in entries) else (None, ())


def _has_file(folder):
    return any(files for _, _, files in os.walk(folder))


def _files(src, skip):
    """Relative paths of the files to import; SaveError when there are too many or too large to be a save."""
    out, size = [], 0
    for root, dirs, files in os.walk(src):
        if Path(root) == Path(src):
            dirs[:] = [d for d in dirs if d not in skip and not d.startswith('.')]
            files = [f for f in files if f not in skip and not f.startswith('.')]
        for f in files:
            p = Path(root) / f
            out.append(p.relative_to(src))
            size += p.stat().st_size
            if len(out) > MAX_FILES or size > MAX_BYTES:
                raise SaveError(f'{src} holds more than a save ({len(out)} files, {size / 1e6:.0f} MB so far); '
                                'choose the folder with the save\'s files')
    if not out:
        raise SaveError(f'{src} holds no files')
    return out


def _ready(tx):
    """The newest finished commit (ready-<n>) in a save-tx folder, or None."""
    ready = []
    if Path(tx).is_dir():
        for d in Path(tx).iterdir():
            m = re.fullmatch(r'ready-(\d+)', d.name)
            if m and d.is_dir():
                ready.append((int(m.group(1)), d))
    return max(ready)[1] if ready else None


def _newest(game, account):
    """The folder the game would load now, in the order of runtime/fs_host.c save_recover: a finished commit still
    waiting in save-tx, else the account folder, else an interrupted commit's new save, else the save it replaced."""
    tx = Path(game) / 'profile' / 'save-tx'
    for d in (_ready(tx), account, tx / 'new', tx / 'old'):
        if d is not None and d.is_dir():
            return d
    return None


def game_running(game):
    """Windows: ocarina.exe cannot be opened for writing while it runs (launcher/build.py game_running)."""
    from .build import game_running as running
    return running(game)


def import_save(game, folder=None, log=print):
    """Make the save in folder (None: the one waiting in <game>/saves) the game's save. Returns {'files', 'source',
    'backup'}; backup is the folder the replaced save went to, or None when there was none."""
    game = Path(game)
    if folder is not None and Path(folder).resolve() == saves_dir(game).resolve():
        folder = None   # the saves folder itself, picked in the dialog: what waits in it, as the button takes it
    from_saves = folder is None
    src, skip = waiting(game) if from_saves else find_save(folder)
    if src is None:
        raise SaveError('the saves folder holds no save' if from_saves else f'{folder} holds no save files')
    prof = (game / 'profile').resolve()
    if Path(src).resolve() == prof or prof in Path(src).resolve().parents:
        raise SaveError('that is the game\'s own save')
    files = _files(src, set(skip))
    account = account_dir(game)
    saves = ensure_saves_dir(game)
    stamp = time.strftime('%Y-%m-%d %H.%M.%S')
    backup = None
    current = _newest(game, account)
    if current is not None and _has_file(current):
        backup = _fresh(saves, f'backup {stamp}')
        shutil.copytree(current, backup)
    tx = game / 'profile' / 'save-tx'
    tx.mkdir(parents=True, exist_ok=True)
    stage = tx / 'import'
    shutil.rmtree(stage, ignore_errors=True)
    for rel in files:
        (stage / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(src) / rel, stage / rel)
        # a save copied from read-only media keeps the attribute through copy2; the game rewrites its files
        os.chmod(stage / rel, stat.S_IREAD | stat.S_IWRITE)
    # The journal of the replaced save goes out of save-tx (the game would load a finished commit first, or complete an
    # interrupted one): what holds files moves into the backup, beside the save it was copied from, so a copy the
    # backup's own choice missed (an interrupted commit's other half) is kept too.
    moved = []
    for d in list(tx.iterdir()):
        if d == stage:
            continue
        if _has_file(d) if d.is_dir() else d.stat().st_size:
            if backup is None:
                backup = _fresh(saves, f'backup {stamp}')
                backup.mkdir()
            (backup / 'save-tx').mkdir(exist_ok=True)
            shutil.move(str(d), str(backup / 'save-tx' / d.name))
            moved.append(d)
        else:
            _rmtree(d) if d.is_dir() else d.unlink(missing_ok=True)
    account.parent.mkdir(parents=True, exist_ok=True)
    # The imported save goes into place before the old one is deleted; a failed rename puts the old one back, and the
    # journal too: a finished commit waiting in save-tx is newer than the account folder, and without it the game
    # would load the older save although nothing was imported.
    gone = None
    try:
        if account.exists():
            gone = _fresh(tx, 'replaced')
            os.replace(account, gone)
        try:
            os.replace(stage, account)
        except OSError:
            if gone is not None:
                os.replace(gone, account)
            raise
    except OSError:
        for d in moved:
            try:
                shutil.move(str(backup / 'save-tx' / d.name), str(d))
            except OSError as e:
                log(f'saves: could not put {d.name} back into {tx}: {e}; it is in {backup / "save-tx"}')
        _rmtree(stage)
        raise
    if gone is not None:
        _rmtree(gone)
    if from_saves:   # what was put into saves/ moves out whole, so the next import does not take any of it again
        done = _fresh(saves, f'imported {stamp}')
        done.mkdir()
        if Path(src) == saves:
            top = sorted({rel.parts[0] for rel in files})
        else:   # the save sits in a folder that was put there (a game folder, an emulator's id folders)
            top = [Path(src).relative_to(saves).parts[0]]
        for name in top:
            shutil.move(str(saves / name), str(done / name))
    log(f'saves: imported {len(files)} files from {src} into {account}' + (f'; the old save is in {backup}' if backup
                                                                          else ''))
    return {'files': len(files), 'source': Path(src), 'backup': backup}
