"""PlayStation (DualSense) and Steam Deck button prompts for Ocarina of Time (Switch 2, game 1.1.1): a RomFS overlay
folder for runtime/romfs.c overlay_init (prompts = playstation or steamdeck), drawn on the player's machine from the
game's own files.

Ocarina draws its prompts as text in its button font, Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs: a zstd frame
(with a dictionary of Pack/ZsDic.pack.zs) holding a .bfotf (tools/button_icons.py bfotf_decode) whose private-use code
points from U+E040 are the buttons in TotK's order: A B X Y, L R ZL ZR, d-pad, Home, Plus, Minus, then the sticks and
the d-pad's directions. This tool redraws them as a DualSense or a Steam Deck names them.

PlayStation:

- the face buttons: Cross, Circle, Square and Triangle, as filled discs with the symbol cut out. Which Switch button
  shows which symbol follows the pad layout (runtime/input_map.c), as the Xbox prompts do:
  - --style playstation, for pad_layout = nintendo: buttons by position, so Switch A (right) is Circle, B (bottom)
    Cross, X (top) Triangle and Y (left) Square;
  - --style playstation-label, for pad_layout = xbox and auto (a DualSense is not labelled the Nintendo way): Switch A
    is on Cross, B on Circle, X on Square and Y on Triangle.
- L R ZL ZR as L1 R1 L2 R2 (the labels cut in ClarimoUDPE-DemiBold, one of the game's text fonts), Plus as Options
  (three bars), Minus as Create, and the d-pad as a cross with the meant directions lit.

Steam Deck (its buttons are labelled as an Xbox pad's, A at the bottom):
- the face buttons keep the game's A B X Y discs. --style steamdeck, for pad_layout = nintendo, swaps their glyphs (A
  with B, X with Y), as the right button that does Switch A is labelled B; --style steamdeck-label, for xbox and auto,
  keeps them.
- L R ZL ZR as L1 R1 L2 R2, Plus as Menu (three bars), Minus as View (two windows), the d-pad as a cross.

In every style the sticks, Home and the console keep the game's glyphs.

The font grows by a few hundred bytes, more than its entry of the resource size table
(System/Resource/ResourceSizeTable.Product.111...rsizetable.zs) leaves room for, so that table goes into the overlay
too with the entry raised (button_icons.grow_restbl). Both files are compressed again with the dictionary of the
original frame.

    .venv-py314/bin/python tools/ocarina_prompts.py --romfs <game>/romfs --out <game>/romfs-patches-playstation
    .venv-py314/bin/python tools/ocarina_prompts.py --romfs <game>/romfs --style playstation-label \\
        --out <game>/romfs-patches-playstation-label

--romfs: the folder with romfs.bin and index.json (tools/make_game.py's <out>/romfs). The output folder holds the
overlay files and manifest.tsv (path, size, SHA-256 of the overlay file, SHA-256 of the original), the format
runtime/romfs.c overlay_load checks. Everything in it is derived from the player's game: keep it on their machine.
Needs Python 3.14 (compression.zstd) and fontTools.
"""
import argparse
import compression.zstd as zstd
import hashlib
import io
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import button_icons as bi  # noqa: E402
from fontTools.pens.reverseContourPen import ReverseContourPen  # noqa: E402
from fontTools.ttLib import TTFont  # noqa: E402

FONT = 'Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs'
TEXT_FONT = 'Font2/Scalable/ClarimoUDPE-DemiBold.Nin_Ounce_NVN2.bfotf.zs'
RESTBL = 'System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'
STYLES = ('playstation', 'playstation-label', 'steamdeck', 'steamdeck-label')
# Switch face button -> DualSense symbol, per style (the module docstring).
FACES = {
    'playstation': {0xE040: 'circle', 0xE041: 'cross', 0xE042: 'triangle', 0xE043: 'square'},
    'playstation-label': {0xE040: 'cross', 0xE041: 'circle', 0xE042: 'square', 0xE043: 'triangle'},
    'steamdeck': {}, 'steamdeck-label': {},   # the game's own letters
}
# Face glyphs that trade places, per style: the Steam Deck under the Nintendo layout names A as B and X as Y.
SWAPS = {'steamdeck': ((0xE040, 0xE041), (0xE042, 0xE043))}


