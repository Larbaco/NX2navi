"""The launcher's long tasks, run on a worker thread: install, update, rebuild and the update check. Each job is a
list of steps the window shows as rows; the window reads the job's state on a timer (rev changes on every change)
and never blocks on it. The same jobs run headless from launcher/cli.py."""
import collections
import threading
import time
import traceback

from . import build, feed, gamefiles, net, toolchain, updater, versions

PENDING, RUNNING, DONE, SKIPPED, FAILED = 'pending', 'running', 'done', 'skipped', 'failed'

STEPS = {
    'tools': ('Gather Build Tools', 'Downloading the compiler and libraries the build uses. This happens once and '
                                    'takes about 170 MB.'),
    'tools_update': ('Update Build Tools', 'Bringing the compiler and libraries to the versions this release was '
                                           'tested with.'),
    'check': ('Inspect Your Game', 'Making sure the dump is Ocarina of Time 1.1.1.'),
    'exefs': ('Unpack the Program', "Unpacking the game's program files."),
    'romfs': ('Pack the Game Data', 'Checking every game file against the dump and packing them into one image.'),
    'lift': ('Translate the Code', "Each function of your game's program becomes C code for the compiler."),
    'compile': ('Compile', 'Compiling the translated code and the port into a Windows program.'),
    'compile_changed': ('Compile Changed Files', 'Compiling the files this version changed. Files that did not '
                                                 'change are kept from the last build.'),
    'shaders': ('Prepare the Shaders', "Translating every shader of the game ahead of time, so play does not stop to "
                                       'compile them.'),
    'ready': ('Ready to Play', 'The game is built.'),
    'download': ('Download {ver}', 'Downloading the new version of NX2navi.'),
    'verify': ('Verify the Download', 'Checking the download against its checksum.'),
    'swap': ('Swap In the New Build', 'Linking the new build. It replaces the old one only once it is complete.'),
}
WEIGHTS = {'tools': 12, 'tools_update': 4, 'check': 1, 'exefs': 2, 'romfs': 14, 'lift': 14, 'compile': 55,
           'compile_changed': 55, 'shaders': 18, 'ready': 0, 'download': 8, 'verify': 1, 'swap': 6}


def mmss(seconds):
    s = int(round(seconds))
    return f'{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}' if s >= 3600 else f'{s // 60}:{s % 60:02d}'


class Step:
    def __init__(self, sid, ver=''):
        label, desc = STEPS[sid]
        self.id, self.label, self.desc = sid, label.format(ver=ver), desc
        self.state, self.value, self.fraction, self.detail = PENDING, 'Waiting', 0.0, ''
        self.t0, self.seconds = None, 0.0


