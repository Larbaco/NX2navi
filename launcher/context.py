"""What every part of the launcher shares: the folder layout, the settings and the log (data/logs/launcher.log,
the run before it in launcher.1.log)."""
import os
import threading
import time

from .paths import Layout
from .settings import Settings

LOG_LIMIT = 4 << 20


class Context:
    def __init__(self, layout=None, echo=False):
        self.layout = (layout or Layout()).ensure()
        self.settings = Settings(self.layout.settings)
        self.version = self.layout.version
        self.echo = echo
        self._lock = threading.Lock()
        path = self.layout.logs / 'launcher.log'
        try:
            if path.is_file() and path.stat().st_size > LOG_LIMIT:
                os.replace(path, path.with_name('launcher.1.log'))
        except OSError:
            pass
        try:
            self._file = open(path, 'a', encoding='utf-8', errors='replace')
        except OSError:
            self._file = None
        self.log(f'--- NX2navi launcher {self.version} ({"checkout" if self.layout.dev else "release"}) in '
                 f'{self.layout.root}')
        if self.settings.unreadable:
            self.log(f'{self.layout.settings} is not valid JSON: the settings start from their defaults; the file as '
                     'it was is kept as launcher.json.bad')

    def log(self, line):
        stamp = time.strftime('%H:%M:%S')
        with self._lock:
            if self._file:
                for part in str(line).splitlines() or ['']:
                    self._file.write(f'{stamp} {part}\n')
                self._file.flush()
        if self.echo:
            print(line, flush=True)

    def log_tail(self, n=80):
        try:
            with open(self.layout.logs / 'launcher.log', encoding='utf-8', errors='replace') as f:
                return f.readlines()[-n:]
        except OSError:
            return []
