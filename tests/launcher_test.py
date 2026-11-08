"""launcher/ and tools/make_game.py's bookkeeping, without game files or Windows.

Run: python tests/launcher_test.py   (Python 3.14 for the zstd tests; the window test needs tkinter, numpy and a display)

- versions, settings
- toolchain: the package database, dependency resolution and an install from a local fake repository
- net: downloads that resume, check their sha256 and stop on cancel; the certificate checks against a local HTTPS
  server with certificates the openssl command makes (an expired one, an unknown one), the clock comparison
- feed: GitHub releases (stable, preview, private with a token) and a plain manifest URL, served locally
- updater: staging a release, switching, rollback, pruning, refusing unsafe archives
- gamefiles: the .bfotf font wrapping and zstd dictionaries, on a font of this system made into a fake RomFS
- make_game.py: a synthetic .nup through the check, exefs and romfs steps with --events, skipped steps on a second
  run, --rebuild without the game files, the object cache of the compile step with the system's C compiler, and its
  windres call under folders with spaces (a script in windres's place)
- jobs: an install driven by a fake make_game.py
- the window: --selftest draws every screen (skipped without a display)
"""
import contextlib
import hashlib
import http.server
import io
import json
import os
import shutil
import ssl
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))

from launcher import build, feed, gamefiles, jobs, net, saves, settings, toolchain, updater, versions, winsys  # noqa: E402
from launcher.context import Context  # noqa: E402
from launcher.paths import Layout  # noqa: E402

try:
    from compression import zstd as ZSTD
except ImportError:
    ZSTD = None


# ---------------------------------------------------------------- helpers

class Server:
    """A local HTTP server for a folder, with extra routes (path -> (status, headers, body)) and Range support; HTTPS
    with cert (a PEM file holding the certificate and its key). HEAD answers a redirect to https, as a server that
    serves only HTTPS does."""

    def __init__(self, folder, routes=None, cert=None):
        folder, routes = Path(folder), routes if routes is not None else {}
        self.hits = []
        server = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_HEAD(self):
                server.hits.append((self.path, dict(self.headers)))
                self.send_response(301)
                self.send_header('Location', 'https://127.0.0.1' + self.path)
                self.send_header('Content-Length', '0')
                self.end_headers()

            def do_GET(self):
                server.hits.append((self.path, dict(self.headers)))
                if self.path in routes:
                    status, headers, body = routes[self.path]
                    self.send_response(status)
                    for k, v in headers.items():
                        self.send_header(k, v)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                p = folder / self.path.lstrip('/').split('?')[0]
                if not p.is_file():
                    self.send_error(404)
                    return
                data = p.read_bytes()
                rng = self.headers.get('Range')
                if rng and rng.startswith('bytes='):
                    start = int(rng[6:].split('-')[0])
                    if start >= len(data):
                        self.send_error(416)
                        return
                    self.send_response(206)
                    self.send_header('Content-Range', f'bytes {start}-{len(data) - 1}/{len(data)}')
                    data = data[start:]
                else:
                    self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except ConnectionError:   # the client stopped reading (the cancel test)
                    pass

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}/'
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert)
            self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
            self.url = 'https' + self.url[4:]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_cert(folder, start, end):
    """A self-signed certificate for 127.0.0.1 valid from start to end (epoch seconds), with its key, as one PEM file
    in the new folder; made by the openssl command (its ca command takes any dates, its req command none in the
    past)."""
    folder = Path(folder)
    folder.mkdir(parents=True)
    (folder / 'index.txt').write_text('')
    (folder / 'serial.txt').write_text('01\n')
    (folder / 'ca.cnf').write_text('[ca]\ndefault_ca = d\n[d]\ndatabase = index.txt\nserial = serial.txt\n'
                                   'new_certs_dir = .\ndefault_md = sha256\npolicy = p\ncopy_extensions = copy\n'
                                   'unique_subject = no\n[p]\ncommonName = supplied\n')
    def stamp(t):
        return time.strftime('%Y%m%d%H%M%SZ', time.gmtime(t))

    def run(*args):
        subprocess.run(['openssl', *args], cwd=folder, check=True, capture_output=True)

    run('req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-keyout', 'key.pem',
        '-out', 'req.csr', '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1')
    run('ca', '-batch', '-config', 'ca.cnf', '-selfsign', '-keyfile', 'key.pem', '-in', 'req.csr', '-out', 'cert.pem',
        '-startdate', stamp(start), '-enddate', stamp(end), '-notext')
    pem = folder / 'both.pem'
    pem.write_bytes((folder / 'cert.pem').read_bytes() + (folder / 'key.pem').read_bytes())
    return pem


class CountingContext(ssl.SSLContext):
    """A client SSLContext that counts its handshakes."""

    def wrap_socket(self, *args, **kwargs):
        self.handshakes = getattr(self, 'handshakes', 0) + 1
        return super().wrap_socket(*args, **kwargs)


def client_context(trust=None):
    ctx = CountingContext(ssl.PROTOCOL_TLS_CLIENT)
    if trust:
        ctx.load_verify_locations(cafile=trust)
    else:
        ctx.load_default_certs()
    return ctx


def tar_bytes(files, compress=True):
    """A tar (zstd-compressed when the stdlib has zstd, else gzip) of {name: bytes}."""
    buf = io.BytesIO()
    mode = 'w:gz' if not (compress and ZSTD) else 'w'
    with tarfile.open(fileobj=buf, mode=mode) as t:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    data = buf.getvalue()
    return ZSTD.compress(data) if compress and ZSTD else data


def desc(name, version, filename, data, depends=(), provides=()):
    lines = ['%FILENAME%', filename, '', '%NAME%', name, '', '%VERSION%', version, '', '%CSIZE%', str(len(data)), '',
             '%ISIZE%', str(len(data) * 3), '', '%SHA256SUM%', sha(data), '']
    if depends:
        lines += ['%DEPENDS%', *depends, '']
    if provides:
        lines += ['%PROVIDES%', *provides, '']
    return '\n'.join(lines).encode()


def fake_repo(folder, damage=None):
    """A pacman repository with the ten root packages (each holding one of make_game.py's required files), one
    dependency and one name provided by another package."""
    P = toolchain.PREFIX
    files = {'clang': {'ucrt64/bin/clang.exe': b'clang', 'ucrt64/bin/clang++.exe': b'clang++'},
             'sdl3': {'ucrt64/bin/SDL3.dll': b'sdl', 'ucrt64/lib/libSDL3.dll.a': b'implib'},
             'vulkan-memory-allocator': {'ucrt64/include/vma/vk_mem_alloc.h': b'// vma'},
             'vulkan-loader': {'ucrt64/lib/libvulkan-1.dll.a': b'implib'},
             'llvm-libs': {'ucrt64/bin/libLLVM.dll': b'llvm', 'ucrt64/share/doc/llvm/README': b'skipped'}}
    deps = {'clang': [P + 'llvm-libs=23.1', P + 'gcc'], 'sdl3': [P + 'vulkan']}
    entries = {}
    for root in list(toolchain.ROOTS) + ['llvm-libs']:
        name = P + root
        fn = f'{name}-1.0-1-any.pkg.tar.zst'
        data = tar_bytes({'.PKGINFO': b'pkg', **files.get(root, {f'ucrt64/share/{root}/x': b'x'})})
        if damage == root:
            (folder / fn).write_bytes(data[:-3] + b'bad')
        else:
            (folder / fn).write_bytes(data)
        entries[f'{name}-1.0-1/desc'] = desc(name, '1.0-1', fn, data, deps.get(root, ()),
                                             [P + 'vulkan'] if root == 'vulkan-loader' else ())
    (folder / 'ucrt64.db').write_bytes(tar_bytes(entries))


