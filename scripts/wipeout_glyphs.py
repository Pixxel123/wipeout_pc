# SPDX-License-Identifier: GPL-2.0-or-later
"""Controller button glyphs for WipEout's own prompts, in the game's style.

WipEout writes its button prompts with a font of PlayStation buttons (fe/fonts/ps_buttons: the
.fnt maps a character to a rectangle of the .gnf atlas) and shows the L1/R1 tabs of its menus as
pictures (fe/images/but_l1_hd, but_r1_hd), entries of data00.psarc and data08.psarc. build()
redraws them with the buttons of an Xbox or Nintendo controller (scripts/pad_styles.py): each
glyph keeps its own outline (ring, box, trigger shield, oval, pill) and only its symbol or label
changes, white on black as the game draws them, or black on a filled shape. The labels follow
GOW3_PAD_MAP. The archives are never written: the entries are packed into the bytes the originals
take and applied as runtime file patches (scripts/psarc.py).
"""
import argparse
import struct
from pathlib import Path

import gnf
import pad_styles
import psarc

VERSION = 2  # of the drawing: a change redraws the cached entries
FONT_MAP = 'data/fe/fonts/ps_buttons.fnt'
FONT_ATLAS = 'data/fe/fonts/ps_buttons.gnf'
TABS = {'data/fe/images/but_l1_hd.gnf': 'l1', 'data/fe/images/but_r1_hd.gnf': 'r1'}
SCALE = 4  # labels drawn this many times larger, then reduced
RING_LETTER = 0.5  # a face button's letter: this share of the inside of its ring
FONT = Path(__file__).resolve().parent.parent / 'launcher' / 'RussoOne-Regular.ttf'
# The gamepad button that presses each game button by default (src/runtime_pad.c pad_map); L2/R2
# stay on the triggers.
DEFAULT_PAD = {'cross': 'a', 'circle': 'b', 'square': 'x', 'triangle': 'y', 'l1': 'leftshoulder',
               'r1': 'rightshoulder', 'l2': 'lefttrigger', 'r2': 'righttrigger', 'l3': 'leftstick',
               'r3': 'rightstick', 'options': 'start', 'touchpad_right': 'back'}
# Buttons drawn as a symbol rather than their label; the d-pad's as an arrow head.
SYMBOLS = {'start': {'xbox': 'menu', 'nintendo': 'plus'}, 'back': {'xbox': 'view', 'nintendo': 'minus'}}
ICONS = {'menu', 'view', 'plus', 'minus', *pad_styles.DPAD}

# The characters of ps_buttons and the game button each shows. Face buttons come in sets of
# triangle, circle, square, cross: rings (four sizes), a thin ring and the bare symbols.
FACES = ('triangle', 'circle', 'square', 'cross')
CHARACTERS = {}
for chars in ('0123', 'ABCD', 'abcd', 'ΪΫάέ', 'βγδε'):
    CHARACTERS.update(dict(zip(chars, FACES)))
CHARACTERS.update({'ή': 'triangle', 'ί': 'circle', 'ΰ': 'square', 'α': 'cross'})
for chars, button in (('4EΚ', 'l1'), ('5FΛ', 'l2'), ('6GΜ', 'r1'), ('7HΝ', 'r2'),
                      ('ef', 'l3'), ('gh', 'r3'), ('OP', 'l1'), ('QR', 'r1'), ('IΞΟ', 'options'),
                      ('=NΧ', 'touchpad_right'), ('i', 'options')):
    CHARACTERS.update(dict.fromkeys(chars, button))
BARE = set('ήίΰα')
CAPTIONED = set('IΞΟ')  # OPTIONS written above a pill: the word changes, the pill stays
REDRAWN = set('=NΧ')  # the touchpad's dotted plate: its button's icon instead
WORDS = {'menu': 'MENU', 'view': 'VIEW', 'plus': '+', 'minus': '−'}


