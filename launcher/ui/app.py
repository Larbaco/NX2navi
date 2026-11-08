"""The launcher's window, laid out like the game's options menu (docs/launcher.md, "Look"): a wooden title band with
Navi and the name in the game's Cetra Display across the whole top (the window's own title bar: drag it to move the
window, minimise and close at its right end), a list of rows on the left (a bronze label cell and a dark value cell each), a white
glowing cursor on the selected row, filigree dividers between groups and a description box on the right that
explains the selected row.

The window is one Tk canvas at 1000 x 640 logical pixels, scaled to the display. The pictures come from art.py; text,
flat cells and the orange choices are canvas items. Screens:

    first_run   the dump, the install folder, three choices and Begin Installation
    home        Play, Updates, the game folder, Rebuild the Game, Settings
    job         the steps of an install, update or rebuild, with their progress, and Cancel; after a failure Try Again
                and Copy Report
    settings    the launcher's choices, the folders, the logs and Uninstall
    uninstall   what to remove, then Uninstall or Cancel

Mouse, keyboard (arrows, Enter or Space, Esc, F5) and an XInput gamepad (d-pad or stick, A, B, Y) all move the cursor.
"""
import argparse
import os
import shutil
import sys
import time
import tkinter as tk
import tkinter.font as tkfont
from pathlib import Path
from tkinter import filedialog

from .. import NAME, build, feed, gamefiles, jobs, saves, versions, winsys
from ..context import Context
from . import art

W, H = 1000, 640
BAND = (0, 1000, 56)            # x0, x1, height: the whole top of the window, which is also its title bar
TITLE_BUTTONS = (('min', 908, 954), ('close', 954, 1000))   # x ranges on the band
PANEL = (112, 60, 580)          # x0, top, x1
ROWS = (140, 552, 96)           # x0, x1, top
LABEL_W = 222                   # 54 % of the row
GAP = 12
DESC = (614, 336)               # x, width
P = art.PALETTE


class Row:
    def __init__(self, kind, label='', value='', style='normal', big=False, icon=None, badge=False, desc=None,
                 on=None, left=None, right=None, get=None, options=None, fraction=0.0, actions=None, check=False,
                 focus_value=False, height=None, enabled=True):
        self.kind, self.label, self.value, self.style = kind, label, value, style
        self.big, self.icon, self.badge, self.desc, self.check = big, icon, badge, desc, check
        self.on, self.left, self.right, self.get, self.options = on, left, right, get, options
        self.fraction, self.actions, self.focus_value, self.enabled = fraction, actions or [], focus_value, enabled
        default_h = {'divider': 26, 'action': 54, 'pair': 44}.get(kind, 64 if big else 40)
        self.h = height or default_h
        self.rect = self.value_rect = None
        self.parts = []              # pair: [(rect, label, callback)]

    @property
    def focusable(self):
        return self.kind != 'divider'


def fit(font, text, width):
    """text shortened in the middle with an ellipsis so it fits width pixels."""
    if font.measure(text) <= width:
        return text
    lo, hi = 1, len(text)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        keep = mid // 2
        cand = text[:keep] + '…' + text[len(text) - (mid - keep):]
        if font.measure(cand) <= width:
            lo = mid
        else:
            hi = mid - 1
    keep = lo // 2
    return text[:keep] + '…' + text[len(text) - (lo - keep):]


def free_gb(path):
    p = Path(path)
    while not p.exists() and p.parent != p:
        p = p.parent
    try:
        return shutil.disk_usage(p).free / 1e9
    except OSError:
        return 0.0


def drive_of(path):
    return Path(path).anchor.rstrip('\\/') or str(Path(path).parent)