def fake_fast_repo(folder, clang_version='1.0-1'):
    """The CLANG64 packages toolchain.FAST_PACKAGES names, in folder/clang64."""
    folder = folder / 'clang64'
    folder.mkdir()
    P = toolchain.FAST_PREFIX
    files = {'clang': {'clang64/bin/clang.exe': b'clang64', 'clang64/bin/clang-repl.exe': b'not needed',
                       'clang64/lib/clang/23/include/immintrin.h': b'// x86'},
             'llvm-libs': {'clang64/bin/libLLVM-23.dll': b'llvm', 'clang64/share/doc/x': b'skipped'}}
    entries = {}
    for root in toolchain.FAST_PACKAGES:
        name, ver = P + root, clang_version if root == 'clang' else '1.0-1'
        fn = f'{name}-{ver}-any.pkg.tar.zst'
        data = tar_bytes({'.PKGINFO': b'pkg', **files.get(root, {f'clang64/bin/{root}.dll': b'dll'})})
        (folder / fn).write_bytes(data)
        entries[f'{name}-{ver}/desc'] = desc(name, ver, fn, data, (), ())
    (folder / 'clang64.db').write_bytes(tar_bytes(entries))


class Tmp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-test-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.layout = Layout(root=self.tmp / 'launcher', app=ROOT).ensure()


# ---------------------------------------------------------------- versions, settings

class VersionTest(unittest.TestCase):
    def test_order(self):
        self.assertTrue(versions.newer('0.3.11', '0.3.10'))
        self.assertTrue(versions.newer('0.4', '0.3.99'))
        self.assertFalse(versions.newer('0.3.10+abc', '0.3.10'))
        self.assertTrue(versions.newer('1.0.0', '1.0.0-rc1'))
        self.assertEqual(versions.display('0.3.11+abc1234'), '0.3.11')
        with self.assertRaises(ValueError):
            versions.parse('banana')


class SettingsTest(Tmp):
    def test_roundtrip_and_reset(self):
        s = settings.Settings(self.layout.settings)
        self.assertTrue(s['auto_check'])
        s['auto_check'], s['game_dir'], s['channel'] = False, 'D:/g', 'preview'
        s.save()
        t = settings.Settings(self.layout.settings)
        self.assertEqual((t['auto_check'], t['game_dir'], t['channel']), (False, 'D:/g', 'preview'))
        t.reset()
        self.assertEqual((t['auto_check'], t['game_dir'], t['channel']), (True, 'D:/g', 'stable'))

    def test_bad_types_ignored(self):
        self.layout.settings.write_text(json.dumps({'auto_check': 'yes', 'channel': 'preview', 'junk': 1}))
        s = settings.Settings(self.layout.settings)
        self.assertEqual((s['auto_check'], s['channel']), (True, 'preview'))
        self.assertNotIn('junk', s)

    def test_bom_and_typo(self):
        # a file saved by an editor with a BOM reads as written; one with a typo keeps a copy before a save replaces it
        self.layout.settings.write_bytes(b'\xef\xbb\xbf' + json.dumps({'game_dir': 'D:/g'}).encode())
        s = settings.Settings(self.layout.settings)
        self.assertEqual(s['game_dir'], 'D:/g')
        self.assertFalse(s.unreadable)
        typo = '{"game_dir": "D:/g", "github_token": "t",}'
        self.layout.settings.write_text(typo, encoding='utf-8')
        s = settings.Settings(self.layout.settings)
        self.assertTrue(s.unreadable)
        self.assertEqual(s['game_dir'], '')
        s.save()
        self.assertEqual(self.layout.settings.with_name('launcher.json.bad').read_text(encoding='utf-8'), typo)


class UninstallTest(Tmp):
    def tree(self, root, rels):
        for rel in rels:
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('x')

    def test_plan_keeps_the_game_and_what_is_not_ours(self):
        from types import SimpleNamespace
        root = self.tmp / 'NX2navi'
        self.tree(root, ('NX2navi.exe', 'app/current.txt', 'python.old-3/x', 'game/profile/save/1/a.sav', 'mine.txt'))
        layout = SimpleNamespace(root=root, toolchain=root / 'toolchain')
        gone, empty = winsys.uninstall_plan(layout)
        self.assertNotIn(root / 'game', gone)
        self.assertNotIn(root, gone)
        self.assertIn(root / 'python.old-3', gone)
        self.assertEqual(empty, [root])
        gone, _ = winsys.uninstall_plan(layout, str(root / 'game'))
        self.assertEqual(gone[0], root / 'game')
        # the toolchain moved to C:\NX2navi: that folder goes only when nothing else is left in it
        other = self.tmp / 'C NX2navi' / 'toolchain'
        gone, empty = winsys.uninstall_plan(SimpleNamespace(root=root, toolchain=other))
        self.assertIn(other, gone)
        self.assertNotIn(other.parent, gone)
        self.assertEqual(empty, [root, other.parent])

    @unittest.skipUnless(os.name == 'nt', 'runs the cmd.exe script')
    def test_script_on_windows(self):
        from types import SimpleNamespace
        root = self.tmp / 'NX2navi ÿ x'   # spaces and a character outside ASCII
        self.tree(root, ('NX2navi.exe', 'app/1/VERSION', 'python/pythonw.exe', 'python.old-12/x', 'pydeps/z',
                         'data/launcher.json', 'toolchain/msys64/a', 'game/profile/save/1/a.sav', 'mine.txt'))
        layout = SimpleNamespace(root=root, toolchain=root / 'toolchain')
        script = winsys.uninstall_script(*winsys.uninstall_plan(layout)).replace('ping -n 3 127.0.0.1 >nul', 'echo')
        subprocess.run(f'cmd.exe /c "{script}"', capture_output=True)
        self.assertEqual(sorted(p.name for p in root.iterdir()), ['game', 'mine.txt'])
        self.assertTrue((root / 'game/profile/save/1/a.sav').is_file())
        (root / 'mine.txt').unlink()
        script = winsys.uninstall_script(*winsys.uninstall_plan(layout, str(root / 'game')))
        subprocess.run(f'cmd.exe /c "{script.replace("ping -n 3 127.0.0.1 >nul", "echo")}"', capture_output=True)
        self.assertFalse(root.exists())


# ---------------------------------------------------------------- toolchain

class ToolchainTest(Tmp):
    def test_resolve_and_install(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url)
        names = [p['name'] for p in lock['packages']]
        self.assertEqual(names[0], toolchain.PREFIX + 'clang')
        self.assertIn(toolchain.PREFIX + 'llvm-libs', names)              # a dependency with a version constraint
        self.assertEqual(names.count(toolchain.PREFIX + 'vulkan-loader'), 1)   # 'vulkan' resolves to its provider
        self.assertFalse(toolchain.is_current(self.layout, lock))
        phases = []
        toolchain.install(self.layout, lock, progress=lambda ph, d, t: phases.append(ph), log=lambda m: None)
        self.assertTrue(toolchain.is_current(self.layout, lock))
        self.assertTrue((self.layout.msys / 'bin' / 'clang.exe').is_file())
        self.assertFalse((self.layout.msys / 'share' / 'doc').exists())    # documentation left out
        self.assertIn('download', phases)
        self.assertIn('unpack', phases)
        hits = len(srv.hits)
        toolchain.install(self.layout, lock, log=lambda m: None)           # cached package files: no download
        self.assertEqual(len(srv.hits), hits)
        self.assertEqual(toolchain.lock_digest(lock), toolchain.installed(self.layout)['digest'])

    def test_fast_clang_beside(self):
        """CLANG64's clang goes into msys64/clang64 when its version matches UCRT64's: clang.exe, the DLLs and its
        headers only."""
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        fake_fast_repo(repo)
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url, fast_repo=srv.url + 'clang64/')
        self.assertEqual(lock.get('fast_repo'), srv.url + 'clang64/')
        self.assertEqual(sum(p.get('env') == 'clang64' for p in lock['packages']), len(toolchain.FAST_PACKAGES))
        toolchain.install(self.layout, lock, log=lambda m: None)
        fast = self.layout.msys.parent / 'clang64'
        self.assertEqual((fast / 'bin' / 'clang.exe').read_bytes(), b'clang64')
        self.assertTrue((fast / 'bin' / 'libLLVM-23.dll').is_file())
        self.assertTrue((fast / 'lib' / 'clang' / '23' / 'include' / 'immintrin.h').is_file())
        self.assertFalse((fast / 'bin' / 'clang-repl.exe').exists())
        self.assertFalse((fast / 'share').exists())
        self.assertTrue(toolchain.is_current(self.layout, lock))

    def test_fast_clang_left_out_on_another_version(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        fake_fast_repo(repo, clang_version='2.0-1')
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url, fast_repo=srv.url + 'clang64/')
        self.assertNotIn('fast_repo', lock)
        self.assertIn('2.0-1', lock['fast_skipped'])
        self.assertFalse(any(p.get('env') for p in lock['packages']))

    def test_damaged_package_refused(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo, damage='opus')
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url)
        with self.assertRaises(toolchain.ToolchainError):
            toolchain.install(self.layout, lock, log=lambda m: None)
        self.assertFalse(toolchain.is_current(self.layout, lock))

    def test_wheel_tag(self):
        tag = toolchain.wheel_tag()[0]
        self.assertTrue(tag.startswith(f'cp{sys.version_info.major}{sys.version_info.minor}-'))