class Job:
    kind = 'job'
    title = ''

    def __init__(self, ctx, step_ids, ver=''):
        self.ctx = ctx
        self.steps = [Step(s, ver) for s in step_ids]
        self.rev = 0
        self.cancel_flag = threading.Event()
        self.proc = None
        self.result = None          # None while running, then 'ok', 'failed' or 'cancelled'
        self.error = ''
        self.lines = collections.deque(maxlen=500)
        self.started = time.time()
        self.finished = None
        self.after = None           # what the window does next ('restart', ...)
        self._lock = threading.Lock()

    # ---- state the window reads
    def step(self, sid):
        for s in self.steps:
            if s.id == sid:
                return s
        return None

    def current(self):
        for s in self.steps:
            if s.state in (RUNNING, FAILED):
                return s
        return next((s for s in self.steps if s.state == PENDING), self.steps[-1])

    def overall(self):
        total = sum(WEIGHTS[s.id] for s in self.steps) or 1
        got = 0.0
        for s in self.steps:
            w = WEIGHTS[s.id]
            got += w if s.state in (DONE, SKIPPED) else w * s.fraction if s.state == RUNNING else 0
        return min(1.0, got / total)

    def eta(self):
        f = self.overall()
        elapsed = time.time() - self.started
        if f < 0.08 or elapsed < 20:
            return None
        return elapsed * (1 - f) / f

    def elapsed(self):
        return (self.finished or time.time()) - self.started

    # ---- changes, from the worker
    def _touch(self):
        self.rev += 1

    def log(self, line):
        self.lines.append(line)
        self.ctx.log(line)
        self._touch()

    def begin(self, sid, detail=''):
        s = self.step(sid)
        if s and s.state != RUNNING:
            s.state, s.fraction, s.detail, s.t0, s.value = RUNNING, 0.0, detail, time.time(), '0%'
            self._touch()

    def progress(self, sid, fraction, detail=''):
        s = self.step(sid)
        if s:
            if s.state == PENDING:
                self.begin(sid)
            s.fraction = max(0.0, min(1.0, fraction))
            s.value = f'{int(s.fraction * 100)}%'
            if detail:
                s.detail = detail
            self._touch()

    def finish(self, sid, value=None):
        s = self.step(sid)
        if s:
            s.seconds = time.time() - s.t0 if s.t0 else 0.0
            s.state, s.fraction = DONE, 1.0
            s.value = value or f'Done · {mmss(s.seconds)}'
            self._touch()

    def skip(self, sid, value='Unchanged'):
        s = self.step(sid)
        if s:
            s.state, s.value = SKIPPED, value
            self._touch()

    def cancelled(self):
        return self.cancel_flag.is_set()

    def cancel(self):
        self.cancel_flag.set()
        if self.proc:
            self.proc.cancel()
        self._touch()

    # ---- running
    def start(self):
        threading.Thread(target=self._main, name=f'job-{self.kind}', daemon=True).start()
        return self

    def run_inline(self):
        self._main()
        return self

    def _main(self):
        try:
            self.run()
            self.result = 'ok'
        except net.Cancelled:
            self.result = 'cancelled'
        except Exception as e:   # every failure ends the job with a message; the log has the traceback
            if self.cancelled():
                self.result = 'cancelled'
            else:
                self.result, self.error = 'failed', str(e) or type(e).__name__
                self.ctx.log(traceback.format_exc())
        finally:
            for s in self.steps:
                if s.state == RUNNING:
                    s.state = FAILED if self.result == 'failed' else PENDING
                    s.value = 'Stopped' if self.result == 'failed' else 'Cancelled'
            self.finished = time.time()
            self._touch()

    def run(self):
        raise NotImplementedError

    # ---- shared pieces
    def ensure_tools(self, sid):
        layout = self.ctx.layout
        self.begin(sid)
        lock = toolchain.load_lock(layout, self.ctx.version)
        if toolchain.is_current(layout, lock):
            have_tools = True
        else:
            have_tools = False

            def prog(phase, done, total):
                if phase == 'download':
                    self.progress(sid, 0.9 * done / max(total, 1),
                                  f'{done / 1e6:.0f} of {total / 1e6:.0f} MB downloaded')
                else:
                    self.progress(sid, 0.9 + 0.1 * done / max(total, 1), f'Unpacking {done} of {total} packages')
            toolchain.install(layout, lock, progress=prog, cancel=self.cancelled, log=self.log,
                              version=self.ctx.version)
        toolchain.ensure_zbic(layout, cancel=self.cancelled, log=self.log, version=self.ctx.version)
        if have_tools:
            self.skip(sid, 'Unchanged' if sid == 'tools_update' else 'Ready')
        else:
            self.finish(sid)

    def make_game(self, args, step_map, link_step=None):
        """Run make_game.py; its steps drive ours through step_map (make_game step -> our step id or None)."""
        def on_event(ev):
            kind = ev.get('ev')
            sid = step_map.get(ev.get('step'))
            if kind == 'step' and sid:
                st = ev.get('state')
                if st == 'start':
                    self.begin(sid)
                elif st == 'done':
                    if sid == 'check':
                        self.finish(sid, 'Version 1.1.1')
                    elif link_step and sid in ('compile', 'compile_changed'):
                        self.finish(sid)
                        self.finish(link_step)
                    else:
                        self.finish(sid)
                elif st == 'skip':
                    self.skip(sid, 'Kept' if sid in ('exefs', 'romfs') else 'Unchanged')
                    if link_step and sid in ('compile', 'compile_changed'):
                        self.skip(link_step, 'Unchanged')
            elif kind == 'progress' and sid:
                done, total, unit = ev.get('done', 0), max(ev.get('total', 1), 1), ev.get('unit', '')
                if unit == 'bytes':
                    detail = f'{done / 1e9:.1f} of {total / 1e9:.1f} GB'
                else:
                    detail = f'{done:,} of {total:,} {unit}'
                self.progress(sid, done / total, detail)
            elif kind == 'phase' and ev.get('phase') == 'link':
                if link_step:
                    s = self.step(step_map.get('compile'))
                    if s and s.state == RUNNING:
                        self.finish(s.id)
                    self.begin(link_step, 'Linking')
                else:
                    self.progress(sid or 'compile', 0.99, 'Linking')
            elif kind == 'error':
                self.error = ev.get('message', '')

        # Cancel pressed before the build started (while the tools were checked) found no process to stop: it stops
        # here, and one pressed while the process starts stops it at once
        if self.cancelled():
            raise net.Cancelled()
        self.proc = build.Run(build.command(self.ctx.layout, args), on_event, self.log)
        if self.cancelled():
            self.proc.cancel()
        code = self.proc.wait()
        err = self.proc.error
        self.proc = None
        if self.cancelled():
            raise net.Cancelled()
        if code:
            raise build.BuildError(err or f'the build stopped (exit code {code})')


