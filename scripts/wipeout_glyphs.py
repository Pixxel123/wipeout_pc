# SPDX-License-Identifier: GPL-2.0-or-later
"""Controller button glyphs for WipEout's own prompts, in the game's style.

WipEout writes its button prompts with a font of PlayStation buttons (fe/fonts/ps_buttons: the
.fnt maps a character to a rectangle of the .gnf atlas) and shows the L1/R1 tabs of its menus as
pictures (fe/images/but_l1_hd, but_r1_hd). They are entries of data00.psarc and data08.psarc,
stored uncompressed. build() redraws them with the buttons of an Xbox or Nintendo controller:
each glyph keeps its own outline (ring, box, trigger shield, oval, pill) and only its symbol or
label changes, white on black as the game draws them, or black on a filled shape. The labels
follow GOW3_PAD_MAP and the controller's letters, as scripts/glyphs.py does for God of War III.

The archives are never written: build() writes the new entries to out/wipeout_glyphs/ and returns
runtime file patches (src/runtime_file.c), "guest offset length fnv @host-file", which apply only
while the archive still holds the bytes they were made from.
"""
import struct
from pathlib import Path

import glyphs
import gnf
import psarc

VERSION = 1  # of the drawing: a change redraws the cached entries
ARCHIVES = ('data00.psarc', 'data08.psarc')
FONT_MAP = 'data/fe/fonts/ps_buttons.fnt'
FONT_ATLAS = 'data/fe/fonts/ps_buttons.gnf'
TABS = {'data/fe/images/but_l1_hd.gnf': 'l1', 'data/fe/images/but_r1_hd.gnf': 'r1'}
SCALE = 4  # labels drawn this many times larger, then reduced
RING_LETTER = 0.5  # a face button's letter: this share of the inside of its ring

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


def label(button, style, faces, pad_map):
    """The label (a letter, a word or a glyphs.ICONS name) of a game button on this controller."""
    host = glyphs.pad_buttons(pad_map).get(button)
    if not host:
        return None
    return glyphs.glyph(host, style, faces)[1]


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
    of a glyphs.ICONS shape in it; drawn large and reduced."""
    from PIL import Image, ImageDraw, ImageFont
    w, h = size
    layer = Image.new('RGBA', (w * scale, h * scale), (0, 0, 0, 0))
    x0, y0, x1, y1 = (v * scale for v in box)
    if text in glyphs.ICONS:
        glyphs.draw_label(layer, (x0, y0, x1, y1), text, (255, 255, 255))
        return layer.getchannel('A').resize((w, h), Image.LANCZOS)
    draw = ImageDraw.Draw(layer)
    size_px = max(6, int((y1 - y0) * 1.5))
    while True:
        try:
            font = ImageFont.truetype(str(glyphs.FONT), size_px)
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


def redraw_atlas(atlas, rects, style, faces, pad_map):
    """The ps_buttons atlas with every glyph of a known game button relabelled."""
    out = atlas.copy()
    for char, button in CHARACTERS.items():
        if char not in rects:
            continue
        text = label(button, style, faces, pad_map)
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


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def entries(path):
    """{name (lower case): (offset, bytes)} of the stored entries build() changes in one archive."""
    archive = psarc.Archive(path)
    found = {}
    for name in (FONT_MAP, FONT_ATLAS, *TABS):
        i = archive.names.get(name)
        if i is None or archive.compressed(i):
            continue
        found[name] = (archive.entries[i]['offset'], archive.read(i))
    return found


def build(game, out, style, faces=None, pad_map=''):
    """Runtime file patch lines that show `style`'s buttons in WipEout's prompts; [] when there is
    nothing to change (a PlayStation controller, the keyboard, or no archive with the font)."""
    import io
    import json
    from PIL import Image
    target = Path(out) / 'wipeout_glyphs'
    if style not in ('xbox', 'nintendo'):
        import shutil
        shutil.rmtree(target, ignore_errors=True)
        return []
    lines = []
    for archive_name in ARCHIVES:
        path = next((p for p in Path(game).iterdir() if p.name.lower() == archive_name), None)
        if not path:
            continue
        stat = path.stat()
        key = {'version': VERSION, 'style': style, 'faces': faces, 'pad_map': pad_map,
               'archive': [stat.st_size, int(stat.st_mtime)]}
        folder = target / archive_name
        cache = folder / 'patches.json'
        try:
            cached = json.loads(cache.read_text())
            if cached['key'] == key and all(Path(p).is_file() for *_x, p in cached['patches']):
                lines += [f'/app0/{archive_name} {o:x} {n:x} {h:x} @{p}' for o, n, h, p in cached['patches']]
                continue
        except (OSError, ValueError, KeyError, TypeError):
            pass
        found = entries(path)
        changed = {}
        if FONT_MAP in found and FONT_ATLAS in found:
            texture = gnf.Texture(found[FONT_ATLAS][1])
            atlas = redraw_atlas(texture.image(), font_map(found[FONT_MAP][1]), style, faces, pad_map)
            changed[FONT_ATLAS] = texture.replaced(atlas)
        for name, button in TABS.items():
            text = label(button, style, faces, pad_map)
            if name in found and text:
                texture = gnf.Texture(found[name][1])
                changed[name] = texture.replaced(redraw_tab(texture.image(), text))
        folder.mkdir(parents=True, exist_ok=True)
        patches = []
        for name, data in changed.items():
            offset, original = found[name]
            file = folder / Path(name).name
            file.write_bytes(data)
            patches.append((offset, len(data), fnv1a(original), str(file.resolve())))
        cache.write_text(json.dumps({'key': key, 'patches': patches}))
        lines += [f'/app0/{archive_name} {o:x} {n:x} {h:x} @{p}' for o, n, h, p in patches]
    return lines


def preview(fnt, gnf_file, out_png, style='xbox'):
    """The redrawn atlas of a ps_buttons.fnt and .gnf pair, as a PNG (for a look without the game)."""
    from pathlib import Path as P
    texture = gnf.Texture(P(gnf_file).read_bytes())
    redraw_atlas(texture.image(), font_map(P(fnt).read_bytes()), style, None, '').save(out_png)


if __name__ == '__main__':
    import sys
    preview(*sys.argv[1:])