# ---------------------------------------------------------------- net

class NetTest(Tmp):
    def test_download_resume_and_checks(self):
        data = os.urandom(3 * net.CHUNK + 123)
        (self.tmp / 'f.bin').write_bytes(data)
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        dest = self.tmp / 'out' / 'f.bin'
        dest.parent.mkdir()
        (dest.parent / 'f.bin.part').write_bytes(data[:1000])             # a part file from a cut connection
        net.download(srv.url + 'f.bin', dest, sha256=sha(data), size=len(data))
        self.assertEqual(dest.read_bytes(), data)
        self.assertEqual(srv.hits[-1][1].get('Range'), 'bytes=1000-')
        with self.assertRaises(net.DownloadError):
            net.download(srv.url + 'f.bin', self.tmp / 'out' / 'g.bin', sha256='0' * 64)
        self.assertFalse((self.tmp / 'out' / 'g.bin').exists())
        with self.assertRaises(net.Cancelled):
            net.download(srv.url + 'f.bin', self.tmp / 'out' / 'h.bin', cancel=lambda: True)
        with self.assertRaises(net.DownloadError):
            net.download(srv.url + 'missing.bin', self.tmp / 'out' / 'm.bin')

    def test_server_clock(self):
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        t = net.server_clock(srv.url)          # the Date of the redirect to https, which is not followed
        self.assertIsNotNone(t)
        self.assertLess(abs(t - time.time()), 5)
        self.assertEqual([h[0] for h in srv.hits], ['/'])
        srv.close()
        self.assertIsNone(net.server_clock(srv.url, timeout=2))


@unittest.skipUnless(shutil.which('openssl'), 'needs the openssl command')
class CertificateTest(Tmp):
    def setUp(self):
        super().setUp()
        saved = net._CHECKS, dict(net._ACCEPTED), net.server_clock
        self.addCleanup(lambda: setattr(net, '_CHECKS', saved[0]) or setattr(net, '_ACCEPTED', saved[1]) or
                        setattr(net, 'server_clock', saved[2]))
        net._ACCEPTED = {}
        self.data = os.urandom(100_000)
        (self.tmp / 'f.bin').write_bytes(self.data)

    def serve(self, start, end):
        cert = make_cert(self.tmp / f'cert{start}', start, end)
        srv = Server(self.tmp, cert=cert)
        self.addCleanup(srv.close)
        return srv, cert

    def test_refused_certificate(self):
        """An expired certificate in the trust store: the player's 'certificate has expired', at once, not retried,
        with the advice the server's clock calls for."""
        now = time.time()
        srv, cert = self.serve(now - 400 * 86400, now - 300 * 86400)
        check = client_context(trust=cert)
        net._CHECKS = [("Python's check", check)]
        asked = []
        cases = ((now + 400 * 86400, "This PC's clock reads"), (now - 3 * 86400, 'set the date, time and time zone'),
                 (now + 60, "This PC's clock agrees with 127.0.0.1's. Security software"),
                 (None, "Check that this PC's date, time and time zone are right"))
        for i, (theirs, says) in enumerate(cases):
            net.server_clock = lambda url, timeout=10, version='dev', t=theirs: asked.append(url) or t
            t0 = time.time()
            with self.assertRaises(net.CertificateError) as cm:
                net.download(srv.url + 'f.bin', self.tmp / 'out' / 'f.bin', sha256=sha(self.data))
            self.assertLess(time.time() - t0, 5)                 # no retries: the answer would be the same
            msg = str(cm.exception)
            self.assertIn("the certificate of 127.0.0.1 was not accepted (Python's check: certificate has expired)",
                          msg)
            self.assertIn(says, msg)
            self.assertEqual(check.handshakes, i + 1)
        self.assertEqual(asked, ['http://127.0.0.1/'] * len(cases))   # the clock is asked over plain HTTP
        self.assertEqual(srv.hits, [])                            # no request got through
        self.assertFalse((self.tmp / 'out' / 'f.bin.part').exists())

    def test_second_check_accepts(self):
        """A certificate the first check refuses and the second accepts downloads, and the second goes first for that
        host from then on."""
        now = time.time()
        srv, cert = self.serve(now - 86400, now + 2 * 86400)
        refuses, accepts = client_context(), client_context(trust=cert)
        net._CHECKS = [("Python's check", refuses), ("Windows' check", accepts)]
        dest = net.download(srv.url + 'f.bin', self.tmp / 'out' / 'f.bin', sha256=sha(self.data))
        self.assertEqual(dest.read_bytes(), self.data)
        self.assertEqual((refuses.handshakes, accepts.handshakes), (1, 1))
        self.assertEqual(net._ACCEPTED, {'127.0.0.1': 1})
        self.assertEqual(net.get_bytes(srv.url + 'f.bin'), self.data)
        self.assertEqual((refuses.handshakes, accepts.handshakes), (1, 2))
        net._CHECKS = [("Python's check", refuses)]
        net.server_clock = lambda *a, **k: None
        with self.assertRaises(net.CertificateError) as cm:
            net.get_bytes(srv.url + 'f.bin')
        self.assertIn("(Python's check: self-signed certificate)", str(cm.exception))


# ---------------------------------------------------------------- feed and updater

def make_app_zip(folder, ver, extra=None):
    app = folder / f'app-{ver}'
    (app / 'launcher').mkdir(parents=True)
    (app / 'launcher' / 'start.pyw').write_text('# test\n')
    (app / 'VERSION').write_text(ver + '\n')
    z = folder / f'NX2navi-app-{ver}.zip'
    with zipfile.ZipFile(z, 'w') as zf:
        for p in app.rglob('*'):
            if p.is_file():
                zf.write(p, p.relative_to(app).as_posix())
        for name, data in (extra or {}).items():
            zf.writestr(name, data)
    return z


def manifest_for(z, ver):
    return {'format': 1, 'version': ver, 'notes': '- One\n- Two',
            'app': {'name': z.name, 'size': z.stat().st_size, 'sha256': sha(z.read_bytes())}}