class InstallJob(Job):
    kind = 'install'
    title = 'Installing'

    def __init__(self, ctx, dump, game_dir):
        super().__init__(ctx, ['tools', 'check', 'exefs', 'romfs', 'lift', 'compile', 'shaders', 'ready'])
        self.dump, self.game_dir = str(dump), str(game_dir)

    def run(self):
        self.ensure_tools('tools')
        self.make_game(build.install_args(self.ctx.layout, self.dump, self.game_dir),
                       {s: s for s in ('check', 'exefs', 'romfs', 'lift', 'compile', 'shaders')})
        if self.step('shaders').state == PENDING:
            self.skip('shaders')
        self.finish('ready', 'Ready')
        try:
            gamefiles.extract_fonts(self.ctx.layout, self.game_dir)
        except Exception as e:
            self.log(f'fonts: {e}')


class RebuildJob(Job):
    kind = 'rebuild'
    title = 'Rebuilding'

    def __init__(self, ctx, game_dir, dump=''):
        super().__init__(ctx, ['tools_update', 'lift', 'compile_changed', 'swap', 'shaders'])
        self.game_dir, self.dump = str(game_dir), dump

    def run(self):
        self.ensure_tools('tools_update')
        layout = self.ctx.layout
        if build.game_running(self.game_dir):
            raise build.BuildError('the game is running: close it and try again')
        if build.can_rebuild(self.game_dir):
            args = build.rebuild_args(layout, self.game_dir)
        elif self.dump:
            args = build.install_args(layout, self.dump, self.game_dir)
        else:
            raise build.BuildError('the game folder has no earlier build: install the game again')
        self.make_game(args, {'lift': 'lift', 'compile': 'compile_changed', 'shaders': 'shaders'}, link_step='swap')
        for sid in ('lift', 'compile_changed', 'swap', 'shaders'):
            if self.step(sid).state == PENDING:
                self.skip(sid)


class UpdateDownloadJob(Job):
    """Before the restart: download, check and stage the release, then switch app/current.txt to it."""
    kind = 'update'

    def __init__(self, ctx, release):
        ver = versions.display(release.version)
        super().__init__(ctx, ['download', 'verify', 'tools_update', 'lift', 'compile_changed', 'swap', 'shaders'], ver)
        self.release, self.ver = release, ver
        self.title = f'Updating to {ver}'

    def run(self):
        layout = self.ctx.layout
        self.begin('download')
        prog = lambda d, t: self.progress('download', d / max(t, 1), f'{d / 1e6:.1f} of {t / 1e6:.1f} MB')  # noqa: E731
        folder = updater.stage_app(layout, self.release, progress=prog, cancel=self.cancelled,
                                   version=self.ctx.version)
        if updater.runtime_needed(layout, self.release):
            updater.stage_runtime(layout, self.release, progress=prog, cancel=self.cancelled,
                                  version=self.ctx.version)
        self.finish('download')
        self.begin('verify')
        if not (folder / 'launcher' / 'start.pyw').is_file():
            raise updater.UpdateError(f'{folder} is incomplete')
        self.finish('verify', 'Checksum OK')
        if layout.dev:   # a checkout runs from the repository: nothing to switch to
            self.after = 'staged'
            return
        updater.switch_to(layout, self.ver)
        self.after = 'restart'


class ContinueUpdateJob(RebuildJob):
    """After the restart, in the new version: bring the tools to its lock and rebuild the game."""
    kind = 'update'

    def __init__(self, ctx, game_dir, old, dump=''):
        Job.__init__(self, ctx, ['download', 'verify', 'tools_update', 'lift', 'compile_changed', 'swap', 'shaders'],
                     ctx.layout.version)
        self.game_dir, self.dump, self.old = str(game_dir), dump, old
        self.title = f'Updating to {versions.display(ctx.layout.version)}'
        for sid, value in (('download', 'Done'), ('verify', 'Checksum OK')):
            self.step(sid).state, self.step(sid).value, self.step(sid).fraction = DONE, value, 1.0

    def run(self):
        if self.game_dir and build.can_rebuild(self.game_dir):
            RebuildJob.run(self)
        else:
            self.ensure_tools('tools_update')
            for sid in ('lift', 'compile_changed', 'swap', 'shaders'):
                self.skip(sid, 'No game yet')
        updater.prune(self.ctx.layout)


class CheckJob(Job):
    kind = 'check'

    def __init__(self, ctx):
        super().__init__(ctx, [])
        self.release = None

    def run(self):
        s = self.ctx.settings
        self.release = feed.latest(feed.load(self.ctx.layout, s), s.get('channel', 'stable'),
                                   s.get('github_token', ''), self.ctx.version)

    def overall(self):
        return 0.0


def update_available(ctx, release):
    return bool(release) and versions.newer(release.version, ctx.layout.version)