class App:
    def __init__(self, root, ctx, args):
        self.root, self.ctx, self.args = root, ctx, args
        self.layout, self.settings = ctx.layout, ctx.settings
        self.s = self.compute_scale()
        self.sprites = {}
        self.photos = {}
        self.rows, self.focus = [], 0
        self.screen = None
        self.job = None
        self.job_rev = -1
        self.job_return = 'home'
        self.check = None
        self.release = None
        self.check_error = ''
        self.checked_at = 0.0
        self.message = ''
        self.save_message = ''   # the last Import Saves result, shown in its description
        self.dumps = None
        self.dump_search = None
        self.uninstall_game = False
        self.game_proc = None
        self.cursor_frames, self.cursor_k = [], 0
        self.last_second = 0
        self.gamepad = winsys.Gamepad()
        self.tb_hover = None
        self.drag = None
        self.ico = None

        root.title(NAME)
        root.resizable(False, False)
        root.configure(background=P['window'])
        self.canvas = tk.Canvas(root, width=self.X(W), height=self.X(H), highlightthickness=0, bd=0,
                                background=P['window'], cursor='arrow')
        self.canvas.pack()
        self.set_icon()
        self.load_fonts()
        self.draw_static()
        self.bind_input()
        self.start()
        self.root.after(80, self.animate)
        self.root.after(150, self.tick)
        self.root.after(50, self.poll_gamepad)
        self.center()
        self.framed = not winsys.frameless(root)   # Windows: the band is the title bar
        self.root.after(60, self.native_window)

    # ---------------------------------------------------------------- scale, icon, fonts

    def compute_scale(self):
        try:
            s = self.root.winfo_fpixels('1i') / 96.0
        except tk.TclError:
            s = 1.0
        sw, sh = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
        s = min(max(s, 1.0), sw * 0.94 / W, sh * 0.88 / H)
        return max(0.75, round(s * 4) / 4)

    def X(self, v):
        return int(round(v * self.s))

    def center(self):
        self.root.update_idletasks()
        w, h = self.X(W), self.X(H)
        x = max(0, (self.root.winfo_screenwidth() - w) // 2)
        y = max(0, (self.root.winfo_screenheight() - h) // 3)
        self.root.geometry(f'{w}x{h}+{x}+{y}')

    def set_icon(self):
        """Navi as the window's icon: Tk's photo icon everywhere, and on Windows the .ico set on the window itself, so
        the taskbar button shows Navi rather than Python's icon."""
        try:
            self.photos['icon'] = art.photo(self.root, art.navi_icon(64))
            self.root.iconphoto(True, self.photos['icon'])
            if os.name == 'nt':
                data = art.navi_ico()
                self.ico = self.layout.cache / 'nx2navi.ico'
                if not self.ico.is_file() or self.ico.read_bytes() != data:
                    self.ico.write_bytes(data)
        except (tk.TclError, OSError) as e:
            self.ctx.log(f'icon: {e}')

    def native_window(self):
        if self.ico:
            winsys.set_window_icon(self.root, self.ico)
        try:
            self.root.focus_force()
        except tk.TclError:
            pass

    def load_fonts(self):
        """The game's Cetra Display when this PC has decoded it from the player's game (data/cache/fonts), else the
        system's interface font."""
        fams = {}
        for role, (path, family) in gamefiles.cached_fonts(self.layout).items():
            if winsys.register_font(path):
                fams[role] = family
        available = set(tkfont.families(self.root))
        fallback = next((f for f in ('Segoe UI', 'Source Sans 3', 'Noto Sans', 'DejaVu Sans', 'Helvetica')
                         if f in available), 'TkDefaultFont')
        # Every text is in the game's Cetra Display; only the build log keeps a monospaced face so its lines align.
        text = title = fams.get('title') if fams.get('title') in available else fallback
        mono = next((f for f in ('Consolas', 'Cascadia Mono', 'DejaVu Sans Mono', 'Courier New') if f in available),
                    'TkFixedFont')
        self.game_fonts = bool(fams.get('title') and fams['title'] in available)
        self.ctx.log(f'fonts: {text}' + (' (the game\'s, from data/cache/fonts)' if self.game_fonts else
                                        ' (the game\'s Cetra Display is not decoded yet)'))

        self.fspec = {}

        def F(family, px, weight='normal'):
            font = tkfont.Font(root=self.root, family=family, size=-max(8, int(round(px * self.s))), weight=weight)
            self.fspec[id(font)] = (family, max(8, int(round(px * self.s))), weight)
            return font
        self.fcache = {}
        self.f = {'band': F(title, 25), 'big': F(text, 23), 'big_small': F(text, 21), 'label': F(text, 17),
                  'value': F(text, 16), 'desc': F(text, 17), 'small': F(text, 15), 'heading': F(title, 21),
                  'sub': F(title, 17), 'action': F(text, 18), 'status': F(text, 15), 'mono': F(mono, 11.5),
                  'badge': F(text, 15, 'bold'), 'action_big': F(text, 21)}

    def fit_font(self, font, text, width, smallest=0.72):
        """font, or the same face a few pixels smaller so text fits width (down to smallest of its size)."""
        if font.measure(text) <= width:
            return font
        family, px, weight = self.fspec[id(font)]
        for size in range(px - 1, max(7, int(px * smallest)) - 1, -1):
            key = (family, size, weight)
            if key not in self.fcache:
                self.fcache[key] = tkfont.Font(root=self.root, family=family, size=-size, weight=weight)
            if self.fcache[key].measure(text) <= width:
                return self.fcache[key]
        return self.fcache[(family, max(7, int(px * smallest)), weight)]

    def fonts_from_game(self, game_dir='', dump=''):
        """Decode the game's fonts in the background and redraw with them."""
        if self.game_fonts:
            return

        def work():
            try:
                got = gamefiles.extract_fonts(self.layout, game_dir, dump)
            except Exception as e:
                self.ctx.log(f'fonts: {e}')
                got = {}
            if got:
                self.root.after(0, self.reload_fonts)
        import threading
        threading.Thread(target=work, daemon=True).start()

    def reload_fonts(self):
        self.load_fonts()
        self.draw_static()
        self.render()

    # ---------------------------------------------------------------- pictures

    def sprite(self, key, make):
        if key not in self.sprites:
            self.sprites[key] = art.photo(self.root, make())
        return self.sprites[key]

    def draw_static(self):
        c, s = self.canvas, self.s
        c.delete('static')
        c.create_image(0, 0, anchor='nw', image=self.sprite('backdrop', lambda: art.backdrop(self.X(W), self.X(H), s)),
                       tags='static')
        bx0, bx1, bh = BAND
        px0, ptop, px1 = PANEL
        c.create_image(self.X(px0), self.X(ptop), anchor='nw', tags='static',
                       image=self.sprite('panel', lambda: art.panel(self.X(px1) - self.X(px0), self.X(H - ptop), s)))
        c.create_image(self.X(bx0), 0, anchor='nw', tags='static',
                       image=self.sprite('band', lambda: art.band(self.X(bx1) - self.X(bx0), self.X(bh), s)))
        k = max(1, int(round(2 * s)))   # Navi in whole pixels: two screen pixels per artwork pixel at 100 %
        emblem = self.sprite(('navi', k), lambda: art.navi(k))
        ew = emblem.width()
        title_w = self.f['band'].measure(NAME)
        gap = self.X(12)
        x = self.X(W / 2) - (ew + gap + title_w) // 2
        c.create_image(x, self.X(bh / 2), anchor='w', image=emblem, tags='static')
        tx = x + ew + gap
        c.create_text(tx + 1, self.X(bh / 2) + self.X(2), text=NAME, anchor='w', font=self.f['band'], fill='#1a120a',
                      tags='static')
        c.create_text(tx, self.X(bh / 2), text=NAME, anchor='w', font=self.f['band'], fill=P['band_text'],
                      tags='static')
        c.create_rectangle(0, 0, self.X(W) - 1, self.X(H) - 1, outline='#140e09', tags='static')
        c.tag_lower('static')
        self.draw_title_buttons()

    def draw_title_buttons(self):
        """Minimise and close at the band's right end, lit when the mouse is over them."""
        c, X = self.canvas, self.X
        c.delete('tb')
        bh = X(BAND[2])
        for kind, x0, x1 in TITLE_BUTTONS:
            if self.tb_hover == kind:
                c.create_rectangle(X(x0), 0, X(x1), bh - 1, outline='', tags='tb',
                                   fill=P['close_hover'] if kind == 'close' else P['button_hover'])
            size = X(12)
            glyph = self.sprite(('glyph', kind, size), lambda kd=kind: art.title_glyph(kd, size, self.s))
            c.create_image((X(x0) + X(x1)) // 2, bh // 2, image=glyph, tags='tb')

    def title_button_at(self, x, y):
        if y >= self.X(BAND[2]):
            return None
        for kind, x0, x1 in TITLE_BUTTONS:
            if self.X(x0) <= x < self.X(x1):
                return kind
        return None

    # ---------------------------------------------------------------- input

    def bind_input(self):
        r = self.root
        for key, fn in (('<Up>', lambda e: self.move(-1)), ('<Down>', lambda e: self.move(1)),
                        ('<Left>', lambda e: self.side(-1)), ('<Right>', lambda e: self.side(1)),
                        ('<Return>', lambda e: self.activate()), ('<KP_Enter>', lambda e: self.activate()),
                        ('<space>', lambda e: self.activate()), ('<Escape>', lambda e: self.back()),
                        ('<F5>', lambda e: self.check_now(force=True))):
            r.bind(key, fn)
        self.canvas.bind('<Motion>', self.on_motion)
        self.canvas.bind('<Button-1>', self.on_click)
        self.canvas.bind('<B1-Motion>', self.on_drag)
        self.canvas.bind('<ButtonRelease-1>', lambda e: setattr(self, 'drag', None))
        self.canvas.bind('<Leave>', lambda e: self.set_tb_hover(None))
        r.bind('<Alt-F4>', lambda e: self.on_close())
        r.protocol('WM_DELETE_WINDOW', self.on_close)

    def poll_gamepad(self):
        try:
            for b in self.gamepad.poll():
                {'up': lambda: self.move(-1), 'down': lambda: self.move(1), 'left': lambda: self.side(-1),
                 'right': lambda: self.side(1), 'a': self.activate, 'b': self.back,
                 'y': lambda: self.check_now(force=True)}.get(b, lambda: None)()
        except Exception as e:
            self.ctx.log(f'gamepad: {e}')
            self.gamepad.dll = None
        self.root.after(50, self.poll_gamepad)

    def focusables(self):
        return [i for i, r in enumerate(self.rows) if r.focusable]

    def move(self, d):
        idx = self.focusables()
        if not idx:
            return
        pos = idx.index(self.focus) if self.focus in idx else 0
        self.focus = idx[max(0, min(len(idx) - 1, pos + d))]
        self.render()

    def focused(self):
        return self.rows[self.focus] if 0 <= self.focus < len(self.rows) else None

    def side(self, d):
        r = self.focused()
        if not r:
            return
        if r.kind == 'toggle':
            r.on(d < 0)                     # left = On, right = Off
        elif r.kind == 'enum':
            (r.left if d < 0 else r.right)()
        elif r.kind == 'pair':
            r.pick = max(0, min(len(r.actions) - 1, getattr(r, 'pick', 0) + d))
        else:
            return
        self.render()

    def activate(self):
        r = self.focused()
        if not r or not r.enabled:
            return
        if r.kind == 'toggle':
            r.on(not r.get())
        elif r.kind == 'enum':
            r.right()
        elif r.kind == 'pair':
            r.actions[getattr(r, 'pick', 0)][1]()
            return
        elif r.on:
            r.on()
            return
        self.render()

    def back(self):
        if self.screen == 'settings':
            self.show('home')
        elif self.screen == 'uninstall':
            self.show('settings')
        elif self.screen == 'job' and self.job and self.job.result is None:
            self.cancel_job()
        elif self.screen == 'job':
            self.leave_job()

    def hit(self, x, y):
        for i, r in enumerate(self.rows):
            if r.rect and r.focusable:
                x0, y0, x1, y1 = r.rect
                if x0 <= x <= x1 and y0 - self.X(GAP / 2) <= y <= y1 + self.X(GAP / 2):
                    return i
        return None

    def set_tb_hover(self, kind):
        if kind != self.tb_hover:
            self.tb_hover = kind
            self.draw_title_buttons()

    def on_drag(self, e):
        if self.drag:   # moving the window without the system's help (Linux)
            dx, dy = self.drag
            self.root.geometry(f'+{e.x_root - dx}+{e.y_root - dy}')

    def on_motion(self, e):
        self.set_tb_hover(self.title_button_at(e.x, e.y))
        i = self.hit(e.x, e.y)
        if i is not None and i != self.focus:
            self.focus = i
            r = self.rows[i]
            if r.kind == 'pair':
                r.pick = 0 if e.x < (r.rect[0] + r.rect[2]) / 2 else 1
            self.render()
        elif i is not None and self.rows[i].kind == 'pair':
            r = self.rows[i]
            pick = 0 if e.x < (r.rect[0] + r.rect[2]) / 2 else 1
            if pick != getattr(r, 'pick', 0):
                r.pick = pick
                self.render()

    def on_click(self, e):
        try:
            self.root.focus_force()
        except tk.TclError:
            pass
        kind = self.title_button_at(e.x, e.y)
        if kind == 'close':
            self.on_close()
            return
        if kind == 'min':
            self.set_tb_hover(None)
            winsys.minimize(self.root)
            return
        if e.y < self.X(BAND[2]):   # the band moves the window, like a title bar
            if not winsys.start_drag(self.root):
                self.drag = (e.x_root - self.root.winfo_x(), e.y_root - self.root.winfo_y())
            return
        i = self.hit(e.x, e.y)
        if i is None:
            return
        self.focus = i
        r = self.rows[i]
        if not r.enabled:
            self.render()
            return
        if r.kind == 'toggle' and r.value_rect:
            x0, _, x1, _ = r.value_rect
            r.on(e.x < (x0 + x1) / 2)
            self.render()
        elif r.kind == 'enum' and r.value_rect:
            x0, _, x1, _ = r.value_rect
            (r.left if e.x < x0 + (x1 - x0) / 3 else r.right)()
            self.render()
        elif r.kind == 'pair':
            r.pick = 0 if e.x < (r.rect[0] + r.rect[2]) / 2 else 1
            r.actions[r.pick][1]()
        else:
            self.activate()

    def on_close(self):
        if self.job and self.job.result is None:
            self.job.cancel()
        self.root.destroy()

    # ---------------------------------------------------------------- screens

    def start(self):
        a = self.args
        gd = self.settings.get('game_dir')
        if a.rolled_back:
            self.ctx.log(f'the stub went back from {a.rolled_back} to {self.layout.version}')
        for old in self.layout.root.glob('python.old*'):   # the Python an update replaced (launcher/native/nx2navi.c)
            shutil.rmtree(old, ignore_errors=True)
        if gd or self.settings.get('dump'):
            self.fonts_from_game(gd, self.settings.get('dump'))
        if a.uninstall:
            self.show('uninstall')
        elif a.continue_update is not None and not a.rolled_back:
            # after a rollback the stub passes the failed version's --continue-update on to this one: the game was
            # never rebuilt for the new version, so there is nothing to finish, and home tells why the version is back
            self.run_job(jobs.ContinueUpdateJob(self.ctx, gd if gd and build.built_version(gd) else '',
                                                a.continue_update, self.settings.get('dump')), 'home')
        elif gd and build.built_version(gd):
            self.show('home')
            self.after_update_notice()
            if self.settings.get('auto_check'):
                self.check_now()
        else:
            self.show('first_run')
            self.find_dumps()

    def after_update_notice(self):
        if self.args.rolled_back:
            self.message = (f'Version {self.args.rolled_back} did not start, so NX2navi went back to '
                            f'{versions.display(self.layout.version)}.')

    def show(self, screen, focus=None):
        self.screen = screen
        self.rows = getattr(self, 'rows_' + screen)()
        idx = self.focusables()
        self.focus = focus if focus is not None and focus in idx else (idx[0] if idx else 0)
        if screen == 'job' and focus is None:
            self.focus = self.job_focus()
        self.render()

    # ---- first run

    def find_dumps(self):
        import threading
        extra = [self.args.source] if self.args.source else []
        extra.append(str(self.layout.root.parent))

        def work():
            try:
                found = gamefiles.find_dumps(self.layout, extra)
            except Exception as e:
                self.ctx.log(f'dump search: {e}')
                found = []
            self.root.after(0, lambda: self.dumps_found(found))
        self.dump_search = threading.Thread(target=work, daemon=True)
        self.dump_search.start()

    def dumps_found(self, found):
        self.dumps = found
        if found and not self.settings.get('dump'):
            self.settings['dump'] = str(found[0])
            self.settings.save()
            self.fonts_from_game('', str(found[0]))
        if self.screen == 'first_run':
            self.show('first_run', self.focus)

    def game_dir(self):
        return self.settings.get('game_dir') or str(self.layout.default_game_dir())

    def rows_first_run(self):
        dump = self.settings.get('dump')
        if dump:
            dump_value, dump_style, check = Path(dump).name, 'normal', True
            where = str(Path(dump).parent)
            dump_desc = (f'Your dump: {Path(dump).name}, in {where}. Select this row to choose another .nup file.',)
        elif self.dumps is None:
            dump_value, dump_style, check = 'Looking for your dump…', 'dim', False
            dump_desc = ('Looking for your dump of Ocarina of Time in Downloads, Desktop, Documents and the folder '
                         'the setup ran from.',)
        else:
            dump_value, dump_style, check = 'Choose your .nup file', 'salmon', False
            dump_desc = ('Choose your own dump of Ocarina of Time with the 1.1.1 update, as one .nup file. None was '
                         'found in Downloads, Desktop or Documents.',)
        gd = self.game_dir()
        need = 30
        free = free_gb(gd)
        folder_desc = (f'The game is built into this folder. {free:.0f} GB free on {drive_of(gd)}; the game needs '
                       f'about {need} GB. Select this row to choose another folder.',)
        ready = bool(dump)
        begin_desc = (f'Build Ocarina of Time on this PC from your own dump. It takes 10 to 25 minutes, once, and '
                      f'downloads 170 MB of build tools. The game needs about {need} GB on {drive_of(gd)} '
                      f'({free:.0f} GB free).',) if ready else \
            ('Choose your dump first: select Game Dump above.',)
        return [
            Row('value', 'Game Dump', dump_value, dump_style, big=True, check=check, on=self.choose_dump,
                desc=dump_desc),
            Row('value', 'Install Folder', gd, big=True, on=self.choose_folder, desc=folder_desc),
            Row('divider'),
            self.toggle('Desktop Shortcut', 'desktop_shortcut',
                        'Put an NX2navi shortcut on the desktop. The Start menu always has one.'),
            self.toggle('Check for Updates at Start', 'auto_check',
                        'Look for a new version of NX2navi each time the launcher opens. Nothing is installed '
                        'until you choose Update.'),
            self.channel_row(),
            Row('divider'),
            Row('action', 'Begin Installation', on=self.begin_install, desc=begin_desc, enabled=True),
        ]

    def toggle(self, label, key, desc):
        def on(v):
            self.settings[key] = bool(v)
            self.settings.save()
            if key == 'desktop_shortcut':
                winsys.desktop_shortcut(self.layout, bool(v))
        return Row('toggle', label, on=on, get=lambda: bool(self.settings.get(key)), desc=(desc,))

    def channel_row(self):
        def flip():
            self.settings['channel'] = 'preview' if self.settings.get('channel') == 'stable' else 'stable'
            self.settings.save()
            self.release, self.checked_at = None, 0
        return Row('enum', 'Update Channel', options=('Stable', 'Preview'),
                   get=lambda: 'Preview' if self.settings.get('channel') == 'preview' else 'Stable',
                   left=flip, right=flip,
                   desc=('Stable gets each finished release. Preview also gets test builds before they are '
                         'released, which may have new problems.',))

    def choose_dump(self):
        start = str(Path(self.settings.get('dump')).parent) if self.settings.get('dump') else str(Path.home())
        path = filedialog.askopenfilename(parent=self.root, title='Choose your Ocarina of Time dump',
                                          initialdir=start, filetypes=[('Game dump', '*.nup'), ('All files', '*.*')])
        if not path:
            return
        if not gamefiles.is_game_nup(self.layout, path):
            self.message = f'{Path(path).name} is not a dump of Ocarina of Time.'
            self.render()
            return
        self.settings['dump'] = path
        self.settings.save()
        self.message = ''
        self.fonts_from_game('', path)
        self.show(self.screen, self.focus)

    def choose_folder(self):
        path = filedialog.askdirectory(parent=self.root, title='Choose the folder for the game',
                                       initialdir=str(Path(self.game_dir()).parent))
        if not path:
            return
        p = Path(path)
        if p.exists() and any(p.iterdir()) and not (p / 'build-cache').is_dir() and p.name.lower() != 'nx2navi':
            p = p / 'NX2navi'
        self.settings['game_dir'] = str(p)
        self.settings.save()
        self.show(self.screen, self.focus)

    def begin_install(self):
        dump = self.settings.get('dump')
        if not dump:
            self.focus = 0
            self.choose_dump()
            return
        gd = self.game_dir()
        if free_gb(gd) < 31 and not (Path(gd) / 'romfs' / 'romfs.bin').is_file():
            self.message = f'{drive_of(gd)} has {free_gb(gd):.0f} GB free; the game needs about 30 GB.'
            self.render()
            return
        self.settings['game_dir'] = gd
        self.settings.save()
        if self.settings.get('desktop_shortcut'):
            winsys.desktop_shortcut(self.layout, True)
        self.run_job(jobs.InstallJob(self.ctx, dump, gd), 'home', back='first_run')

    # ---- home

    def rows_home(self):
        gd = self.settings.get('game_dir')
        built = build.built_version(gd)
        mine = versions.display(self.layout.version)
        play_value = 'Ocarina of Time'
        if self.release and jobs.update_available(self.ctx, self.release):
            nv = versions.display(self.release.version)
            upd = Row('value', 'Updates', f'Version {nv}', 'orange_arrow', big=True, icon='refresh', badge=True,
                      on=self.start_update, focus_value=True, desc=self.update_desc)
        elif self.check and self.check.result is None:
            upd = Row('value', 'Updates', 'Checking…', 'dim', big=True, icon='refresh', on=lambda: None,
                      desc=('Looking for a new version of NX2navi.',))
        elif self.check_error:
            upd = Row('value', 'Updates', 'Could not check', 'dim', big=True, icon='refresh',
                      on=lambda: self.check_now(force=True),
                      desc=(f'The update check failed: {self.check_error}', 'Select this row to try again.'))
        elif self.checked_at:
            upd = Row('value', 'Updates', f'Up to Date · {mine}', 'normal', big=True, icon='refresh', check=True,
                      on=lambda: self.check_now(force=True),
                      desc=(f'{mine} is the newest version. Checked {self.ago(self.checked_at)}.',
                            'Select this row to check again.'))
        else:
            upd = Row('value', 'Updates', f'Version {mine}', 'normal', big=True, icon='refresh',
                      on=lambda: self.check_now(force=True),
                      desc=(f'You have NX2navi {mine}. Select this row to look for a new version.',))
        rebuild_value = f'Built with {versions.display(built)}' if built else 'Not built'
        rebuild_style = 'normal' if versions.display(built) == mine else 'salmon'
        play_desc = ('Start Ocarina of Time. The launcher steps aside while you play and comes back when you quit.',)
        if built and versions.display(built) != mine:
            play_desc += (f'The game was built with {versions.display(built)}. Rebuild the Game brings it to {mine}.',)
        if self.message:
            play_desc = (self.message,) + play_desc
        return [
            Row('value', 'Play', play_value, big=True, icon='play', on=self.play, desc=play_desc),
            upd,
            Row('divider'),
            Row('value', 'Game Folder', gd, on=lambda: self.open(gd),
                desc=('Open the game folder. The game keeps your save in its profile folder, its settings in '
                      'nx2navi.ini and its logs in logs. Its saves folder is for saves to import.',)),
            self.saves_row(gd),
            Row('value', 'Rebuild the Game', rebuild_value, rebuild_style, on=self.start_rebuild,
                desc=('Build the game again from the files of the last build. Only what changed is compiled. Use it '
                      'after changing build tools, or if the game stopped starting.',)),
            Row('value', 'Settings', '', 'arrow', on=lambda: self.show('settings'),
                desc=('Updates, the game folder, the logs and uninstalling.',)),
        ]

    def saves_row(self, gd):
        """Import Saves: a save put into the game folder's saves folder, else a folder the player picks."""
        ready = False
        if gd and Path(gd).is_dir():
            try:
                saves.ensure_saves_dir(gd)
                ready = saves.waiting(gd)[0] is not None
            except OSError:
                pass
        desc = ('Bring in a save from a Switch save backup, an emulator or another NX2navi install. Put it into the '
                'saves folder of the game folder and select this row, or select this row and choose its folder.',
                'Your current save is copied to the saves folder first.')
        if self.save_message:
            desc = (self.save_message,) + desc
        return Row('value', 'Import Saves', 'From the Saves Folder' if ready else 'Choose a Folder',
                   'orange_arrow' if ready else 'arrow', on=self.import_saves, desc=desc)

    def import_saves(self):
        gd = self.settings.get('game_dir')
        if not gd or not Path(gd).is_dir():
            self.save_message = 'Install the game first.'
        elif (self.game_proc and self.game_proc.poll() is None) or saves.game_running(gd):
            self.save_message = 'Quit the game first: it keeps its save open while it runs.'
        else:
            try:
                folder = None
                if saves.waiting(gd)[0] is None:
                    folder = filedialog.askdirectory(parent=self.root, title='Choose the folder with the save',
                                                     initialdir=str(saves.ensure_saves_dir(gd)), mustexist=True)
                    if not folder:
                        return
                r = saves.import_save(gd, folder, log=self.ctx.log)
            except (saves.SaveError, OSError) as e:
                self.save_message = f'The save was not imported: {e}'
            else:
                self.save_message = (f'Save imported: {r["files"]} files from {r["source"].name}.'
                                     + (f' Your old save is in saves\\{r["backup"].name}.' if r['backup'] else ''))
        self.show('home', self.focus)

    def update_desc(self):
        r = self.release
        nv, mine = versions.display(r.version), versions.display(self.layout.version)
        notes = feed.plain_notes(r.notes, 6)
        return {'title': f'Version {nv} Is Ready',
                'text': [f'You have {mine}. Updating downloads the new version and rebuilds the game in a few '
                         f'minutes. Your saves and settings stay as they are.'],
                'divider': bool(notes), 'subtitle': "What's New" if notes else '', 'bullets': notes}

    def ago(self, t):
        d = time.time() - t
        if d < 90:
            return 'just now'
        if d < 3600:
            return f'{int(d // 60)} minutes ago'
        return time.strftime('at %H:%M', time.localtime(t))

    def check_now(self, force=False):
        if self.check and self.check.result is None:
            return
        if not force and time.time() - self.settings.get('last_check', 0) < 600 and self.release:
            return
        self.check_error = ''
        self.check = jobs.CheckJob(self.ctx).start()
        if self.screen == 'home':
            self.show('home', self.focus)

    def check_done(self):
        c = self.check
        if c.result == 'ok':
            self.release = c.release
            self.checked_at = time.time()
            self.settings['last_check'] = self.checked_at
            if self.release:
                self.settings['last_seen'] = self.release.version
            self.settings.save()
            self.ctx.log(f'update check: {self.release.version if self.release else "no release"}')
        else:
            self.check_error = c.error or 'cancelled'
            self.ctx.log(f'update check: {self.check_error}')
        if self.screen == 'home':
            self.show('home', self.focus)

    def play(self):
        gd = self.settings.get('game_dir')
        exe = Path(gd) / 'ocarina.exe'
        if not exe.is_file():
            self.message = 'The game is not built yet.'
            self.render()
            return
        if self.game_proc and self.game_proc.poll() is None:
            return
        try:
            self.game_proc = winsys.start_game(exe)
        except OSError as e:
            self.message = f'The game could not start: {e}'
            self.render()
            return
        self.ctx.log(f'play: {exe}')
        if self.settings.get('hide_while_playing'):
            self.root.withdraw()
        self.root.after(1000, self.watch_game)

    def watch_game(self):
        if self.game_proc and self.game_proc.poll() is None:
            self.root.after(1000, self.watch_game)
            return
        code = self.game_proc.returncode if self.game_proc else 0
        self.game_proc = None
        self.root.deiconify()
        self.root.lift()
        if code not in (0, None):
            self.message = f'The game ended with code {code}. Its log is logs\\nx2navi.log in the game folder.'
        self.show('home', self.focus)

    def open(self, path):
        try:
            winsys.open_path(path)
        except OSError as e:
            self.message = str(e)
            self.render()

    def start_update(self):
        if not self.release:
            return
        gd = self.settings.get('game_dir')
        if gd and build.game_running(gd):
            self.message = 'Close the game first: the update rebuilds it.'
            self.show('home', self.focus)
            return
        self.run_job(jobs.UpdateDownloadJob(self.ctx, self.release), 'home')

    def start_rebuild(self):
        gd = self.settings.get('game_dir')
        self.run_job(jobs.RebuildJob(self.ctx, gd, self.settings.get('dump')), 'home')

    # ---- settings

    def rows_settings(self):
        gd = self.settings.get('game_dir')
        dump = self.settings.get('dump')
        return [
            self.toggle('Check for Updates at Start', 'auto_check',
                        'Look for a new version of NX2navi each time the launcher opens. Nothing is installed '
                        'until you choose Update.'),
            self.toggle('Hide Launcher While Playing', 'hide_while_playing',
                        'Hide this window while the game runs; it comes back when you quit the game.'),
            self.toggle('Keep Build Cache', 'keep_cache',
                        'Keep the translated code and compiled files (about 5 GB) in the game folder, so an update '
                        'compiles only what changed. Off: they are deleted after each build.'),
            self.channel_row(),
            Row('divider'),
            Row('value', 'Game Folder', gd or 'Not chosen', on=lambda: gd and self.open(gd),
                desc=('Open the game folder: saves in profile, the game\'s settings in nx2navi.ini, its logs in '
                      'logs.',)),
            Row('value', 'Game Dump', Path(dump).name if dump else 'Not chosen', on=self.choose_dump,
                desc=(f'The dump the game was built from: {dump}.' if dump else 'No dump chosen yet.',
                      'Select this row to choose another .nup file.')),
            Row('divider'),
            Row('value', 'Open Logs Folder', '', 'arrow', on=lambda: self.open(self.layout.logs),
                desc=('The launcher\'s logs. Attach launcher.log when you report a problem with installing or '
                      'updating; never send game files.',)),
            Row('value', 'Uninstall NX2navi', 'Keeps your saves', 'salmon', on=lambda: self.show('uninstall'),
                desc=('Remove the launcher and its build tools. You choose on the next screen whether the built '
                      'game goes too.',)),
            Row('action', 'Back', on=lambda: self.show('home'), height=40, desc=('Back to the main menu.',)),
        ]

    # ---- uninstall

    def rows_uninstall(self):
        def on(v):
            self.uninstall_game = bool(v)
        gd = self.settings.get('game_dir')
        return [
            Row('toggle', 'Remove the Built Game', on=on, get=lambda: self.uninstall_game,
                desc=(f'On: the game folder ({gd or "none"}) is deleted too, saves included.',
                      'Off: the game folder and your saves stay where they are.')),
            Row('divider'),
            Row('pair', actions=[('Uninstall', self.do_uninstall), ('Cancel', lambda: self.show('settings'))],
                desc=('Uninstall removes NX2navi, its build tools and its shortcuts. Your dump is never touched.',)),
        ]

    def do_uninstall(self):
        gd = self.settings.get('game_dir') if self.uninstall_game else ''
        if gd and build.game_running(gd):
            self.message = 'Close the game first.'
            self.render()
            return
        self.ctx.log(f'uninstall (game folder too: {bool(gd)})')
        winsys.uninstall(self.layout, gd, self.ctx.log)
        self.root.destroy()

    # ---- jobs

    def run_job(self, job, return_to, back=None):
        self.job, self.job_return, self.job_back = job, return_to, back or return_to
        self.job_rev = -1
        self.message = ''
        winsys.keep_awake(True)
        job.start()
        self.show('job')

    def rows_job(self):
        j = self.job
        rows = []
        for st in j.steps:
            if st.state == jobs.RUNNING:
                rows.append(Row('progress', st.label, fraction=st.fraction, value=f'{int(st.fraction * 100)}%',
                                focus_value=True, desc=None))
            else:
                style = {jobs.DONE: 'normal', jobs.SKIPPED: 'dim', jobs.PENDING: 'dim', jobs.FAILED: 'salmon'}[st.state]
                rows.append(Row('value', st.label, st.value, style, check=st.state == jobs.DONE, desc=None,
                                focus_value=st.state == jobs.FAILED))
            if st.id in ('romfs', 'verify') and st is not j.steps[-1]:
                rows.append(Row('divider'))
        if j.result is None:
            label = {'install': 'Cancel Installation', 'update': 'Cancel Update'}.get(j.kind, 'Cancel')
            rows.append(Row('action', label, on=self.cancel_job, height=44))
        elif j.result == 'failed':
            rows.append(Row('pair', actions=[('Try Again', self.retry_job), ('Copy Report', self.copy_report)]))
        for r in rows:
            r.desc = self.job_desc
        return rows

    def job_focus(self):
        """The cursor sits on the running step, or on Try Again after a failure."""
        for i, r in enumerate(self.rows):
            if self.job.result == 'failed' and r.kind == 'pair':
                return i
            if r.kind == 'progress':
                return i
        return self.focusables()[-1] if self.focusables() else 0

    def job_desc(self):
        j = self.job
        cur = j.current()
        lines = [ln for ln in j.lines if ln.strip() and not ln.startswith('@@')][-6:]
        if j.result == 'failed':
            return {'title': 'The Build Stopped' if j.kind != 'update' or cur.id not in ('download', 'verify')
                    else 'The Update Stopped',
                    'text': [self.friendly(j.error), 'Nothing in your game folder was changed.' if cur.id not in
                             ('download', 'verify', 'tools', 'tools_update') else ''],
                    'log': lines, 'note': 'Try again first. If it stops again, use Copy Report and send it with your '
                                          'issue. Never send game files.'}
        if j.result == 'cancelled':
            return {'title': 'Cancelled', 'text': ['Nothing was changed that a new start cannot finish.']}
        if j.result == 'ok':
            return {'title': 'Done', 'text': [f'Finished in {jobs.mmss(j.elapsed())}.']}
        text = [cur.desc + (f' {cur.detail}.' if cur.detail else '')]
        eta = j.eta()
        return {'title': self.step_title(cur), 'text': text, 'progress': j.overall(),
                'progress_label': ('Overall', self.eta_text(eta)),
                'log': lines,
                'note': 'Keep using your PC if you like. It stays awake until the build is done.'
                if j.kind != 'update' or cur.id not in ('download', 'verify') else
                'Your saves and settings stay as they are.'}

    @staticmethod
    def eta_text(eta):
        if not eta:
            return ''
        minutes = max(1, int(eta // 60 + 1))
        return 'about 1 minute left' if minutes == 1 else f'about {minutes} minutes left'

    def step_title(self, st):
        return {'tools': 'Gathering Build Tools', 'tools_update': 'Updating Build Tools', 'check': 'Inspecting Your Game',
                'exefs': 'Unpacking the Program', 'romfs': 'Packing the Game Data', 'lift': 'Translating the Code',
                'compile': 'Compiling', 'compile_changed': 'Compiling Changed Files', 'shaders': 'Preparing the Shaders',
                'ready': 'Ready to Play',
                'download': 'Downloading', 'verify': 'Verifying', 'swap': 'Swapping In the New Build'}.get(st.id,
                                                                                                         st.label)

    @staticmethod
    def friendly(err):
        e = (err or 'unknown error').strip()
        return e[0].upper() + e[1:] + ('' if e.endswith('.') else '.')

    def cancel_job(self):
        if self.job and self.job.result is None:
            self.job.cancel()

    def retry_job(self):
        j = self.job
        if isinstance(j, jobs.InstallJob):
            self.run_job(jobs.InstallJob(self.ctx, j.dump, j.game_dir), self.job_return, self.job_back)
        elif isinstance(j, jobs.ContinueUpdateJob):
            self.run_job(jobs.ContinueUpdateJob(self.ctx, j.game_dir, j.old, j.dump), self.job_return)
        elif isinstance(j, jobs.RebuildJob):
            self.run_job(jobs.RebuildJob(self.ctx, j.game_dir, j.dump), self.job_return)
        elif isinstance(j, jobs.UpdateDownloadJob):
            self.run_job(jobs.UpdateDownloadJob(self.ctx, j.release), self.job_return)

    def copy_report(self):
        j = self.job
        report = [f'NX2navi {self.layout.version} ({"checkout" if self.layout.dev else "release"}), {sys.platform}',
                  f'Job: {j.kind}, step: {j.current().label}', f'Error: {j.error}', '', 'Last lines:']
        report += [ln for ln in list(j.lines)[-60:]]
        self.root.clipboard_clear()
        self.root.clipboard_append('\n'.join(report))
        self.message = 'Report copied.'
        self.render()

    def leave_job(self):
        j = self.job
        if j.result == 'ok':
            self.show(self.job_return)
        elif j.result == 'cancelled':
            self.show(self.job_back)
        else:
            self.show(self.job_back)

    def job_finished(self):
        j = self.job
        winsys.keep_awake(False)
        self.ctx.log(f'job {j.kind}: {j.result} {j.error}')
        if j.result == 'ok':
            if j.after == 'restart':
                if winsys.restart(self.layout, ['--continue-update', versions.display(self.layout.version)]):
                    self.root.after(200, self.root.destroy)
                    return
                self.message = 'The update is ready: start NX2navi again to finish it.'
            elif j.after == 'staged':
                self.message = 'The update was downloaded; a checkout keeps running from the repository.'
            if not self.settings.get('keep_cache') and j.kind in ('install', 'update', 'rebuild'):
                self.drop_build_cache()
            if j.kind == 'install':
                self.message = 'The game is built. Select Play.'
            elif j.kind in ('update', 'rebuild'):
                self.message = 'The game is rebuilt and up to date.' if j.kind == 'rebuild' else \
                    f'NX2navi is now {versions.display(self.layout.version)}.'
                self.release = None
            self.reload_fonts_if_new()
            self.show(self.job_return)
            if j.kind == 'install' and self.settings.get('auto_check'):
                self.check_now()
        elif j.result == 'cancelled':
            self.show(self.job_back)
        else:
            self.show('job')

    def drop_build_cache(self):
        """Keep Build Cache off: delete the lifted C and the objects (about 5 GB) but keep state.json, which an
        update needs to rebuild without the dump; the next build then lifts and compiles everything again."""
        gd = self.settings.get('game_dir')
        if not gd:
            return
        cache = Path(gd) / 'build-cache'
        for name in ('lift', 'obj', 'gen'):
            shutil.rmtree(cache / name, ignore_errors=True)
        for name in ('link.rsp', 'link.key'):
            (cache / name).unlink(missing_ok=True)

    def reload_fonts_if_new(self):
        if not self.game_fonts and gamefiles.cached_fonts(self.layout):
            self.reload_fonts()

    # ---------------------------------------------------------------- timers

    def tick(self):
        try:
            if self.check and self.check.result is not None and not getattr(self.check, 'seen', False):
                self.check.seen = True
                self.check_done()
            if self.screen == 'job' and self.job:
                if self.job.rev != self.job_rev:
                    self.job_rev = self.job.rev
                    if self.job.result is not None:
                        self.job_finished()
                    else:
                        self.rows = self.rows_job()
                        self.focus = self.job_focus()
                        self.render()
                elif int(time.time()) != self.last_second and self.job.result is None:
                    self.render()
            self.last_second = int(time.time())
        except Exception as e:   # a drawing error must not stop the timer
            self.ctx.log(f'tick: {e!r}')
        self.root.after(150, self.tick)

    def animate(self):
        if self.cursor_frames:
            self.cursor_k = (self.cursor_k + 1) % len(self.cursor_frames)
            self.canvas.itemconfigure('cursor', image=self.cursor_frames[self.cursor_k])
        self.root.after(250, self.animate)

    # ---------------------------------------------------------------- drawing

    def render(self):
        c = self.canvas
        c.delete('dyn')
        if self.screen == 'job' and self.job:
            if self.job.result is None or self.rows == []:
                self.rows = self.rows_job()
            if not (0 <= self.focus < len(self.rows)) or not self.rows[self.focus].focusable:
                self.focus = self.job_focus()
        x0, x1, y = ROWS
        for i, r in enumerate(self.rows):
            self.draw_row(r, self.X(x0), self.X(y), self.X(x1), self.X(y + r.h))
            y += r.h + GAP
        self.draw_status()
        self.draw_cursor()
        self.draw_desc()

    def draw_status(self):
        if self.screen != 'job' or not self.job:
            return
        j = self.job
        steps = [s for s in j.steps]
        cur = j.current()
        n = steps.index(cur) + 1 if cur in steps else len(steps)
        left = f'Stopped at step {n} of {len(steps)}' if j.result == 'failed' else f'Step {n} of {len(steps)}'
        if self.message:
            left = self.message
        y = self.X(H - 24)
        self.canvas.create_text(self.X(PANEL[0] + 30), y, text=left, anchor='sw', font=self.f['status'],
                                fill=P['status'], tags='dyn')
        self.canvas.create_text(self.X(PANEL[2] - 30), y, text=f'Elapsed {jobs.mmss(j.elapsed())}', anchor='se',
                                font=self.f['status'], fill=P['status'], tags='dyn')

    def text(self, x, y, text, font, fill, anchor='w', shadow=True, width=0):
        c = self.canvas
        if shadow:
            c.create_text(x + 1, y + 1, text=text, font=font, fill='#2a1e10', anchor=anchor, tags='dyn',
                          width=width)
        return c.create_text(x, y, text=text, font=font, fill=fill, anchor=anchor, tags='dyn', width=width)

    def draw_row(self, r, x0, y0, x1, y1):
        c, s, X = self.canvas, self.s, self.X
        r.rect = (x0, y0, x1, y1)
        if r.kind == 'divider':
            img = self.sprite(('filigree', x1 - x0), lambda: art.filigree(x1 - x0, s)[0])
            c.create_image(x0, (y0 + y1) // 2, anchor='w', image=img, tags='dyn')
            return
        if r.kind in ('action', 'pair'):
            parts = [(r.label, r.on)] if r.kind == 'action' else r.actions
            gap = X(GAP)
            w = (x1 - x0 - gap * (len(parts) - 1)) // len(parts)
            r.parts = []
            for k, (label, cb) in enumerate(parts):
                px0 = x0 + k * (w + gap)
                px1 = px0 + w if k < len(parts) - 1 else x1
                img = self.sprite(('label', px1 - px0, y1 - y0), lambda a=px1 - px0, b=y1 - y0: art.label_cell(a, b, s))
                ring = max(1, int(round(s)))
                c.create_image(px0 - ring, y0 - ring, anchor='nw', image=img, tags='dyn')
                font = self.fit_font(self.f['action_big'] if r.h >= 50 else self.f['action'], label,
                                     px1 - px0 - X(16))
                self.text((px0 + px1) // 2, (y0 + y1) // 2, label, font, P['text_bright'], anchor='center')
                r.parts.append(((px0, y0, px1, y1), label, cb))
            r.value_rect = None
            return
        lw = X(LABEL_W)
        lx1 = x0 + lw
        img = self.sprite(('label', lw, y1 - y0), lambda: art.label_cell(lw, y1 - y0, s))
        ring = max(1, int(round(s)))
        c.create_image(x0 - ring, y0 - ring, anchor='nw', image=img, tags='dyn')
        tx = x0 + X(16)
        if r.icon:
            size = X(32)
            ic = self.sprite(('icon', r.icon, size), lambda: art.framed_icon(size, s, r.icon))
            c.create_image(tx, (y0 + y1) // 2, anchor='w', image=ic, tags='dyn')
            tx += size + X(14)
        font = (self.f['big'] if r.icon else self.f['big_small']) if r.big else self.f['label']
        font = self.fit_font(font, r.label, lx1 - tx - X(10) - (X(32) if r.badge else 0))
        tid = self.text(tx, (y0 + y1) // 2, r.label, font, P['text_bright'] if r.big else P['text'])
        if r.badge:
            bx = self.canvas.bbox(tid)[2] + X(10)
            size = X(22)
            b = self.sprite(('badge', size), lambda: art.badge(size, s))
            c.create_image(bx, (y0 + y1) // 2, anchor='w', image=b, tags='dyn')
            c.create_text(bx + (size + 4) // 2, (y0 + y1) // 2, text='!', font=self.f['badge'], fill='#3a2208',
                          tags='dyn')
        vx0 = lx1
        r.value_rect = (vx0, y0, x1, y1)
        c.create_rectangle(vx0, y0, x1 - 1, y1 - 1, fill=P['value'], outline=P['value_border'], width=1, tags='dyn')
        pad = X(3) if not r.big else X(4)
        ix0, iy0, ix1, iy1 = vx0 + pad, y0 + pad, x1 - pad, y1 - pad
        cy = (y0 + y1) // 2
        cx = (vx0 + x1) // 2
        vfont = self.f['label'] if r.kind in ('toggle', 'enum', 'progress') or r.style == 'orange_arrow' \
            else self.f['value']
        if r.kind == 'toggle':
            on = bool(r.get())
            mid = (ix0 + ix1) // 2
            for k, (label, sel) in enumerate((('On', on), ('Off', not on))):
                sx0, sx1 = (ix0, mid) if k == 0 else (mid, ix1)
                if sel:
                    self.orange(sx0, iy0, sx1, iy1)
                self.canvas.create_text((sx0 + sx1) // 2, cy, text=label, font=vfont,
                                        fill=P['on_text'] if sel else P['off'], tags='dyn')
        elif r.kind == 'enum':
            self.orange(ix0, iy0, ix1, iy1)
            cw, ch = X(9), X(12)
            for left in (True, False):
                img = self.sprite(('chev', cw, ch, left, 'on'), lambda lf=left: art.chevron(cw, ch, P['on_text'], lf))
                c.create_image(ix0 + X(9) if left else ix1 - X(9), cy, anchor='w' if left else 'e', image=img,
                               tags='dyn')
            self.canvas.create_text(cx, cy, text=r.get(), font=vfont, fill=P['on_text'], tags='dyn')
        elif r.kind == 'progress':
            fw = int((ix1 - ix0) * max(0.0, min(1.0, r.fraction)))
            if fw > 2:
                self.orange(ix0, iy0, ix0 + fw, iy1)
            self.canvas.create_text(cx + 1, cy + 1, text=r.value, font=vfont, fill='#1a120a', tags='dyn')
            self.canvas.create_text(cx, cy, text=r.value, font=vfont, fill=P['on_text'], tags='dyn')
        elif r.style == 'orange_arrow':
            self.orange(ix0, iy0, ix1, iy1)
            cw, ch = X(9), X(12)
            label_w = vfont.measure(r.value)
            total = label_w + X(10) + cw
            sx = cx - total // 2
            self.canvas.create_text(sx, cy, text=r.value, font=vfont, fill=P['on_text'], anchor='w', tags='dyn')
            img = self.sprite(('chev', cw, ch, False, 'on'), lambda: art.chevron(cw, ch, P['on_text']))
            c.create_image(sx + label_w + X(10), cy, anchor='w', image=img, tags='dyn')
        elif r.style == 'arrow':
            cw, ch = X(9), X(12)
            img = self.sprite(('chev', cw, ch, False, 'dim'), lambda: art.chevron(cw, ch, '#d9ccae'))
            c.create_image(x1 - X(16), cy, anchor='e', image=img, tags='dyn')
            if r.value:   # a value, centred in the cell's room left of the arrow
                right = x1 - X(16) - cw - X(10)
                avail = right - vx0 - X(16)
                vfont = self.fit_font(vfont, r.value, avail, 0.8)
                c.create_text((vx0 + right) // 2, cy, text=fit(vfont, r.value, avail), font=vfont,
                              fill=P['value_text'], tags='dyn')
        else:
            color = {'normal': P['value_text'], 'dim': P['dim'], 'salmon': P['salmon']}.get(r.style, P['value_text'])
            avail = (x1 - vx0) - X(20) - (X(21) if r.check else 0)
            vfont = self.fit_font(vfont, r.value, avail, 0.8)
            label = fit(vfont, r.value, avail)
            tw = vfont.measure(label)
            sx = cx - (tw + (X(21) if r.check else 0)) // 2
            if r.check:
                size = X(14)
                ck = self.sprite(('check', size), lambda: art.check_mark(size, s))
                c.create_image(sx, cy, anchor='w', image=ck, tags='dyn')
                sx += X(21)
            self.canvas.create_text(sx, cy, text=label, font=vfont, fill=color, anchor='w', tags='dyn')

    def orange(self, x0, y0, x1, y1, tags=('dyn',)):
        c = self.canvas
        c.create_rectangle(x0, y0, x1 - 1, y1 - 1, fill=P['orange'], outline=P['orange_dark'], width=1, tags=tags)
        if x1 - x0 > 4:
            c.create_rectangle(x0 + 1, y0 + 1, x1 - 2, y1 - 2, outline=P['orange_light'], width=1, tags=tags)

    def draw_cursor(self):
        r = self.focused()
        self.cursor_frames = []
        if not r or not r.rect:
            return
        if r.kind == 'pair' and r.parts:
            box = r.parts[getattr(r, 'pick', 0)][0]
        elif (r.kind in ('toggle', 'enum', 'progress') or r.focus_value) and r.value_rect:
            box = r.value_rect
        else:
            box = r.rect
        x0, y0, x1, y1 = box
        frames, mg = self.glow(x1 - x0, y1 - y0)
        self.cursor_frames = frames
        self.cursor_k %= len(frames)
        self.canvas.create_image(x0 - mg, y0 - mg, anchor='nw', image=frames[self.cursor_k], tags=('dyn', 'cursor'))

    def glow(self, w, h):
        key = ('glow', w, h)
        if key not in self.sprites:
            frames, mg = art.glow_frames(w, h, self.s)
            self.sprites[key] = ([art.photo(self.root, f) for f in frames], mg)
        return self.sprites[key]

    def draw_desc(self):
        r = self.focused()
        d = r.desc if r else None
        if callable(d):
            d = d()
        if not d:
            if self.message and self.screen != 'job':
                d = (self.message,)
            else:
                return
        if isinstance(d, tuple):
            d = {'text': [t for t in d if t]}
        elif self.message and self.screen in ('first_run', 'settings', 'uninstall'):
            d = dict(d)
            d['text'] = [self.message] + list(d.get('text', []))
        c, X = self.canvas, self.X
        dx, dw = DESC
        x0 = X(dx)
        w = X(dw)
        padx, pady = X(26), X(22)
        inner = w - 2 * padx
        items = []
        y = 0

        def add(kind, *args):
            items.append((kind, args))

        if d.get('title'):
            add('text', d['title'], self.f['heading'], P['text_bright'], X(14))
        for t in d.get('text', []):
            if t:
                add('text', t, self.f['desc'], P['desc_text'], X(12))
        if d.get('progress') is not None:
            add('progress', d['progress'], d.get('progress_label', ('', '')))
        if d.get('divider'):
            add('filigree')
        if d.get('subtitle'):
            add('text', d['subtitle'], self.f['sub'], P['gold'], X(8))
        for b in d.get('bullets', []):
            add('bullet', b)
        if d.get('log'):
            add('log', d['log'])
        if d.get('note'):
            add('text', d['note'], self.f['small'], P['muted'], X(4))
        # measure: draw at a provisional top, then move everything to the final top
        top0 = X(112)
        y = top0 + pady
        made = []
        for kind, args in items:
            if kind == 'text':
                text, font, fill, after = args
                tid = c.create_text(x0 + padx, y, text=text, font=font, fill=fill, anchor='nw', width=inner,
                                    tags=('dyn', 'desc'))
                bb = c.bbox(tid)
                y = bb[3] + after
                made.append(tid)
            elif kind == 'progress':
                frac, (left, right) = args
                c.create_text(x0 + padx, y, text=left, font=self.f['small'], fill=P['muted'], anchor='nw',
                              tags=('dyn', 'desc'))
                c.create_text(x0 + w - padx, y, text=right, font=self.f['small'], fill=P['muted'], anchor='ne',
                              tags=('dyn', 'desc'))
                y += self.f['small'].metrics('linespace') + X(6)
                h = X(22)
                c.create_rectangle(x0 + padx, y, x0 + w - padx - 1, y + h - 1, fill=P['value'],
                                   outline=P['value_border'], tags=('dyn', 'desc'))
                fw = int((inner - X(6)) * frac)
                if fw > 2:
                    self.orange(x0 + padx + X(3), y + X(3), x0 + padx + X(3) + fw, y + h - X(3), ('dyn', 'desc'))
                y += h + X(14)
            elif kind == 'filigree':
                img = self.sprite(('filigree', inner), lambda: art.filigree(inner, self.s)[0])
                c.create_image(x0 + padx, y + X(9), anchor='w', image=img, tags=('dyn', 'desc'))
                y += X(22)
            elif kind == 'bullet':
                (text,) = args
                c.create_rectangle(x0 + padx + X(1), y + X(8), x0 + padx + X(6), y + X(13), fill='#c99a45',
                                   outline='', tags=('dyn', 'desc'))
                tid = c.create_text(x0 + padx + X(16), y, text=text, font=self.f['value'], fill=P['desc_text'],
                                    anchor='nw', width=inner - X(16), tags=('dyn', 'desc'))
                y = c.bbox(tid)[3] + X(8)
            elif kind == 'log':
                (lines,) = args
                lh = self.f['mono'].metrics('linespace')
                shown = [fit(self.f['mono'], ln, inner - X(24)) for ln in lines[-6:]]
                h = lh * max(len(shown), 1) + X(20)
                c.create_rectangle(x0 + padx, y, x0 + w - padx - 1, y + h - 1, fill=P['value'],
                                   outline=P['value_border'], tags=('dyn', 'desc'))
                ty = y + X(10)
                for ln in shown:
                    color = P['salmon'] if ln.lower().startswith(('error', 'stopped')) else P['log_text']
                    c.create_text(x0 + padx + X(12), ty, text=ln, font=self.f['mono'], fill=color, anchor='nw',
                                  tags=('dyn', 'desc'))
                    ty += lh
                y += h + X(14)
        height = y - top0 + pady - X(4)
        top = X(236) if height < X(200) else X(112)
        top = min(top, X(H - 30) - height) if top + height > X(H - 30) else top
        dy = top - top0
        if dy:
            c.move('desc', 0, dy)
        hb = max(X(60), height)
        key = ('desc', w, hb // 4)
        if key not in self.sprites:
            img, pad = art.desc_box(w, hb, self.s)
            self.sprites[key] = (art.photo(self.root, img), pad)
        pic, pad = self.sprites[key]
        bid = c.create_image(x0 - pad, top - pad, anchor='nw', image=pic, tags='dyn')
        c.tag_lower(bid, 'desc')


def parse_args(argv):
    ap = argparse.ArgumentParser(prog='NX2navi')
    ap.add_argument('--from', dest='source', help='a folder to look in for the dump first (the setup\'s folder)')
    ap.add_argument('--continue-update', metavar='OLD', help='finish an update after the restart (from OLD)')
    ap.add_argument('--rolled-back', metavar='VERSION', help='the stub went back from VERSION')
    ap.add_argument('--uninstall', action='store_true')
    ap.add_argument('--root', help='the launcher folder (tests)')
    ap.add_argument('--selftest', action='store_true', help='draw every screen and close')
    ap.add_argument('--shots', help='with --selftest: save a PNG of each screen into this folder')
    a, _unknown = ap.parse_known_args(argv)
    return a


def single_instance(layout, wait):
    """An exclusive lock on data/launcher.lock for this process's life, or None when another launcher holds it after
    wait seconds (the one an update restart replaces may still be closing)."""
    path = layout.data / 'launcher.lock'
    deadline = time.time() + wait
    while True:
        try:
            f = open(path, 'a+')
            try:
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return f
            except OSError:
                f.close()
                raise
        except OSError:
            if time.time() >= deadline:
                return None
            time.sleep(0.25)


def main(argv=None):
    a = parse_args(sys.argv[1:] if argv is None else argv)
    winsys.dpi_aware()
    winsys.app_id()
    from ..paths import Layout
    ctx = Context(Layout(root=a.root) if a.root else None)
    lock = None
    if not a.selftest:
        lock = single_instance(ctx.layout, 20 if (a.continue_update or a.rolled_back) else 2)
        if lock is None:
            ctx.log('another launcher is running')
            return 0
    root = tk.Tk(className='NX2navi')
    app = App(root, ctx, a)
    if a.selftest:
        from .selftest import run
        root.after(300, lambda: run(app, a.shots))
    root.mainloop()
    return 0