class FeedTest(Tmp):
    def serve_github(self):
        self.routes = {}
        srv = Server(self.tmp, self.routes)
        self.addCleanup(srv.close)
        return srv

    def publish(self, releases):
        self.routes['/repos/o/r/releases?per_page=20'] = (200, {'Content-Type': 'application/json'},
                                                          json.dumps(releases).encode())

    def release(self, srv, tag, ver, pre=False, manifest=True):
        z = make_app_zip(self.tmp, ver)
        m = self.tmp / f'm-{ver}.json'
        m.write_text(json.dumps(manifest_for(z, ver)))
        assets = [{'name': z.name, 'browser_download_url': srv.url + z.name, 'url': srv.url + z.name}]
        if manifest:
            assets.append({'name': feed.MANIFEST, 'browser_download_url': srv.url + m.name, 'url': srv.url + m.name})
        return {'tag_name': tag, 'prerelease': pre, 'draft': False, 'body': 'notes', 'html_url': 'x', 'assets': assets}

    def test_github_channels(self):
        srv = self.serve_github()
        self.publish([self.release(srv, 'v0.4.0-rc1', '0.4.0-rc1', pre=True),
                      self.release(srv, 'v0.3.12', '0.3.12', manifest=False), self.release(srv, 'v0.3.11', '0.3.11')])
        f = {'type': 'github', 'repo': 'o/r', 'api': srv.url.rstrip('/')}
        self.assertEqual(feed.latest(f, 'stable').version, '0.3.11')    # 0.3.12 lacks a manifest
        self.assertEqual(feed.latest(f, 'preview').version, '0.4.0-rc1')
        r = feed.latest(f, 'stable', token='secret')
        self.assertEqual(r.headers.get('Authorization'), 'Bearer secret')
        self.assertTrue(any(h.get('Authorization') == 'Bearer secret' for _, h in srv.hits))
        self.assertEqual(feed.plain_notes('## Head\n- **One**\n* `Two`\n<!-- x -->'), ['Head', 'One', 'Two'])

    def test_url_feed_and_update(self):
        z = make_app_zip(self.tmp, '0.9.0')
        (self.tmp / feed.MANIFEST).write_text(json.dumps(manifest_for(z, '0.9.0')))
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        rel = feed.latest({'type': 'url', 'url': srv.url + feed.MANIFEST})
        self.assertEqual(rel.version, '0.9.0')
        self.layout.apps.mkdir(parents=True)
        (self.layout.apps / 'current.txt').write_text('0.8.0\n')
        old = self.layout.apps / '0.8.0' / 'launcher'
        old.mkdir(parents=True)
        (old / 'start.pyw').write_text('')
        for stale in ('0.7.0', '0.6.0'):
            (self.layout.apps / stale).mkdir()
        folder = updater.stage_app(self.layout, rel)
        self.assertTrue((folder / 'launcher' / 'start.pyw').is_file())
        updater.switch_to(self.layout, '0.9.0')
        self.assertEqual((updater.current(self.layout), updater.previous(self.layout)), ('0.9.0', '0.8.0'))
        updater.prune(self.layout)
        self.assertEqual(sorted(p.name for p in self.layout.apps.iterdir() if p.is_dir()), ['0.8.0', '0.9.0'])
        self.assertTrue(updater.rollback(self.layout))
        self.assertEqual(updater.current(self.layout), '0.8.0')
        self.assertFalse(updater.rollback(self.layout))                  # nothing further back

    def test_unsafe_or_wrong_archives(self):
        z = make_app_zip(self.tmp, '1.0.0', extra={'../evil.txt': 'x'})
        with self.assertRaises(updater.UpdateError):
            updater.safe_extract(z, self.tmp / 'x')
        self.assertFalse((self.tmp / 'evil.txt').exists())
        z2 = make_app_zip(self.tmp, '1.0.1')
        m = manifest_for(z2, '1.0.2')                                     # the archive says 1.0.1
        (self.tmp / feed.MANIFEST).write_text(json.dumps(m))
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        rel = feed.Release(m, assets={z2.name: srv.url + z2.name})
        with self.assertRaises(updater.UpdateError):
            updater.stage_app(self.layout, rel)
        self.assertFalse((self.layout.apps / '1.0.2').exists())


# ---------------------------------------------------------------- gamefiles