def font_map(data):
    """{character: (x0, y0, x1, y1)} of a ps_buttons.fnt: 18-byte glyph records from 0x240 (the
    character, its width and height, then x0, x1, y0, y1 in the atlas), until the first empty one."""
    rects = {}
    for at in range(0x240, len(data) - 17, 18):
        code, _w, _h, x0, x1, y0, y1 = struct.unpack_from('<HBBHHHH', data, at)
        if not code:
            break
        rects[chr(code)] = (x0, y0, x1, y1)
    return rects


def pad_buttons(pad_map):
    """Game button -> gamepad button, with a GOW3_PAD_MAP value ('cross=b,circle=a') applied; L2/R2
    stay on the triggers, as in the runtime."""
    buttons = dict(DEFAULT_PAD)
    for entry in (pad_map or '').split(','):
        game, _, host = (part.strip().casefold() for part in entry.partition('='))
        if game in buttons and host and game not in ('l2', 'r2'):
            buttons[game] = host
    return buttons


def label(button, style, pad_map):
    """The label (letters, or an ICONS name) of a game button on this controller."""
    host = pad_buttons(pad_map).get(button)
    if host in SYMBOLS:
        return SYMBOLS[host][style]
    if host in pad_styles.DPAD:
        return host
    return pad_styles.STYLES[style].get(host, (None,))[0]


def draw_icon(image, box, name, color):
    """An ICONS shape centred in box: Menu's three bars, Plus, Minus, View's two windows, or a
    d-pad direction's arrow head."""
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    x0, y0, x1, y1 = box
    cx, cy, r = (x0 + x1) / 2, (y0 + y1) / 2, min(x1 - x0, y1 - y0) / 2
    t = max(1, r * 0.26)
    if name in ('plus', 'minus', 'menu'):
        for row in ((-0.55, 0, 0.55) if name == 'menu' else (0,)):
            draw.rounded_rectangle((cx - r * 0.8, cy + row * r - t / 2, cx + r * 0.8, cy + row * r + t / 2), t / 3, fill=color)
        if name == 'plus':
            draw.rounded_rectangle((cx - t / 2, cy - r * 0.8, cx + t / 2, cy + r * 0.8), t / 3, fill=color)
    elif name == 'view':
        for dx, dy in ((-0.25, -0.2), (0.25, 0.2)):
            draw.rectangle((cx + (dx - 0.55) * r, cy + (dy - 0.45) * r, cx + (dx + 0.55) * r, cy + (dy + 0.45) * r),
                           outline=color, width=int(t * 0.7))
    else:
        ax, ay = {'dpup': (0, -1), 'dpdown': (0, 1), 'dpleft': (-1, 0), 'dpright': (1, 0)}[name]
        draw.polygon([(cx + ax * r * 0.8, cy + ay * r * 0.8),
                      (cx - ax * r * 0.5 - ay * r * 0.7, cy - ay * r * 0.5 - ax * r * 0.7),
                      (cx - ax * r * 0.5 + ay * r * 0.7, cy - ay * r * 0.5 + ax * r * 0.7)], fill=color)