class RomFS:
    """Files of the RomFS image tools/make_game.py writes (romfs.bin and index.json)."""
    def __init__(self, folder):
        self.folder = Path(folder)
        index = json.loads((self.folder / 'index.json').read_text(encoding='utf-8'))
        self.files = {f['path']: f for f in index['files']}
        self.dicts = None

    def raw(self, path):
        f = self.files[path]
        with open(self.folder / 'romfs.bin', 'rb') as image:
            image.seek(f['offset'])
            data = image.read(f['size'])
        assert len(data) == f['size'], path
        return data

    def dictionary(self, dict_id):
        if self.dicts is None:
            pack = zstd.decompress(self.raw(DICTIONARIES))
            self.dicts = {}
            for lo, hi in sarc_members(pack).values():
                d = zstd.ZstdDict(pack[lo:hi])
                self.dicts[d.dict_id] = d
        return self.dicts[dict_id] if dict_id else None

    def read(self, path):
        """(contents, dictionary of its zstd frame or None)."""
        data = self.raw(path)
        d = self.dictionary(zstd.get_frame_info(data).dictionary_id)
        return zstd.decompress(data, zstd_dict=d), d


def sarc_members(data):
    """Member name -> (start, end) of a SARC archive."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, _, start = struct.unpack_from('<HxxII', data, 4)
    assert data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]
    nodes = [struct.unpack_from('<IIII', data, header + 12 + 16 * i) for i in range(count)]
    sfnt = header + 12 + 16 * count
    assert data[sfnt:sfnt + 4] == b'SFNT'
    out = {}
    for _, attr, lo, hi in nodes:
        at = sfnt + 8 + (attr & 0xffff) * 4
        out[data[at:data.index(b'\0', at)].decode()] = (start + lo, start + hi)
    return out


# ---- drawing, in the button font's 1000-unit em (button_icons: the round icons fill y Y0..Y1)
def face(symbol, advance):
    """A filled disc with a DualSense face symbol cut out of it."""
    def draw(pen):
        cx, r = advance / 2, bi.R
        bi.circle(pen, cx, bi.CY, r)
        hole = ReverseContourPen(pen)
        s, t = r * 0.50, r * 0.13   # the symbol's half size and its stroke
        if symbol == 'circle':   # a ring: the outer contour a hole, the inner one filled again
            bi.circle(hole, cx, bi.CY, s)
            bi.circle(pen, cx, bi.CY, s - 2 * t)
        elif symbol == 'square':
            q = s * 0.86
            bi.polygon(hole, [(cx - q, bi.CY - q), (cx + q, bi.CY - q), (cx + q, bi.CY + q), (cx - q, bi.CY + q)])
            q2 = q - 2 * t
            bi.polygon(pen, [(cx - q2, bi.CY - q2), (cx + q2, bi.CY - q2), (cx + q2, bi.CY + q2), (cx - q2, bi.CY + q2)])
        elif symbol == 'triangle':
            h = s * 1.05
            top, left, right = (cx, bi.CY + h), (cx - h * 0.95, bi.CY - h * 0.62), (cx + h * 0.95, bi.CY - h * 0.62)
            bi.polygon(hole, [left, right, top])
            k = 0.52   # the inner triangle, scaled about the outer one's centroid and filled again
            gx, gy = cx, (top[1] + left[1] + right[1]) / 3
            bi.polygon(pen, [(gx + (x - gx) * k, gy + (y - gy) * k) for x, y in (left, right, top)])
        else:   # cross: one 12-point outline (two overlapping bars would cancel each other out where they meet)
            d, k = s * 0.80, t * 1.05 / 1.41421356   # arm tip centre offset; bar half-width per axis at the tips
            c, y = 2 * k, bi.CY                         # where the bars' edges cross, on each axis
            bi.polygon(hole, [(cx, y - c), (cx + d - k, y - d - k), (cx + d + k, y - d + k),
                              (cx + c, y), (cx + d + k, y + d - k), (cx + d - k, y + d + k),
                              (cx, y + c), (cx - d + k, y + d + k), (cx - d - k, y + d - k),
                              (cx - c, y), (cx - d - k, y - d + k), (cx - d + k, y - d - k)])
    return advance, draw


def create_button(advance):
    """Create (Minus): a disc with the button's three short strokes fanned out."""
    def draw(pen):
        cx, r = advance / 2, bi.R
        bi.circle(pen, cx, bi.CY, r)
        hole = ReverseContourPen(pen)
        for dx, lean in ((-0.42, -0.45), (0.0, 0.0), (0.42, 0.45)):
            x0, y0 = cx + dx * r * 0.55, bi.CY - r * 0.40
            x1, y1 = x0 + lean * r * 0.55, y0 + r * 0.80
            w = r * 0.075
            bi.polygon(hole, [(x0 - w, y0), (x1 - w, y1), (x1 + w, y1), (x0 + w, y0)][::-1])
    return advance, draw