def sarc(files):
    names = b''
    offs = []
    for n in files:
        offs.append(len(names))
        b = n.encode() + b'\0'
        names += b + b'\0' * (-len(b) % 4)
    data, ents = b'', b''
    for (n, d), no in zip(files.items(), offs):
        start = len(data)
        data += d
        ents += struct.pack('<IIII', 0, 0x01000000 | (no // 4), start, len(data))
        data += b'\0' * (-len(data) % 8)
    sfat = b'SFAT' + struct.pack('<HHI', 0xC, len(files), 0x65) + ents
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names
    base = 0x14 + len(sfat) + len(sfnt)
    head = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, base + len(data), base, 0x100, 0)
    return head + sfat + sfnt + data


def bfotf(otf, key=0x1234ABCD):
    body = otf + b'\0' * (-len(otf) % 4)
    words = struct.unpack(f'>{len(body) // 4}I', body)
    return struct.pack('>II', 0x7F9A0218 ^ key, len(otf) ^ key) + struct.pack(f'>{len(words)}I',
                                                                             *(w ^ key for w in words))


@unittest.skipUnless(ZSTD, 'needs compression.zstd (Python 3.14)')
class FontTest(Tmp):
    FONT = Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')

    @unittest.skipUnless(FONT.is_file(), 'needs DejaVu Sans')
    def test_font_from_romfs_image(self):
        otf = self.FONT.read_bytes()
        self.assertEqual(gamefiles.bfotf_to_otf(bfotf(otf)), otf)
        self.assertIsNone(gamefiles.bfotf_to_otf(b'\0' * 64))
        samples = [otf[i:i + 512] for i in range(0, min(len(otf), 400000), 997)]
        d = ZSTD.train_dict(samples, 16384)
        packed = ZSTD.compress(bfotf(otf), zstd_dict=d)
        self.assertNotEqual(ZSTD.get_frame_info(packed).dictionary_id, 0)
        files = {'Pack/ZsDic.pack.zs': ZSTD.compress(sarc({'zs.zsdic': d.dict_content})),
                 gamefiles.FONTS['title']: packed}
        romfs = self.tmp / 'game' / 'romfs'
        romfs.mkdir(parents=True)
        entries, blob = [], b''
        for path, data in sorted(files.items()):
            blob += b'\0' * (-len(blob) % 16)
            entries.append({'path': path, 'offset': len(blob), 'size': len(data)})
            blob += data
        (romfs / 'romfs.bin').write_bytes(blob)
        (romfs / 'index.json').write_text(json.dumps({'files': entries}))
        got = gamefiles.extract_fonts(self.layout, self.tmp / 'game')
        self.assertEqual(got['title'][1], 'DejaVu Sans')
        self.assertEqual(Path(got['title'][0]).read_bytes(), otf)
        self.assertEqual(gamefiles.cached_fonts(self.layout)['title'][1], 'DejaVu Sans')


# ---------------------------------------------------------------- make_game.py

def nso(build_id=b'\0' * 16, size=0x300):
    head = bytearray(0x100)
    head[0:4] = b'NSO0'
    segs = [(0x100 + i * size, i * 0x10000, size) for i in range(3)]
    for i, (fo, mo, sz) in enumerate(segs):
        struct.pack_into('<III', head, 0x10 + i * 0x10, fo, mo, sz)
        struct.pack_into('<I', head, 0x60 + i * 4, sz)
    head[0x40:0x50] = build_id
    return bytes(head) + b''.join(bytes([i + 1]) * size for i in range(3))


def write_nup(path, exefs, romfs, title_id=0x0400486024DEC000):
    """A .nup (tools/nup.py's format) with one base package: exefs {name: bytes}, romfs {path: bytes}."""
    import nup as nupmod
    pool = bytearray()

    def name(s):
        off = len(pool)
        pool.extend(s.encode())
        return off, len(s.encode())

    dirs, files = [], []

    def add_tree(tree):
        """tree: {name: bytes | dict}; returns the directory index."""
        idx = len(dirs)
        dirs.append(None)
        kids = []
        for n, v in sorted(tree.items()):
            if isinstance(v, dict):
                kids.append(('d', add_tree(v), n))
            else:
                files.append([n, v])
                kids.append(('f', len(files) - 1, n))
        dirs[idx] = kids
        return idx

    root = add_tree({'exefs': exefs, 'romfs': _nest(romfs)})
    nd = len(dirs)
    dir_rows, child_list = [], []
    for kids in dirs:
        first = len(child_list)
        for kind, i, _n in kids:
            child_list.append(i if kind == 'd' else nd + i)
        dir_rows.append((first, len(kids)))
    dir_names = {root: ''}
    for kids in dirs:
        for kind, i, n in kids:
            if kind == 'd':
                dir_names[i] = n
    exefs_dir = next(i for k, i, n in dirs[root] if k == 'd' and n == 'exefs')
    romfs_dir = next(i for k, i, n in dirs[root] if k == 'd' and n == 'romfs')
    toc_head = 0x30 + nupmod.PACKAGE.size + nd * nupmod.DIRECTORY.size + len(files) * nupmod.FILE.size + \
        4 * len(child_list)
    blob_off = nupmod.SUPER.size
    data = bytearray()
    file_rows = []
    for n, v in files:
        no, nl = name(n)
        file_rows.append((no, nl, len(v), blob_off + len(data), len(v), hashlib.sha256(v).digest(), 0))
        data.extend(v)
    dir_name_rows = [name(dir_names[i]) for i in range(nd)]
    toc_off = blob_off + len(data)
    toc = bytearray(struct.pack('<IIQQQQQ', 0x01434F54, 1, nd, len(files), toc_head, len(pool), 0))
    toc += nupmod.PACKAGE.pack(title_id, root, exefs_dir, romfs_dir, 0, 0, 0, 0, 0, 0, 0, b'\0' * 20)
    for i in range(nd):
        toc += nupmod.DIRECTORY.pack(*dir_name_rows[i], *dir_rows[i], 0)
    for no, nl, size, off, stored, digest, codec in file_rows:
        toc += nupmod.FILE.pack(no, nl, size, off, stored, digest, codec, 0, 0, b'\0' * 12)
    toc += struct.pack(f'<{len(child_list)}I', *child_list)
    toc += pool
    total = toc_off + len(toc)
    sup = nupmod.SUPER.pack(b'NUP\x01', 0x80, 1, toc_off, len(toc), hashlib.sha256(bytes(toc)).digest(), blob_off,
                            total, 1, 0, b'\0' * 16, b'\0' * 32)
    Path(path).write_bytes(sup + bytes(data) + bytes(toc))


def _nest(flat):
    tree = {}
    for p, v in flat.items():
        parts = p.split('/')
        node = tree
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = v
    return tree


# Stands in for windres.exe: records its arguments, working directory and temporary folder, opens the .rc and the
# files it names from the working directory as windres does, and leaves two temporary files behind as windres does.
FAKE_WINDRES = r'''
import json, os, re, sys
from pathlib import Path
a = sys.argv[1:]
tmp = os.environ['TMP']
for n in ('cc1', 'cc2'):
    open(os.path.join(tmp, n), 'w').close()
Path(__file__).with_suffix('.json').write_text(json.dumps(
    {'argv': a, 'cwd': os.getcwd(), 'tmp': tmp, 'temp': os.environ['TEMP']}))
if os.environ.get('FAKE_WINDRES_FAIL'):
    sys.exit('windres: preprocessing failed.')
text = open(a[a.index('-i') + 1]).read()
for name in re.findall(r'^\s*\w+\s+\w+\s+"([^"]+)"', text, re.M):
    open(name, 'rb').close()
open(a[a.index('-o') + 1], 'wb').write(b'coff')
'''


class MakeGameTest(Tmp):
    def setUp(self):
        super().setUp()
        import make_game
        self.mg = make_game
        exefs = {'main': nso(bytes.fromhex(make_game.MAIN_BUILD_ID)), 'sdk': nso(), 'subsdk0': nso(), 'rtld': nso(),
                 'main.npdm': b'META' + b'\0' * 0x100}
        self.romfs = {'a.txt': b'hello', 'dir/b.bin': os.urandom(5000), 'dir/sub/c': b'c' * 33}
        self.nup = self.tmp / 'game.nup'
        write_nup(self.nup, exefs, self.romfs)
        self.msys = self.tmp / 'msys'
        for need in ('bin/clang.exe', 'bin/clang++.exe', 'bin/SDL3.dll', 'lib/libSDL3.dll.a'):
            (self.msys / need).parent.mkdir(parents=True, exist_ok=True)
            (self.msys / need).write_bytes(b'')
        (self.msys / 'include' / 'vma').mkdir(parents=True)
        self.layout.pydeps.mkdir(parents=True, exist_ok=True)
        (self.layout.pydeps / 'zbic.py').write_text('def decompress(b):\n    return b\n')
        self.out = self.tmp / 'game out'

    def run_mg(self, *args):
        events, lines = [], []
        run = build.Run(build.command(self.layout, list(args)), events.append, lines.append)
        code = run.wait()
        return code, events, lines

    def test_pipeline_rebuild_and_keys(self):
        code, ev, lines = self.run_mg('--nup', self.nup, '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual(code, 0, '\n'.join(lines))
        states = [(e['step'], e['state']) for e in ev if e['ev'] == 'step']
        self.assertEqual(states, [('check', 'start'), ('check', 'done'), ('exefs', 'start'), ('exefs', 'done'),
                                  ('romfs', 'start'), ('romfs', 'done')])
        self.assertTrue(any(e['ev'] == 'progress' and e['step'] == 'romfs' and e['done'] == e['total'] for e in ev))
        index = json.loads((self.out / 'romfs' / 'index.json').read_text())
        blob = (self.out / 'romfs' / 'romfs.bin').read_bytes()
        for e in index['files']:
            self.assertEqual(blob[e['offset']:e['offset'] + e['size']], self.romfs[e['path']])
            self.assertEqual(e['offset'] % 16, 0)
        flat = (self.out / 'exefs' / 'main').read_bytes()
        self.assertEqual(flat[0x40:0x50].hex(), self.mg.MAIN_BUILD_ID)
        # a second run with the same input skips both steps
        code, ev, _ = self.run_mg('--nup', self.nup, '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual([e['state'] for e in ev if e['ev'] == 'step' and e['step'] in ('exefs', 'romfs')],
                         ['skip', 'skip'])
        # --rebuild needs no game files: it checks the unpacked ExeFS and keeps both steps
        self.nup.rename(self.tmp / 'moved.nup')
        code, ev, lines = self.run_mg('--rebuild', '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual(code, 0, '\n'.join(lines))
        self.assertEqual([e['state'] for e in ev if e['ev'] == 'step' and e['step'] in ('exefs', 'romfs')],
                         ['skip', 'skip'])
        # the exefs and romfs keys do not depend on the kit; the lift key does
        st = json.loads((self.out / 'build-cache' / 'state.json').read_text())
        self.assertEqual(st['steps']['exefs'], self.mg.step_key('exefs', st['input']))
        self.assertNotIn(self.mg.kit_version(), st['steps']['romfs'])

    def test_rebuild_without_build(self):
        code, ev, lines = self.run_mg('--rebuild', '--out', self.tmp / 'empty', '--msys', self.msys)
        self.assertEqual(code, 1)
        self.assertTrue(any(e['ev'] == 'error' and 'rebuild' in e['message'] for e in ev))

    def test_wrong_version_refused(self):
        bad = self.tmp / 'bad.nup'
        write_nup(bad, {'main': nso(b'\x11' * 16), 'sdk': nso(), 'subsdk0': nso(), 'rtld': nso(),
                        'main.npdm': b'META' + b'\0' * 0x100}, {'a': b'a'})
        code, ev, _ = self.run_mg('--nup', bad, '--check')
        self.assertEqual(code, 1)
        self.assertTrue(any(e['ev'] == 'error' and '1.1.1' in e['message'] for e in ev))

    @unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
    def test_object_cache_survives_a_moved_kit(self):
        """The launcher keeps each version in its own folder: a kit at a new place reuses every object."""
        kits = []
        for name in ('0.3.10', '0.3.11'):
            kit = self.tmp / 'app' / name / 'runtime'
            kit.mkdir(parents=True)
            (kit / 'h.h').write_text('#define V 1\n')
            (kit / 'a.c').write_text('#include "h.h"\nint a(void) { return V; }\n')
            kits.append(kit.parent)
        obj = self.tmp / 'obj'
        obj.mkdir()

        def go(kit):
            rt = kit / 'runtime'
            srcs = [(rt / 'a.c', obj / 'rt_a.o')]
            cache = self.mg.ObjectCache(obj / 'objects.json', {'<kit>': kit})
            return self.mg.compile_all(srcs, lambda s_, d: ['cc', '-c', '-I', str(rt), '-o', str(d), str(s_)], cache,
                                       1, dict(os.environ), salt='cc')
        self.assertEqual(go(kits[0])[:2], (1, 0))
        self.assertEqual(go(kits[1])[:2], (0, 1))
        (kits[1] / 'runtime' / 'h.h').write_text('#define V 2\n')        # the new version changed the header
        self.assertEqual(go(kits[1])[:2], (1, 0))

    def main_with(self, romfs, lift, compile_):
        """make_game.main() in this process for a first build, with the given step functions in place of the real
        lift and compile (they need real modules and clang). Returns the SystemExit code, or 0."""
        sys.path.insert(0, str(self.layout.pydeps))   # the fake zbic
        saved = (self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile, sys.argv)
        self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile = romfs, lift, compile_
        sys.argv = ['make_game.py', '--nup', str(self.nup), '--out', str(self.out), '--msys', str(self.msys),
                    '--until', 'compile']   # the shaders step needs the shader translator
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.mg.main()
            return 0
        except SystemExit as e:
            return e.code
        finally:
            self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile, sys.argv = saved
            sys.path.remove(str(self.layout.pydeps))

    def test_romfs_beside_lift_and_compile(self):
        """A first build packs the RomFS image while the lift and the compile run; the compile gets the running step
        and waits for it only for the RomFS index."""
        real_romfs, seen = self.mg.step_romfs, {}

        def romfs(src, out, jobs):
            time.sleep(0.5)
            real_romfs(src, out, jobs)

        def lift(out, cache):
            seen['lift_beside_romfs'] = not (out / 'romfs' / 'index.json').exists()

        def compile_(out, cache, msys, jobs, romfs_job=None):
            seen['job'] = romfs_job is not None
            romfs_job.join()
            seen['image_after_join'] = (out / 'romfs' / 'romfs.bin').is_file()
            (out / self.mg.EXE_NAME).write_bytes(b'MZ')
        self.assertEqual(self.main_with(romfs, lift, compile_), 0)
        self.assertEqual(seen, {'lift_beside_romfs': True, 'job': True, 'image_after_join': True})
        steps = json.loads((self.out / 'build-cache' / 'state.json').read_text())['steps']
        self.assertEqual(set(steps), {'exefs', 'romfs', 'lift', 'compile'})

    def test_romfs_failure_stops_the_build(self):
        ran, lifting, failed = [], threading.Event(), threading.Event()

        def romfs(src, out, jobs):
            lifting.wait(5)
            failed.set()
            self.mg.fail('the RomFS image could not be written')

        def lift(out, cache):
            lifting.set()
            failed.wait(5)    # the RomFS step fails while the lift runs
            time.sleep(0.1)
            ran.append('lift')

        def compile_(out, cache, msys, jobs, romfs_job=None):
            ran.append('compile')
        self.assertEqual(self.main_with(romfs, lift, compile_), 1)
        self.assertEqual(ran, ['lift'])
        steps = json.loads((self.out / 'build-cache' / 'state.json').read_text())['steps']
        self.assertNotIn('romfs', steps)

    @unittest.skipIf(os.name == 'nt', 'uses a POSIX script as the compiler')
    def test_largest_source_compiles_first(self):
        src, obj = self.tmp / 'src', self.tmp / 'obj'
        src.mkdir()
        obj.mkdir()
        for name, size in (('small.c', 10), ('big.c', 5000), ('mid.c', 800)):
            (src / name).write_text('/*' + 'x' * size + '*/\n')
        order = self.tmp / 'order.txt'
        cc = self.tmp / 'fakecc'
        cc.write_text(f'#!{sys.executable}\nimport sys\nargs = sys.argv[1:]\n'
                      'dep, out, s = args[args.index("-MF") + 1], args[args.index("-o") + 1], args[-1]\n'
                      f'open({str(order)!r}, "a").write(s.rsplit("/", 1)[-1] + "\\n")\n'
                      'open(out, "w").close(); open(dep, "w").write(out + ": " + s + "\\n")\n')
        cc.chmod(0o755)
        srcs = [(src / n, obj / (n[:-2] + '.o')) for n in ('small.c', 'big.c', 'mid.c')]
        cache = self.mg.ObjectCache(obj / 'objects.json')
        compiled, reused, bad = self.mg.compile_all(srcs, lambda s, d: [str(cc), '-c', '-o', str(d), str(s)], cache,
                                                    1, dict(os.environ))
        self.assertEqual((compiled, bad), (3, []))
        self.assertEqual(order.read_text().split(), ['big.c', 'mid.c', 'small.c'])

    def test_runtime_split(self):
        tainted = self.mg.per_install_names()
        self.assertTrue({'layout.h', 'patches.h', 'romfs_config.h'} <= tainted)
        names = {p.name: self.mg.per_install(p, tainted) for p, _ in self.mg.runtime_sources()}
        for f in ('loader.c', 'memory.c', 'romfs.c', 'patches.c'):
            self.assertTrue(names[f], f)
        self.assertFalse(names['vk_device.c'])
        self.assertLess(sum(names.values()), len(names) // 4)          # most of the runtime can come prebuilt

    def test_parse_deps(self):
        text = 'C:/o/x.o: C:/src/x.c C:/inc/a\\ b.h \\\n  C:/inc/c.h\n'
        self.assertEqual(self.mg.parse_deps(text), ['C:/src/x.c', 'C:/inc/a b.h', 'C:/inc/c.h'])

    @unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
    def test_object_cache(self):
        src = self.tmp / 'src'
        src.mkdir()
        (src / 'h.h').write_text('#define V 1\n')
        (src / 'a.c').write_text('#include "h.h"\nint a(void) { return V; }\n')
        (src / 'b.c').write_text('int b(void) { return 2; }\n')
        obj = self.tmp / 'obj'
        obj.mkdir()
        srcs = [(src / 'a.c', obj / 'a.o'), (src / 'b.c', obj / 'b.o')]

        def cmd_for(s, d):
            return ['cc', '-c', '-I', str(src), '-o', str(d), str(s)]

        def go():
            cache = self.mg.ObjectCache(obj / 'objects.json')
            return self.mg.compile_all(srcs, cmd_for, cache, 2, dict(os.environ), salt='cc')
        self.assertEqual(go()[:2], (2, 0))
        self.assertEqual(go()[:2], (0, 2))
        (src / 'h.h').write_text('#define V 3\n')                         # a header: only a.c includes it
        self.assertEqual(go()[:2], (1, 1))
        (src / 'b.c').write_text('int b(void) { return syntax error }\n')
        compiled, reused, bad = go()
        self.assertEqual((compiled, len(bad)), (1, 1))
        self.assertNotIn('b.o', json.loads((obj / 'objects.json').read_text()))

    @unittest.skipIf(os.name == 'nt', 'a script stands in for windres.exe')
    def test_compile_resources(self):
        # The toolchain, the kit and the game under folders with spaces, as under C:\Users\Ali Sayed. windres must
        # start gcc itself (--use-temp-file) on a line that names no path but gcc's: the .rc by its bare name from
        # runtime/, no -I, -D or -U.
        msys, obj = self.tmp / 'tool chain' / 'ucrt64', self.out / 'build-cache' / 'obj'
        windres = msys / 'bin' / 'windres.exe'
        windres.parent.mkdir(parents=True)
        windres.write_text(f'#!{sys.executable}\n' + FAKE_WINDRES)
        windres.chmod(0o755)
        obj.mkdir(parents=True)
        rt = self.tmp / 'the kit' / 'runtime'
        rt.mkdir(parents=True)
        for name in ('nx2navi.rc', 'nx2navi.manifest', 'nx2navi.ico'):
            shutil.copy2(ROOT / 'runtime' / name, rt / name)
        rco = self.mg.compile_resources(msys, rt, obj, dict(os.environ))
        self.assertEqual((rco, rco.read_bytes()), (obj / 'nx2navi_rc.o', b'coff'))
        seen = json.loads(windres.with_suffix('.json').read_text())
        args = seen['argv']
        self.assertEqual(seen['cwd'], str(rt))
        self.assertIn('--use-temp-file', args)
        self.assertIn(f'--preprocessor={msys / "bin" / "gcc.exe"}', args)
        self.assertEqual([a for a in args if a.startswith('--preprocessor-arg=')],
                         ['--preprocessor-arg=-E', '--preprocessor-arg=-xc', '--preprocessor-arg=-DRC_INVOKED'])
        self.assertEqual(args[args.index('-i') + 1], 'nx2navi.rc')
        self.assertFalse([a for a in args if a.startswith(('-I', '--include-dir', '-D', '-U'))])
        # the temporary files went to a folder of the build cache, which is gone with the two windres left there
        self.assertEqual(seen['tmp'], seen['temp'])
        self.assertEqual(Path(seen['tmp']).parent, obj)
        self.assertEqual(sorted(p.name for p in obj.iterdir()), ['nx2navi_rc.o'])
        # the same resource again: the object keeps its time, so the link key does not change and nothing relinks
        before = rco.stat().st_mtime_ns
        time.sleep(0.02)
        self.mg.compile_resources(msys, rt, obj, dict(os.environ))
        self.assertEqual(rco.stat().st_mtime_ns, before)
        # a failing windres stops the build with its message
        err = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(err):
            self.mg.compile_resources(msys, rt, obj, dict(os.environ, FAKE_WINDRES_FAIL='1'))
        self.assertIn('windres failed on runtime/nx2navi.rc: windres: preprocessing failed.', err.getvalue())
        self.assertEqual(sorted(p.name for p in obj.iterdir()), ['nx2navi_rc.o'])


# ---------------------------------------------------------------- jobs

FAKE_MAKE_GAME = r'''
import json, sys, time
def ev(**k): print('@@' + json.dumps(k), flush=True)
if '--fail' in open(sys.argv[0] + '.mode').read():
    ev(ev='step', step='check', state='start'); ev(ev='error', message='this is not game version 1.1.1'); sys.exit(1)
for s in ('check', 'exefs', 'romfs', 'lift', 'compile', 'shaders'):
    ev(ev='step', step=s, state='start')
    if s == 'compile':
        ev(ev='progress', step=s, done=5, total=10, unit='files'); ev(ev='phase', step=s, phase='link')
    print('working on', s, flush=True)
    ev(ev='step', step=s, state='done', seconds=0.1)
ev(ev='end', exe='x')
'''


class JobTest(Tmp):
    def make_ctx(self, mode):
        app = self.tmp / 'app'
        (app / 'tools').mkdir(parents=True)
        (app / 'VERSION').write_text('0.3.10\n')
        script = app / 'tools' / 'make_game.py'
        script.write_text(FAKE_MAKE_GAME)
        (app / 'tools' / 'make_game.py.mode').write_text(mode)
        ctx = Context(Layout(root=self.tmp / 'root', app=app))
        return ctx

    def patch_tools(self):
        saved = toolchain.load_lock, toolchain.is_current, toolchain.ensure_zbic
        toolchain.load_lock = lambda layout, version='dev': {'packages': []}
        toolchain.is_current = lambda layout, lock: True
        toolchain.ensure_zbic = lambda *a, **k: None
        self.addCleanup(lambda: setattr(toolchain, 'load_lock', saved[0]) or
                        setattr(toolchain, 'is_current', saved[1]) or setattr(toolchain, 'ensure_zbic', saved[2]))

    def test_install_job(self):
        self.patch_tools()
        ctx = self.make_ctx('ok')
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'ok', j.error)
        self.assertEqual([s.state for s in j.steps], [jobs.SKIPPED] + [jobs.DONE] * 7)   # tools ... shaders, ready
        self.assertEqual(j.step('check').value, 'Version 1.1.1')
        self.assertTrue(any('working on lift' in ln for ln in j.lines))
        self.assertEqual(j.overall(), 1.0)

    def test_failed_job(self):
        self.patch_tools()
        ctx = self.make_ctx('--fail')
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'failed')
        self.assertIn('1.1.1', j.error)
        self.assertEqual(j.step('check').state, jobs.FAILED)


# ---------------------------------------------------------------- the icon

class SavesTest(unittest.TestCase):
    """launcher/saves.py: where the game keeps its save, the save folders it recognises, and the import."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-saves-'))
        self.game = self.tmp / 'game'
        self.game.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def files(self, folder, names):
        for n, data in names.items():
            (folder / n).parent.mkdir(parents=True, exist_ok=True)
            (folder / n).write_bytes(data)
        return folder

    def test_account_folder_as_the_game_names_it(self):
        (self.game / 'profile').mkdir()
        lo, hi = 0x1122334455667788, 0x99aabbccddeeff00
        (self.game / 'profile' / 'account.uid').write_bytes(lo.to_bytes(8, 'little') + hi.to_bytes(8, 'little'))
        self.assertEqual(saves.account_dir(self.game).name, f'{hi:016x}{lo:016x}')   # fs_host.c storage_init
        fresh = self.tmp / 'fresh'
        acct = saves.account_dir(fresh)                       # a game that never ran: the account is made
        self.assertEqual(len((fresh / 'profile' / 'account.uid').read_bytes()), 16)
        self.assertEqual(saves.account_dir(fresh), acct)

    def test_save_folders_found(self):
        plain = self.files(self.tmp / 'backup', {'save.dat': b'1', 'option.dat': b'2'})
        self.assertEqual(saves.find_save(plain)[0], plain)
        emu = self.files(self.tmp / 'emu', {'0000000000000000/' + 'a' * 32 + '/0100000000010000/save.dat': b'1'})
        self.assertEqual(saves.find_save(emu)[0].name, '0100000000010000')
        other = self.files(self.tmp / 'other', {'profile/save/' + 'b' * 32 + '/save.dat': b'1', 'ocarina.exe': b''})
        self.assertEqual(saves.find_save(other)[0].name, 'b' * 32)
        self.assertEqual(saves.find_save(other / 'profile')[0].name, 'b' * 32)
        (self.tmp / 'empty').mkdir()
        self.assertIsNone(saves.find_save(self.tmp / 'empty')[0])

    def test_import_backs_up_and_clears_the_journal(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        tx = self.game / 'profile' / 'save-tx'
        self.files(tx, {'ready-3/save.dat': b'newer', 'work/save.dat': b'w'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported', 'sub/extra.bin': b'x'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertEqual(r['files'], 2)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'imported')
        self.assertEqual((acct / 'sub' / 'extra.bin').read_bytes(), b'x')
        self.assertEqual(list(tx.iterdir()), [])                       # the game loads the imported save
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'newer')   # the save the game would have loaded
        self.assertTrue((self.game / 'saves' / 'README.txt').is_file())
        self.assertTrue((src / 'save.dat').is_file())                  # a picked folder is left as it was

    def test_import_from_the_saves_folder(self):
        d = saves.ensure_saves_dir(self.game)
        self.assertIsNone(saves.waiting(self.game)[0])                 # README.txt alone is no save
        self.files(d, {'emu/0000000000000000/' + 'c' * 32 + '/0100000000010000/save.dat': b'emu'})
        self.assertIsNotNone(saves.waiting(self.game)[0])
        r = saves.import_save(self.game, log=lambda m: None)
        self.assertIsNone(r['backup'])                                 # there was no save yet
        self.assertEqual((saves.account_dir(self.game) / 'save.dat').read_bytes(), b'emu')
        left = sorted(e.name for e in d.iterdir())
        self.assertEqual(left[0], 'README.txt')
        self.assertTrue(left[1].startswith('imported '))
        self.assertIsNone(saves.waiting(self.game)[0])                 # not imported twice
        self.files(d, {'save.dat': b'loose'})                          # files put straight into saves/
        r = saves.import_save(self.game, log=lambda m: None)
        self.assertEqual((saves.account_dir(self.game) / 'save.dat').read_bytes(), b'loose')
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'emu')
        self.assertIsNone(saves.waiting(self.game)[0])

    def test_picking_the_saves_folder_takes_what_waits_there(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'mine'})
        d = saves.ensure_saves_dir(self.game)
        with self.assertRaises(saves.SaveError):                      # README.txt alone is no save
            saves.import_save(self.game, d, log=lambda m: None)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'mine')
        self.files(d, {'save.dat': b'put there'})
        r = saves.import_save(self.game, d, log=lambda m: None)
        self.assertEqual(sorted(p.name for p in acct.iterdir()), ['save.dat'])   # not README.txt or a backup
        self.assertEqual((acct / 'save.dat').read_bytes(), b'put there')
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'mine')

    def test_an_interrupted_commit_is_backed_up(self):
        # runtime/fs_host.c save_recover: with no account folder and no finished commit, save-tx/new is the save, and
        # without it save-tx/old; the import keeps that only copy
        for half in ('new', 'old'):
            with self.subTest(half=half):
                shutil.rmtree(self.game, ignore_errors=True)
                self.game.mkdir()
                acct = saves.account_dir(self.game)
                tx = self.game / 'profile' / 'save-tx'
                self.files(tx, {f'{half}/save.dat': b'only copy'})
                src = self.files(self.tmp / f'mine-{half}', {'save.dat': b'imported'})
                r = saves.import_save(self.game, src, log=lambda m: None)
                self.assertEqual((acct / 'save.dat').read_bytes(), b'imported')
                self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'only copy')
                self.assertEqual((r['backup'] / 'save-tx' / half / 'save.dat').read_bytes(), b'only copy')

    def test_a_failed_swap_keeps_the_old_save(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        real = saves.os.replace

        def replace(a, b):
            if Path(a).name == 'import':   # the imported save going into place: antivirus holds a file
                raise PermissionError(5, 'Access is denied')
            return real(a, b)
        saves.os.replace = replace
        try:
            with self.assertRaises(PermissionError):
                saves.import_save(self.game, src, log=lambda m: None)
        finally:
            saves.os.replace = real
        self.assertEqual((acct / 'save.dat').read_bytes(), b'old')

    def test_a_failed_swap_keeps_the_finished_commit(self):
        # the account folder cannot be moved aside (a file in it held open): the newer finished commit stays in
        # save-tx, where the game loads it first, instead of going into the backup
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'older'})
        tx = self.game / 'profile' / 'save-tx'
        self.files(tx, {'ready-4/save.dat': b'newest'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        real = saves.os.replace

        def replace(a, b):
            if Path(a) == acct:
                raise PermissionError(32, 'The process cannot access the file')
            return real(a, b)
        saves.os.replace = replace
        try:
            with self.assertRaises(PermissionError):
                saves.import_save(self.game, src, log=lambda m: None)
        finally:
            saves.os.replace = real
        self.assertEqual((tx / 'ready-4' / 'save.dat').read_bytes(), b'newest')
        self.assertEqual((acct / 'save.dat').read_bytes(), b'older')
        self.assertFalse((tx / 'import').exists())

    @unittest.skipUnless(os.name == 'nt', 'the running check is Windows only')
    def test_a_read_only_exe_is_not_a_running_game(self):
        exe = self.game / 'ocarina.exe'
        exe.write_bytes(b'MZ')
        os.chmod(exe, stat.S_IREAD)
        self.assertFalse(build.game_running(self.game))
        self.assertFalse(saves.game_running(self.game))
        self.assertTrue(os.access(exe, os.W_OK))

    def test_read_only_files_import_writable(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        os.chmod(src / 'save.dat', stat.S_IREAD)
        os.chmod(acct / 'save.dat', stat.S_IREAD)   # the save it replaces: deleted after the swap all the same
        try:
            saves.import_save(self.game, src, log=lambda m: None)
            self.assertTrue(os.access(acct / 'save.dat', os.W_OK))
            self.assertEqual([p.name for p in (self.game / 'profile' / 'save-tx').iterdir()], [])
        finally:
            os.chmod(src / 'save.dat', stat.S_IREAD | stat.S_IWRITE)

    def test_another_installs_finished_commit_is_its_save(self):
        other = self.files(self.tmp / 'other', {'profile/save/' + 'b' * 32 + '/save.dat': b'older',
                                                'profile/save-tx/ready-2/save.dat': b'newest'})
        self.assertEqual(saves.find_save(other)[0].name, 'ready-2')

    def test_refusals(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'1'})
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, self.game / 'profile', log=lambda m: None)   # its own save
        big = self.tmp / 'big'
        big.mkdir()
        for i in range(saves.MAX_FILES + 1):
            (big / f'{i}.txt').write_bytes(b'')
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, big, log=lambda m: None)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'1')      # nothing changed


class IconTest(unittest.TestCase):
    def test_navi(self):
        try:
            from launcher.ui import art
        except ImportError:
            self.skipTest('needs numpy')
        self.assertEqual(art.navi(2).shape, (42, 44, 4))
        data = art.navi_ico()
        count = struct.unpack_from('<H', data, 4)[0]
        sizes = [data[6 + 16 * i] or 256 for i in range(count)]
        self.assertEqual(sizes[0], 16)
        self.assertIn(256, sizes)
        off = struct.unpack_from('<I', data, 6 + 12)[0]
        self.assertEqual(struct.unpack_from('<Iii', data, off), (40, 16, 32))   # a bitmap entry, not PNG
        big = struct.unpack_from('<I', data, 6 + 16 * (count - 1) + 12)[0]
        self.assertEqual(data[big:big + 4], b'\x89PNG')


# ---------------------------------------------------------------- the window

def have_display():
    if os.name == 'nt':
        return True
    if not os.environ.get('DISPLAY'):
        return False
    try:
        import tkinter  # noqa: F401
        import numpy  # noqa: F401
        return True
    except ImportError:
        return False


@unittest.skipUnless(have_display(), 'needs tkinter, numpy and a display')
class WindowTest(Tmp):
    def test_selftest(self):
        r = subprocess.run([sys.executable, '-m', 'launcher', '--root', str(self.tmp / 'w'), '--selftest'],
                           cwd=ROOT, capture_output=True, text=True, timeout=180)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('9 screens drawn', r.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
