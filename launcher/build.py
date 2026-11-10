"""Running tools/make_game.py for the launcher: the command line, its "@@" progress lines (make_game.py --events)
and cancelling it with every compiler it started."""
import json
import os
import signal
import stat
import subprocess
import threading

# Runs make_game.py with pydeps/ (zbic) and the script's folder first on sys.path; the embeddable Python's ._pth
# file leaves both off.
RUN = ('import os, runpy, sys; p = sys.argv[1]; s = sys.argv[2]; sys.argv = sys.argv[2:]; '
       'sys.path[:0] = [x for x in p.split(os.pathsep) if x] + [os.path.dirname(os.path.abspath(s))]; '
       'runpy.run_path(s, run_name="__main__")')
CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_PROCESS_GROUP = 0x00000200


def command(layout, args):
    return [layout.python_for_builds(), '-I', '-c', RUN, str(layout.pydeps), str(layout.make_game), *map(str, args),
            '--events']


def has_prompt_art(layout):
    """True when this version holds button art (app/<version>/prompt-art, tools/prompt_art.py FOLDER): make_game.py
    then draws the button prompts in a step of its own, and the jobs show a row for it."""
    return (layout.app / 'prompt-art').is_dir()


def install_args(layout, dump, game_dir):
    dump = str(dump)
    if os.path.isdir(dump):
        src = ['--exefs', os.path.join(dump, 'exefs'), '--romfs', os.path.join(dump, 'romfs')]
    else:
        src = ['--nup', dump]
    return [*src, '--out', str(game_dir), '--msys', str(layout.msys)]


def rebuild_args(layout, game_dir, retune=False):
    """retune: Rebuild the Game, which compiles the game's code for this processor's level; an update keeps the level
    its objects were compiled for, so that it compiles only what changed (tools/make_game.py lifted_level)."""
    return ['--rebuild', *(['--retune'] if retune else []), '--out', str(game_dir), '--msys', str(layout.msys)]


def check_args(dump):
    dump = str(dump)
    if os.path.isdir(dump):
        return ['--exefs', os.path.join(dump, 'exefs'), '--romfs', os.path.join(dump, 'romfs'), '--check']
    return ['--nup', dump, '--check']


class BuildError(Exception):
    pass


class Run:
    """One make_game.py process. on_event(dict) for each "@@" line, on_line(str) for every other line; wait()
    returns the exit code. cancel() ends the process and its children."""

    def __init__(self, cmd, on_event, on_line, cwd=None):
        self.error = None
        kw = {}
        if os.name == 'nt':
            kw['creationflags'] = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
        else:
            kw['start_new_session'] = True
        env = dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     cwd=cwd, env=env, text=True, encoding='utf-8', errors='replace', bufsize=1, **kw)
        self.cancelled = False
        self._reader = threading.Thread(target=self._read, args=(on_event, on_line), daemon=True)
        self._reader.start()

    def _read(self, on_event, on_line):
        for line in self.proc.stdout:
            line = line.rstrip('\r\n')
            if line.startswith('@@'):
                try:
                    ev = json.loads(line[2:])
                except ValueError:
                    on_line(line)
                    continue
                if ev.get('ev') == 'error':
                    self.error = ev.get('message')
                on_event(ev)
            elif line.startswith('error: ') and not self.error:
                self.error = line[7:]
                on_line(line)
            else:
                on_line(line)

    def wait(self):
        code = self.proc.wait()
        self._reader.join(5)
        return code

    def cancel(self):
        self.cancelled = True
        if self.proc.poll() is not None:
            return
        if os.name == 'nt':
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.proc.pid)], capture_output=True,
                           creationflags=CREATE_NO_WINDOW)
        else:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass


def game_running(game_dir, exe='ocarina.exe'):
    """True when the built game runs from game_dir: Windows refuses to open a running exe for writing. A read-only
    exe (a copy restored from a backup or read-only media) is refused too, though nothing runs it: its read-only
    attribute is cleared and the open tried again, as the next build replaces the file anyway."""
    path = os.path.join(str(game_dir), exe)
    if os.name != 'nt' or not os.path.isfile(path):
        return False
    for attempt in range(2):
        try:
            with open(path, 'r+b'):
                return False
        except PermissionError:
            if attempt or not os.stat(path).st_file_attributes & stat.FILE_ATTRIBUTE_READONLY:
                return True
            try:
                os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
            except OSError:
                return True
        except OSError:
            return False
    return True


def build_state(game_dir):
    """The game folder's build-cache/state.json (make_game.py), or {}."""
    try:
        with open(os.path.join(str(game_dir), 'build-cache', 'state.json'), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def built_version(game_dir):
    """The kit version that last built the game folder ('' when none finished)."""
    st = build_state(game_dir)
    if 'compile' in st.get('steps', {}) and os.path.isfile(os.path.join(str(game_dir), 'ocarina.exe')):
        return st.get('kit', '')
    return ''


def can_rebuild(game_dir):
    st = build_state(game_dir)
    return st.get('version') == 2 and all(s in st.get('steps', {}) for s in ('exefs', 'romfs'))