def icons(style, g, adv):
    """Code point -> (advance, draw) of every glyph this style redraws."""
    base = bi.xbox_icons(g)   # the Xbox shapes for the shoulders, triggers, Plus and the d-pad, relabelled below
    out = {cp: face(sym, adv[cp]) for cp, sym in FACES[style].items()}

    def shoulder(label, trigger, cp):
        def draw(pen):
            a = adv[cp]
            if trigger:
                bi.rounded(pen, 60, bi.Y0, a - 60, bi.Y1, 60, 230); g.hole(pen, label, a / 2, bi.CY - 20, 230, a - 220)
            else:
                bi.rounded(pen, 40, bi.Y0 + 70, a - 40, bi.Y1 - 70, 70, 150); g.hole(pen, label, a / 2, bi.CY, 230, a - 200)
        return adv[cp], draw

    out.update({0xE044: shoulder('L1', False, 0xE044), 0xE045: shoulder('R1', False, 0xE045),
                0xE046: shoulder('L2', True, 0xE046), 0xE047: shoulder('R2', True, 0xE047)})
    out[0xE04B] = base[0xE04B]   # Options (DualSense) or Menu (Steam Deck): three bars
    # Minus: Create on a DualSense, View (two windows) on a Steam Deck
    out[0xE04C] = base[0xE04C] if style.startswith('steamdeck') else create_button(adv[0xE04C])
    for cp in (0xE049, 0xE060, 0xE061, 0xE062, 0xE063, 0xE064, 0xE065):
        out[cp] = base[cp]
    return out


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--romfs', type=Path, required=True, help='the folder with romfs.bin and index.json')
    ap.add_argument('--out', type=Path, required=True, help='the overlay folder to write')
    ap.add_argument('--style', choices=STYLES, default='playstation',
                    help='the controller, and with -label the face buttons by label (pad_layout = xbox or auto)')
    args = ap.parse_args()

    romfs = RomFS(args.romfs)
    font_bfotf, font_dict = romfs.read(FONT)
    otf, key = bi.bfotf_decode(font_bfotf)
    text = TTFont(io.BytesIO(bi.bfotf_decode(romfs.read(TEXT_FONT)[0])[0]))
    adv = bi.font_advances(font_bfotf)
    new_otf = bi.redraw_font(otf, icons(args.style, bi.Glyphs([text]), adv), SWAPS.get(args.style, ()))
    new_bfotf = bi.bfotf_encode(new_otf, key, font_bfotf)
    table, table_dict = romfs.read(RESTBL)
    grown = len(new_bfotf) - len(font_bfotf)
    if grown > 0:
        table = bi.grow_restbl(table, {FONT: grown})

    out = {FONT: zstd.compress(new_bfotf, level=19, zstd_dict=font_dict),
           RESTBL: zstd.compress(table, level=19, zstd_dict=table_dict)}
    args.out.mkdir(parents=True, exist_ok=True)
    lines = ['# path\tsize\tsha256\tsha256 of the original (tools/ocarina_prompts.py --style %s)' % args.style]
    for path, data in out.items():
        dest = args.out / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        lines.append(f'{path}\t{len(data)}\t{sha256(data)}\t{sha256(romfs.raw(path))}')
    (args.out / 'manifest.tsv').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'{args.out}: {args.style} prompts, the button font {len(font_bfotf)} -> {len(new_bfotf)} bytes'
          + (f', its resource size entry raised by {grown}' if grown > 0 else ''))


if __name__ == '__main__':
    main()