def regions(mask, width, height):
    """(outside, outline) of a glyph's ink: `outside` the dark pixels the edge reaches, `outline`
    the ink touching them. Whatever else is the shape's inside: a symbol, a label, its holes."""
    outside, outline = bytearray(width * height), bytearray(width * height)
    todo = [i for i in range(width * height) if (i % width in (0, width - 1) or i // width in (0, height - 1))]
    for grow, fill, ink in ((outside, todo, False), (outline, None, True)):
        if fill is None:
            fill = [i for i in range(width * height) if mask[i] and any(
                outside[j] for j in neighbours(i, width, height))]
            fill += [i for i in range(width * height) if mask[i] and (i % width in (0, width - 1) or i // width in (0, height - 1))]
        stack = [i for i in fill if bool(mask[i]) == ink]
        for i in stack:
            grow[i] = 1
        while stack:
            i = stack.pop()
            for j in neighbours(i, width, height):
                if not grow[j] and bool(mask[j]) == ink:
                    grow[j] = 1
                    stack.append(j)
    return outside, outline


def neighbours(i, width, height):
    x, y = i % width, i // width
    if x:
        yield i - 1
    if x < width - 1:
        yield i + 1
    if y:
        yield i - width
    if y < height - 1:
        yield i + width


def bounds(cells, width):
    xs = [i % width for i, c in enumerate(cells) if c]
    ys = [i // width for i, c in enumerate(cells) if c]
    return (min(xs), min(ys), max(xs) + 1, max(ys) + 1) if xs else None


def draw_text(size, box, text, scale=SCALE):
    """An L mask of `text` filling box (its capitals as tall as the box, narrower if it must be), or
    of an ICONS shape in it; drawn large and reduced."""
    from PIL import Image, ImageDraw, ImageFont
    w, h = size
    layer = Image.new('RGBA', (w * scale, h * scale), (0, 0, 0, 0))
    x0, y0, x1, y1 = (v * scale for v in box)
    if text in ICONS:
        draw_icon(layer, (x0, y0, x1, y1), text, (255, 255, 255))
        return layer.getchannel('A').resize((w, h), Image.LANCZOS)
    draw = ImageDraw.Draw(layer)
    size_px = max(6, int((y1 - y0) * 1.5))
    while True:
        try:
            font = ImageFont.truetype(str(FONT), size_px)
        except OSError:  # the launcher's font is missing: Pillow's own
            font = ImageFont.load_default(size_px)
        left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
        if (right - left <= x1 - x0 and bottom - top <= y1 - y0) or size_px <= 6:
            break
        size_px = int(size_px * 0.96)
    draw.text(((x0 + x1) / 2 - (left + right) / 2, (y0 + y1) / 2 - (top + bottom) / 2), text, font=font,
              fill=(255, 255, 255))
    return layer.getchannel('A').resize((w, h), Image.LANCZOS)


def relabel(glyph, text, kind='auto'):
    """`glyph` (an L image, white ink on black) with its inside replaced by `text`, its outline kept.
    kind: 'auto' (an outlined or a filled shape, from the ink), 'bare' (no outline: the whole
    glyph), 'captioned' (a word above a shape: the word only) or 'redrawn' (an icon instead)."""
    from PIL import Image, ImageChops
    w, h = glyph.size
    mask = bytes(1 if v >= 128 else 0 for v in glyph.tobytes())
    if kind in ('bare', 'redrawn'):
        box = (0, (h - w) / 2, w, (h + w) / 2) if kind == 'redrawn' else (0, h * 0.2, w, h * 0.8)
        return draw_text((w, h), box, text)  # an icon as wide as the plate it replaces; a bare letter
    if kind == 'captioned':
        # From the bottom: blank rows, the shape's rows, blank rows; the caption is above them.
        rows = [any(mask[y * w:(y + 1) * w]) for y in range(h)]
        gap = h
        for ink in (False, True, False):
            while gap and rows[gap - 1] == ink:
                gap -= 1
        out = glyph.copy()
        out.paste(0, (0, 0, w, gap + 1))
        caption = draw_text((w, h), (w * 0.02, 0, w * 0.98, max(1, gap - 1)), text)
        caption.paste(0, (0, gap, w, h))  # the word stays above the shape
        return ImageChops.lighter(out, caption)
    outside, outline = regions(mask, w, h)
    inside = bytes(1 if not outside[i] and not outline[i] else 0 for i in range(w * h))
    shape = sum(1 for i in range(w * h) if not outside[i])
    filled = shape and sum(outline) > 0.6 * shape and sum(1 for i in range(w * h) if inside[i] and not mask[i]) < 0.35 * shape
    area = bounds(inside if not filled else bytes(1 - o for o in outside), w)
    if not area:
        return glyph
    # The new label as tall as the old one (the symbol, or the letters cut out of a filled shape),
    # as wide as the inside allows; centred where the old one was.
    old = bounds(bytes(1 if inside[i] and bool(mask[i]) != filled else 0 for i in range(w * h)), w)
    x0, y0, x1, y1 = area
    if not old:
        old = (x0 + (x1 - x0) / 4, y0 + (y1 - y0) / 4, x1 - (x1 - x0) / 4, y1 - (y1 - y0) / 4)
    if abs((x1 - x0) - (y1 - y0)) < 0.15 * max(x1 - x0, y1 - y0):
        # A ring: the letter centred, the same size in every ring of a set (the PlayStation symbols
        # differ: the circle is larger than the triangle).
        cx, cy, half = (x0 + x1) / 2, (y0 + y1) / 2, (y1 - y0) * RING_LETTER / 2
        box = (cx - half, cy - half, cx + half, cy + half)
    else:  # a box, shield or oval: the label across its inside, as tall as the old one
        half = (x1 - x0) * (0.38 if filled else 0.42)
        box = ((x0 + x1) / 2 - half, old[1], (x0 + x1) / 2 + half, old[3])
    text_mask = draw_text((w, h), box, text)
    region = Image.frombytes('L', (w, h), bytes(255 if (inside[i] or (filled and not outside[i])) else 0 for i in range(w * h)))
    base = glyph.copy()
    if filled:  # the body filled again where its label was cut out; the new label cut out of it
        base = ImageChops.lighter(base, region)
        return ImageChops.subtract(base, ImageChops.multiply(text_mask, region))
    base = ImageChops.subtract(base, region)  # the old symbol gone
    return ImageChops.lighter(base, ImageChops.multiply(text_mask, region))


def redraw_atlas(atlas, rects, style, pad_map):
    """The ps_buttons atlas with every glyph of a known game button relabelled."""
    out = atlas.copy()
    for char, button in CHARACTERS.items():
        if char not in rects:
            continue
        text = label(button, style, pad_map)
        if not text:
            continue
        kind = 'bare' if char in BARE else 'captioned' if char in CAPTIONED else 'redrawn' if char in REDRAWN else 'auto'
        if kind in ('captioned', 'auto') and text in WORDS:
            text = WORDS[text]
        x0, y0, x1, y1 = rects[char]
        out.paste(relabel(atlas.crop((x0, y0, x1, y1)), text, kind), (x0, y0))
    return out


def redraw_tab(image, text):
    """An L1/R1 tab picture (white on transparent) with the tab's label changed."""
    from PIL import Image
    alpha = image.getchannel('A')
    new = relabel(alpha, text)
    white = Image.new('RGBA', image.size, (255, 255, 255, 0))
    white.putalpha(new)
    return white


def redrawn(name, original, style, pad_map, font):
    """The new bytes of an entry this module redraws, or None when it has nothing to change."""
    if name == FONT_ATLAS:
        if font is None:
            return None
        texture = gnf.Texture(original)
        return texture.replaced(redraw_atlas(texture.image(), font_map(font), style, pad_map))
    text = label(TABS[name], style, pad_map)
    if not text:
        return None
    texture = gnf.Texture(original)
    return texture.replaced(redraw_tab(texture.image(), text))


def build(game, out, style, pad_map='', given=None):
    """Runtime file patch lines that show `style`'s buttons in WipEout's prompts; given: the
    archives as psarc.archives takes them. An entry whose new bytes do not fit is left as it is."""
    found = psarc.archives(game, given)

    def make(folder):
        lines = []
        for name, path in found:
            with psarc.Archive(path) as archive:
                font = archive.find(FONT_MAP)
                font = archive.read(font) if font else None
                for entry in (FONT_ATLAS, *TABS):
                    i = archive.find(entry)
                    new = i and redrawn(entry, archive.read(i), style, pad_map, font)
                    if not new:
                        continue
                    changes = archive.repack(i, new)
                    if changes is None:
                        print(f'Glyphs: {name} {entry}: no room for the new picture, left as it is', flush=True)
                        continue
                    lines += psarc.patch_lines(name, changes, folder)
        return lines
    key = [VERSION, style, pad_map, [name for name, _path in found], psarc.stamp(p for _n, p in found)]
    return psarc.cached_lines(Path(out) / 'wipeout_glyphs', key, make)


SWAP_STYLES = ('playstation', 'xbox', 'nintendo')


def swap_line(name, offset, stored, files):
    """A GOW3_GLYPH_SWAP line (src/glyph_swap.h glyph_parse_line): the archive, where the entry's
    packed bytes are (the runtime looks for the texture again after the game reads them) and the
    GNF header length (hex), then style=file for each {style: file}; tab-separated (paths may hold
    spaces)."""
    return '\t'.join([f'/app0/{name}', f'{offset:x}', f'{stored:x}', f'{gnf.HEADER:x}',
                      *(f'{style}={Path(file).resolve()}' for style, file in files.items())]) + '\n'



def variants(game, out, pad_map='', given=None):
    """GOW3_GLYPH_SWAP lines (swap_line, src/runtime_glyphs.c) for the newest copy of the button
    font's atlas and of each tab picture, with a file of each style's texture, the original for
    playstation, written in out/glyph_swap."""
    found = psarc.archives(game, given)

    def make(folder):
        newest = {}
        for name, path in found:
            with psarc.Archive(path) as archive:
                for entry in (FONT_ATLAS, *TABS):
                    if archive.find(entry) is not None:
                        newest[entry] = (name, path)
        lines = []
        for entry, (name, path) in sorted(newest.items()):
            with psarc.Archive(path) as archive:
                i = archive.find(entry)
                original = archive.read(i)
                font = archive.find(FONT_MAP)
                font = archive.read(font) if font is not None else None
                offset = archive.entries[i]['offset']
                stored = archive.stored_size(i)
            files = {}
            for style in SWAP_STYLES:
                data = original if style == 'playstation' else redrawn(entry, original, style, pad_map, font)
                if not data:
                    continue
                files[style] = Path(folder) / f'{Path(name).stem}-{Path(entry).stem}-{style}.gnf'
                files[style].write_bytes(data)
            if len(files) > 1:
                lines.append(swap_line(name, offset, stored, files))
        return lines
    key = [VERSION, pad_map, [name for name, _p in found], psarc.stamp(p for _n, p in found)]
    return psarc.cached_lines(Path(out) / 'glyph_swap', key, make)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True, help='adds its changes to OUT/file_patches.txt')
    parser.add_argument('--style', choices=('xbox', 'nintendo'))
    parser.add_argument('--swap-config', type=Path, help='adds GOW3_GLYPH_SWAP lines (all styles) to this file')
    parser.add_argument('--pad-map', default='', help='GOW3_PAD_MAP')
    psarc.add_archive_option(parser)
    args = parser.parse_args()
    if args.swap_config:
        try:
            lines = variants(args.game, args.out, args.pad_map, args.archive)
        except Exception as error:  # noqa: BLE001  (icons only: whatever fails, the game starts)
            print(f'Glyphs: {error}; the prompts keep the style chosen at start', flush=True)
            lines = []
        with open(args.swap_config, 'a') as f:  # run.py empties it; scripts/xbox_controls.py adds too
            f.writelines(lines)
        print(f'Glyphs: {len(lines)} textures switch while the game runs', flush=True)
        return
    if not args.style:
        parser.error('--style or --swap-config is required')
    try:
        lines = build(args.game, args.out, args.style, args.pad_map, args.archive)
    except Exception as error:  # noqa: BLE001  (icons only: whatever fails, the game starts)
        print(f'Glyphs: {error}; the game keeps its own button icons', flush=True)
        return
    with open(args.out / 'file_patches.txt', 'a') as f:
        f.writelines(lines)
    print(f'Glyphs: {args.style} buttons in the game\'s prompts ({len(lines)} changes; game files untouched)'
          if lines else 'Glyphs: no button font found in the game\'s archives', flush=True)


def preview(fnt, gnf_file, out_png, style='xbox'):
    """The redrawn atlas of a ps_buttons.fnt and .gnf pair, as a PNG (for a look without the game)."""
    texture = gnf.Texture(Path(gnf_file).read_bytes())
    redraw_atlas(texture.image(), font_map(Path(fnt).read_bytes()), style, '').save(out_png)


if __name__ == '__main__':
    main()
