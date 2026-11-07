# SPDX-License-Identifier: GPL-2.0-or-later
"""An Xbox controller on the game's Controls screen, drawn as its DualShock 4 is (white, dark
outlines), when the launcher's controller type is Xbox (GOW3_PAD_STYLE=xbox).

The screen (plugins/frontend/gui/controls_definition_ps4.xml) shows the controller from a sheet
(fe/images/controls_ps3.2x.gnf, 1024x1024): the controller fills U 0..1024 V 270..826, drawn at
x 10.5 y 33 in 768x417 (0.75 of a texel), as the layout's body tag says (body_placement). The
button the selected action uses is drawn again, tinted blue, from its own sprite of the sheet
(<Image name="CrossButton"> ...: x y width Height on screen, U V TxtrWidth TxtrHeight on the
sheet). The Xbox controller (art/xbox_controller.svg) is drawn into the same place; its sprites
go where the sheet has room, and each tag gets their places. The archives are not written: the
changed entries are packed into the bytes the originals take and applied as runtime file patches
(scripts/psarc.py); the sheet keeps the game's format (BC7, scripts/gnf.py).

With --swap-config the screen follows the Controller type row while the game runs: dual_layout()
gives each highlight one place on the screen that covers its button on both controllers, and two
sheets are drawn for that layout, the DualShock 4 (the game's own sheet, its sprites moved) and the
Xbox controller. The launch style's sheet is patched in, and both are written for
src/runtime_glyphs.c, which writes the row's style over the sheet in memory."""
import argparse
import contextlib
import math
from pathlib import Path
import re
import xml.etree.ElementTree as ET

import controller_row
import gnf
import psarc

VERSION = 4  # of the drawing and the patches: a change rebuilds the cached ones
DUAL_VERSION = 2  # of the dual layout and its two sheets
STYLES = ('playstation', 'xbox')
SHARE = 'SelectButton'  # the DualShock 4's Share highlight: the Xbox drawing has no such button
SVG = Path(__file__).resolve().parent / 'art' / 'xbox_controller.svg'
TEXTURES = ('data/fe/images/controls_ps3.2x.gnf', 'data/fe/images/controls_ps3.gnf')
LAYOUT = 'data/plugins/frontend/gui/controls_definition_ps4.xml'
SIZE = 1024
# The shipped layout's values, for the tests and a sheet drawn without a layout; the build reads
# them from the layout (body_placement).
BODY_BOX = (0, 270, 1024, 556)  # U V width height of the controller on the sheet
SCREEN = (10.5, 33.0, 0.75)  # where the layout draws it: x y and texels to screen units
INK, PAPER = (20, 20, 20), (255, 255, 255)
LINE = 2.5  # outline width in texels, as the DualShock 4's
SUPER = 4  # drawn this many times larger, then reduced

# SVG shapes of each button the layout highlights, by the layout's (PlayStation) name. The rest
# of the drawing is the controller's body.
PARTS = {
    'CrossButton': ('path4963', 'path4961', 'path5129'),       # A
    'CircleButton': ('path4969', 'path4971', 'path5138'),      # B
    'SquareButton': ('path4957', 'path4959', 'path5132'),      # X
    'TriangleButton': ('path4965', 'path4967', 'path5135'),    # Y
    'L1Button': ('path4943', 'path5176', 'path5157', 'path5159'),  # LB
    'R1Button': ('path4953', 'path5178', 'path5172', 'path5174'),  # RB
    'L2Button': ('path4945', 'path5162', 'path5164'),          # LT
    'R2Button': ('path4951', 'path5167', 'path5169'),          # RT
    'L3Button': ('path5108', 'path4997', 'path4999', 'path5086', 'path5090', 'path5092', 'path5094', 'path5096'),
    'R3Button': ('path5110', 'path4995', 'path5001', 'path5088', 'path5098', 'path5100', 'path5102', 'path5104'),
}
# Left out: the lighter copies of the face letters (a highlight in colour).
SKIP = {'path5204', 'path5208', 'path5212', 'path5216'}


# ------------------------------------------------------------------------------- SVG

def _multiply(a, b):
    """Affine matrices as SVG writes them (a b c d e f: x' = a x + c y + e)."""
    return (a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1], a[0] * b[2] + a[2] * b[3],
            a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5])


def _transform(text):
    m = (1, 0, 0, 1, 0, 0)
    for name, args in re.findall(r'(\w+)\s*\(([^)]*)\)', text or ''):
        v = [float(x) for x in re.findall(r'-?[\d.]+(?:e-?\d+)?', args)]
        m = _multiply(m, tuple(v) if name == 'matrix' else
                      (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0) if name == 'translate' else
                      (v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0))
    return m


def _subpaths(d):
    """[(points, closed)] of a path's M/L/C/Z commands (absolute or relative), curves flattened."""
    tokens = re.findall(r'[MmLlCcZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?', d)
    i, command, current, start, subs, points = 0, 'M', (0.0, 0.0), (0.0, 0.0), [], []
    while i < len(tokens):
        if tokens[i] in 'MmLlCcZz':
            command, i = tokens[i], i + 1
            if command in 'Zz':
                if points:
                    subs.append((points, True))
                points, current = [], start
                continue
        relative = command.islower()
        count = 6 if command in 'Cc' else 2
        v = [float(t) for t in tokens[i:i + count]]
        i += count
        if relative:
            v = [value + current[k % 2] for k, value in enumerate(v)]
        if command in 'Mm':
            if points:
                subs.append((points, False))
            points, start = [], (v[0], v[1])
            command = 'l' if relative else 'L'
        if command in 'Cc':
            p0, p1, p2, p3 = current, (v[0], v[1]), (v[2], v[3]), (v[4], v[5])
            points += [((1 - t) ** 3 * p0[0] + 3 * (1 - t) ** 2 * t * p1[0] + 3 * (1 - t) * t * t * p2[0] + t ** 3 * p3[0],
                        (1 - t) ** 3 * p0[1] + 3 * (1 - t) ** 2 * t * p1[1] + 3 * (1 - t) * t * t * p2[1] + t ** 3 * p3[1])
                       for t in (k / 24 for k in range(1, 25))]
        else:
            points.append((v[-2], v[-1]))
        current = (v[-2], v[-1])
    if points:
        subs.append((points, False))
    return subs


def load_svg(path=SVG):
    """[(id, outlined, subpaths)] in drawing order; points in the document's units. Shapes without
    a stroke (the letters and labels) are filled with ink, the others white and outlined."""
    shapes = []

    def walk(element, matrix):
        for child in element:
            tag, m = child.tag.split('}')[-1], _multiply(matrix, _transform(child.get('transform')))
            if tag == 'g':
                walk(child, m)
            elif tag == 'path' and child.get('id') not in SKIP:
                style = dict(kv.split(':', 1) for kv in (child.get('style') or '').split(';') if ':' in kv)
                subs = [([(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]) for x, y in pts], closed)
                        for pts, closed in _subpaths(child.get('d'))]
                shapes.append((child.get('id'), style.get('stroke', 'none') != 'none', subs))
    walk(ET.parse(path).getroot(), (1, 0, 0, 1, 0, 0))
    return shapes


# ------------------------------------------------------------------------------- drawing

def _bounds(shapes):
    xs = [x for _i, _o, subs in shapes for pts, _c in subs for x, _y in pts]
    ys = [y for _i, _o, subs in shapes for pts, _c in subs for _x, y in pts]
    return min(xs), min(ys), max(xs), max(ys)


def _paint(image, shapes, place):
    """Draws shapes on an RGBA image at SUPER times the sheet's scale (the whole sheet, or one
    sprite); place maps document points to texels of the image at the sheet's scale. Only what
    falls inside the image is drawn."""
    from PIL import Image, ImageChops, ImageDraw
    k, width = SUPER, max(1, round(LINE * SUPER))
    for _id, outlined, subs in shapes:
        polygons = [[(x * k, y * k) for x, y in map(place, pts)] for pts, _closed in subs]
        polygons = [p for p in polygons if len(p) > 2]
        if not polygons:
            continue
        x0 = max(0, int(min(x for p in polygons for x, _y in p)) - width)
        y0 = max(0, int(min(y for p in polygons for _x, y in p)) - width)
        x1 = min(image.width, int(max(x for p in polygons for x, _y in p)) + width + 1)
        y1 = min(image.height, int(max(y for p in polygons for _x, y in p)) + width + 1)
        if x1 <= x0 or y1 <= y0:
            continue
        mask = Image.new('L', (x1 - x0, y1 - y0), 0)
        for p in polygons:  # even-odd: a letter's counter is a hole
            layer = Image.new('L', mask.size, 0)
            ImageDraw.Draw(layer).polygon([(x - x0, y - y0) for x, y in p], fill=255)
            mask = ImageChops.logical_xor(mask.convert('1'), layer.convert('1')).convert('L')
        area = image.crop((x0, y0, x1, y1))
        area.paste(PAPER + (255,) if outlined else INK + (255,), (0, 0), mask)
        if outlined:
            draw = ImageDraw.Draw(area)
            for p in polygons:
                draw.line([(x - x0, y - y0) for x, y in p + p[:1]], fill=INK + (255,), width=width, joint='curve')
        image.paste(area, (x0, y0))


def layout(shapes, box=BODY_BOX):
    """(place, sprites): place maps document points to texels of the controller on the sheet, in
    `box` (U V width height); sprites are {part: (u, v, w, h, x, y)}, a part's sprite on the sheet
    and the texel where its corner is on the controller. Sprites go in rows in the room above and
    below the controller."""
    x0, y0, x1, y1 = _bounds(shapes)
    bu, bv, bw, bh = box
    pad = math.ceil(LINE) + 2  # around each sprite, inside the controller's room too
    scale = min((bw - 4 * pad) / (x1 - x0), (bh - 2 * pad - 2) / (y1 - y0))
    ox, oy = bu + (bw - (x1 - x0) * scale) / 2 - x0 * scale, bv + (bh - (y1 - y0) * scale) / 2 - y0 * scale
    place = lambda p: (p[0] * scale + ox, p[1] * scale + oy)  # noqa: E731
    by_id = {s[0]: s for s in shapes}
    sprites, rows = {}, [[2, 2, bv - 2], [2, bv + bh + 2, SIZE - 2]]  # x, y, bottom of the room
    for part, ids in sorted(PARTS.items(), key=lambda item: item[0]):
        px0, py0, px1, py1 = _bounds([by_id[i] for i in ids])
        (cx0, cy0), (cx1, cy1) = place((px0, py0)), place((px1, py1))
        x, y = math.floor(cx0) - pad, math.floor(cy0) - pad
        w, h = math.ceil(cx1) + pad - x, math.ceil(cy1) + pad - y
        row = next((r for r in rows if r[0] + w <= SIZE - 2 and r[1] + h <= r[2]), None)
        if row is None:
            raise ValueError(f'no room on the sheet for {part}')
        sprites[part] = (row[0], row[1], w, h, x, y)
        row[0] += w + 8  # apart, so the smaller mip levels keep them apart too
    return place, sprites


def occluders(shapes, ids):
    """The shapes drawn over a part (after it, not its own), which hide some of it."""
    first = min(n for n, s in enumerate(shapes) if s[0] in ids)
    return [s for s in shapes[first + 1:] if s[0] not in ids]


def draw_sheet(shapes=None, box=BODY_BOX):
    """(sheet, sprites): the 1024x1024 RGBA sheet with the controller in `box` (U V width height,
    body_placement()'s first four), and layout()'s sprites."""
    from PIL import Image, ImageChops
    shapes = shapes or load_svg()
    place, sprites = layout(shapes, box)
    big = Image.new('RGBA', (SIZE * SUPER, SIZE * SUPER), PAPER + (0,))
    _paint(big, shapes, place)
    for part, (u, v, w, h, x, y) in sprites.items():
        ids = set(PARTS[part])
        # Painted on a canvas of the sprite's size, its corner at 0, 0, and pasted in its place.
        shift = lambda p, x=x, y=y: (place(p)[0] - x, place(p)[1] - y)  # noqa: E731
        sprite = Image.new('RGBA', (w * SUPER, h * SUPER), PAPER + (0,))
        _paint(sprite, [s for s in shapes if s[0] in ids], shift)
        # What covers the part on the controller is cut out, so the highlight lies under it.
        cover = Image.new('RGBA', sprite.size, PAPER + (0,))
        _paint(cover, occluders(shapes, ids), shift)
        alpha = ImageChops.multiply(sprite.getchannel('A'), ImageChops.invert(cover.getchannel('A')))
        sprite.putalpha(alpha)
        big.paste(sprite, (u * SUPER, v * SUPER))
    return big.resize((SIZE, SIZE), Image.LANCZOS), sprites


# ------------------------------------------------------------------------------- layout file

def _number(value, decimals):
    text = f'{value:.{decimals}f}'
    return text.rstrip('0').rstrip('.') if '.' in text else text


def _rewrite(span, values):
    """`span` (name="value" attributes) with these values, padded with spaces to its length;
    fewer decimals when they would not fit."""
    for decimals in (2, 1, 0):
        text = span
        for name, value in values.items():
            text = re.sub(rf'\b{name}="[^"]*"', f'{name}="{_number(value, decimals)}"', text)
        if len(text) <= len(span):
            return text + ' ' * (len(span) - len(text))
    raise ValueError(f'{values} do not fit in {span!r}')


def body_placement(text):
    """(u, v, tw, th, x, y, kx) of the layout's body tag (the first; both screens repeat it): the
    controller's U V width height on the sheet, where it is drawn on the screen and the screen
    units a texel takes. ValueError when the tag is missing, scales width and height differently
    or is not as wide as the sheet."""
    # The first unnamed image drawn from the sheet: others come before it (the panel's dot.gtf).
    for body in re.finditer(r'<Image\b(?![^>]*\bname=)[^>]*>\s*<Values\b((?:[^>"]|"[^"]*")*)>', text):
        attributes = {name.lower(): value for name, value in re.findall(r'(\w+)="([^"]*)"', body.group(1))}
        if 'controls_ps3' in attributes.get('src', '').lower():
            break
    else:
        raise ValueError("the layout's controller body tag was not found")
    try:
        u, v, tw, th = (int(attributes[name]) for name in ('u', 'v', 'txtrwidth', 'txtrheight'))
        x, y, width, height = (float(attributes[name]) for name in ('x', 'y', 'width', 'height'))
    except (KeyError, ValueError):
        raise ValueError(f"the layout's controller body tag is not as expected: {body.group(0)!r}") from None
    kx = width / tw
    if abs(height / th - kx) > 0.01:
        raise ValueError(f'the layout draws the controller {kx:g} wide and {height / th:g} high a texel')
    if tw != SIZE or u + tw > SIZE or v + th > SIZE:
        raise ValueError(f'the layout draws U {u} V {v} {tw}x{th} of the sheet as the controller')
    return u, v, tw, th, x, y, kx


def relayout(xml, sprites):
    """The layout file with each highlight at its Xbox button (sprites from layout(), drawn where
    the body tag says); the Share highlight, which the Xbox drawing has no sprite for, moved off
    the screen. The same length; its tabs (indentation only) become spaces, which pack smaller, so
    the changed numbers fit."""
    bu, bv, _tw, _th, x0, y0, k = body_placement(xml.decode('latin1'))
    places = {SHARE: ({'x': -999, 'y': -999}, {})}  # its size kept (a zero-sized quad is degenerate)
    for name, (u, v, w, h, x, y) in sprites.items():
        places[name] = ({'x': x0 + (x - bu) * k, 'y': y0 + (y - bv) * k, 'width': w * k, 'Height': h * k,
                         'U': u, 'V': v}, {'TxtrWidth': w, 'TxtrHeight': h})
    return _retag(xml, places, set(PARTS) | {SHARE})


HIGHLIGHT = re.compile(r'(<Image name="(\w+)"[^>]*>\s*<Values\s+)(x="[^"]*"\s+y="[^"]*"\s+width="[^"]*"\s+'
                       r'Height="[^"]*"\s+U="[^"]*"\s+V="[^"]*")([^\n]*?)(TxtrWidth="[^"]*"\s+TxtrHeight="[^"]*")')


def _retag(xml, places, required):
    """The layout file with the highlight tags of `places` ({name: (screen and sheet values, sheet
    size values)}) rewritten, the same length; ValueError unless every `required` tag is there.
    Its tabs (indentation only) become spaces, which pack smaller, so the changed numbers fit."""
    text = xml.decode('latin1')
    if re.search(r'=\s*"[^"]*\t', text):  # a value starts after =" and ends at the next quote
        raise ValueError('a tab inside an attribute value; the layout is not rewritten')
    text = text.replace('\t', ' ')
    seen = set()

    def change(match):
        name = match.group(2)
        if name not in places:
            return match.group(0)
        seen.add(name)
        where, size = places[name]
        try:
            return match.group(1) + _rewrite(match.group(3), where) + match.group(4) + _rewrite(match.group(5), size)
        except ValueError:
            # A longer number in one part (a 2-digit TxtrWidth becoming 3): the whole tag's room,
            # what the other numbers' fewer decimals free, is used.
            return match.group(1) + _rewrite(match.group(3) + match.group(4) + match.group(5), {**where, **size})
    new = HIGHLIGHT.sub(change, text)
    if not required <= seen:
        raise ValueError(f'the layout lacks {sorted(required - seen)}')
    return new.encode('latin1')


# ------------------------------------------------------------------------------- both controllers

def highlights(text):
    """{name: (x, y, width, height, u, v, tw, th)} of the layout's highlight tags: where each is on
    the screen and the sprite of the sheet it shows. ValueError when the screens' copies differ."""
    found = {}
    for match in HIGHLIGHT.finditer(text):
        values = dict(re.findall(r'(\w+)="([^"]*)"', match.group(3) + ' ' + match.group(5)))
        try:
            place = tuple(float(values[name]) for name in ('x', 'y', 'width', 'Height', 'U', 'V', 'TxtrWidth', 'TxtrHeight'))
        except (KeyError, ValueError):
            raise ValueError(f'the highlight {match.group(2)} is not as expected') from None
        if found.setdefault(match.group(2), place) != place:
            raise ValueError(f'the Controls screens place the highlight {match.group(2)} differently')
    return found


def _shelves(sizes, rooms, gap=8):
    """{name: (u, v)} placing {name: (w, h)} in rows inside `rooms` [(top, bottom)] of the sheet,
    the tallest first; None when they do not all fit."""
    places, rows = {}, [[2, top, top, bottom] for top, bottom in rooms]  # x, row top, row bottom, room end
    for name, (w, h) in sorted(sizes.items(), key=lambda item: (-item[1][1], item[0])):
        for row in rows:
            if row[0] + w > SIZE - 2:  # a new row below this one, if the room has space
                row[:3] = [2, row[2] + gap, row[2] + gap]
            if row[0] + w <= SIZE - 2 and row[1] + h <= row[3]:
                places[name] = (row[0], row[1])
                row[0] += w + gap
                row[2] = max(row[2], row[1] + h)
                break
        else:
            return None
    return places


def dual_layout(text, sprites):
    """{name: (screen, slot, boxes)} for a layout that works with both sheets. screen (x, y, width,
    height) covers the highlight's place on both controllers: the layout's (DualShock 4) and the Xbox
    sprite's (layout()'s sprites, placed as relayout() does). slot (u, v, w, h) is its sprite on both
    sheets, and boxes {style: (dx, dy, w, h)} where in the slot each controller's button goes (the
    Xbox has no Share). The slots fill the room above and below the controller; they are drawn
    smaller when they would not fit at the controller's own scale. ValueError when they never fit."""
    bu, bv, bw, bh, x0, y0, k = body_placement(text)
    ps = highlights(text)
    missing = (set(PARTS) | {SHARE}) - set(ps)
    if missing:
        raise ValueError(f'the layout lacks {sorted(missing)}')
    rects = {}
    for name in sorted(set(PARTS) | {SHARE}):
        own = {'playstation': ps[name][:4]}
        if name in sprites:
            _u, _v, w, h, x, y = sprites[name]
            own['xbox'] = (x0 + (x - bu) * k, y0 + (y - bv) * k, w * k, h * k)
        left = min(r[0] for r in own.values())
        top = min(r[1] for r in own.values())
        right = max(r[0] + r[2] for r in own.values())
        bottom = max(r[1] + r[3] for r in own.values())
        rects[name] = (left, top, right - left, bottom - top, own)
    rooms = [(2, bv - 2), (bv + bh + 2, SIZE - 2)]
    for share in (1, 0.85, 0.7, 0.6, 0.5, 0.4):
        density = share / k  # texels a screen unit
        sizes = {name: (math.ceil(r[2] * density), math.ceil(r[3] * density)) for name, r in rects.items()}
        places = _shelves(sizes, rooms)
        if places is not None:
            break
    else:
        raise ValueError('no room on the sheet for the highlights of both controllers')
    result = {}
    for name, (left, top, _w, _h, own) in rects.items():
        (u, v), (w, h) = places[name], sizes[name]
        boxes = {}
        for style, (x, y, rw, rh) in own.items():
            dx, dy = round((x - left) * density), round((y - top) * density)
            boxes[style] = (dx, dy, max(1, min(w - dx, round(rw * density))), max(1, min(h - dy, round(rh * density))))
        result[name] = ((left, top, w / density, h / density), (u, v, w, h), boxes)
    return result


def relayout_dual(xml, slots):
    """The layout with each highlight at its dual_layout() place and slot, the same length."""
    places = {name: ({'x': x, 'y': y, 'width': w, 'Height': h, 'U': u, 'V': v}, {'TxtrWidth': tw, 'TxtrHeight': th})
              for name, ((x, y, w, h), (u, v, tw, th), _boxes) in slots.items()}
    return _retag(xml, places, set(places))


def dual(text, original, shapes=None):
    """(slots, {style: 1024x1024 RGBA sheet}) for the layout file's text: dual_layout()'s slots, and
    each controller's sheet: its controller in the layout's box (the game's DualShock 4 from
    `original`, the game's sheet as an image, or the Xbox drawing) and each highlight's sprite in its
    slot, at that controller's box (scaled when the slots are drawn smaller)."""
    from PIL import Image
    placement = body_placement(text)
    bu, bv, bw, bh = placement[:4]
    if original.size != (SIZE, SIZE):
        original = original.resize((SIZE, SIZE), Image.LANCZOS)
    original = original.convert('RGBA')
    drawn, sprites = draw_sheet(shapes, box=(bu, bv, bw, bh))
    slots = dual_layout(text, sprites)
    ps = highlights(text)
    pieces = {'playstation': lambda name: original.crop(_box(ps[name][4:])),
              'xbox': lambda name: drawn.crop(_box(sprites[name][:4]))}
    sheets = {}
    for style, piece in pieces.items():
        source = original if style == 'playstation' else drawn
        sheet = Image.new('RGBA', (SIZE, SIZE), PAPER + (0,))
        sheet.paste(source.crop((bu, bv, bu + bw, bv + bh)), (bu, bv))
        for name, (_screen, (u, v, _w, _h), boxes) in slots.items():
            if style not in boxes:
                continue
            dx, dy, w, h = boxes[style]
            sprite = piece(name)
            if sprite.size != (w, h):
                sprite = sprite.resize((w, h), Image.LANCZOS)
            sheet.paste(sprite, (u + dx, v + dy))
        sheets[style] = sheet
    return slots, sheets


def _box(place):
    """The crop box of (u, v, w, h)."""
    u, v, w, h = (int(round(n)) for n in place)
    return u, v, u + w, v + h


# ------------------------------------------------------------------------------- patches

def changes(game, given=None, row=True):
    """{archive: [(offset, original, new)]} that draw the Xbox controller (and with `row`, the
    Controller type rows). The game reads the newest archive's copy of a file (as
    scripts/menu_patches.py's data09 change), and the picture and the layout go together: an
    archive changes only when all its copies fit. An archive that
    holds the newest copy of a file must fit, or ValueError and nothing changes; an older one that
    does not is left as it is."""
    with contextlib.ExitStack() as opened:
        holders = {name: [] for name in (*TEXTURES, LAYOUT)}  # file -> [(archive name, Archive, entry)]
        for archive_name, path in psarc.archives(game, given):
            archive = opened.enter_context(psarc.Archive(path))
            for name in holders:
                i = archive.find(name)
                if i is not None:
                    holders[name].append((archive_name, archive, i))
        return _changes(holders, row)


def _changes(holders, row=True):
    """changes() of the archives' copies, found and opened: {file: [(archive name, Archive, entry)]}."""
    if not holders[LAYOUT] or not any(holders[name] for name in TEXTURES):
        raise ValueError("the Controls screen's picture or layout was not found")
    # Every layout copy first: the sheet is drawn where they place the controller.
    layouts = {archive_name: archive.read(i) for archive_name, archive, i in holders[LAYOUT]}
    placements = {body_placement(xml.decode('latin1')) for xml in layouts.values()}
    if len(placements) > 1:
        raise ValueError(f'the copies of the layout place the controller differently: {sorted(placements)}')
    sheet, sprites = draw_sheet(box=placements.pop()[:4])
    newest = {copies[-1][0] for copies in holders.values() if copies}
    packed = {}  # archive -> {file: its changes, or None when they do not fit}
    encoded = {}  # (length, header) -> the sheet in a texture of that shape
    for name, copies in holders.items():
        for archive_name, archive, i in copies:
            if name == LAYOUT:
                # The Controller type rows (scripts/controller_row.py) go in the same bytes: two
                # patches of one entry would each check the other's original bytes and fail.
                xml = layouts[archive_name]
                new = relayout(controller_row.add_row(xml) if row else xml, sprites)
            else:
                # The new texture depends only on the original's length and header (its size and
                # levels), so copies that agree in both are encoded once.
                original = archive.read(i)
                key = (len(original), original[:gnf.HEADER])
                if key not in encoded:
                    encoded[key] = gnf.Texture(original).replaced(sheet)
                new = encoded[key]
            packed.setdefault(archive_name, {})[name] = parts = archive.repack(i, new)
            if parts is None and archive_name in newest:
                raise ValueError(f'{archive_name} {name}: the Xbox version does not fit in its place')
    result = {}
    for archive_name, files in packed.items():
        missing = [name for name, parts in files.items() if parts is None]
        if missing:
            print(f'Xbox controller: {archive_name}: no room for {", ".join(missing)}; '
                  'its copies are left as they are', flush=True)
            continue
        result[archive_name] = [change for parts in files.values() for change in parts]
    return result


def build(game, out, given=None, row=True):
    """Lines for out/file_patches.txt, cached in out/xbox_controls until the drawing, the rows or the
    archives change. given: the archives as psarc.archives takes them; row: the Controller type
    rows too (scripts/controller_row.py)."""
    found = psarc.archives(game, given)

    def make(folder):
        return [line for name, parts in changes(game, given, row).items() for line in psarc.patch_lines(name, parts, folder)]
    key = [VERSION, controller_row.VERSION, row, psarc.stamp([SVG]), [name for name, _p in found],
           psarc.stamp(p for _n, p in found)]
    return psarc.cached_lines(Path(out) / 'xbox_controls', key, make)


def dual_changes(game, given=None, row=True):
    """({style: {archive: [(offset, original, new)]}}, swaps) for the dual layout (and with `row`,
    the Controller type rows) and each style's sheet. swaps: [(archive, offset, packed length,
    {style: texture bytes})], the newest copy of each sheet texture. An archive changes only when
    all its copies fit for both styles; the newest copy of a file must fit, or ValueError."""
    with contextlib.ExitStack() as opened:
        holders = {name: [] for name in (*TEXTURES, LAYOUT)}
        for archive_name, path in psarc.archives(game, given):
            archive = opened.enter_context(psarc.Archive(path))
            for name in holders:
                i = archive.find(name)
                if i is not None:
                    holders[name].append((archive_name, archive, i))
        return _dual_changes(holders, row)


def _dual_changes(holders, row=True):
    from PIL import Image
    if not holders[LAYOUT] or not any(holders[name] for name in TEXTURES):
        raise ValueError("the Controls screen's picture or layout was not found")
    layouts = {archive_name: archive.read(i) for archive_name, archive, i in holders[LAYOUT]}
    if len({body_placement(xml.decode('latin1')) for xml in layouts.values()}) > 1:
        raise ValueError('the copies of the layout place the controller differently')
    # The game's newest layout and its largest sheet: what the DualShock 4 sheet is made from.
    text = layouts[holders[LAYOUT][-1][0]].decode('latin1')
    _n, archive, i = next(holders[name][-1] for name in TEXTURES if holders[name])
    slots, sheets = dual(text, gnf.Texture(archive.read(i)).image())
    encoded = {}  # (style, length, header) -> the style's sheet in a texture of that shape
    newest = {copies[-1][0] for copies in holders.values() if copies}
    packed = {}  # archive -> {(file, style): its changes, or None when they do not fit}
    swaps = []
    for name, copies in holders.items():
        for archive_name, archive, i in copies:
            if name == LAYOUT:
                xml = layouts[archive_name]
                new = relayout_dual(controller_row.add_row(xml) if row else xml, slots)
                variants = dict.fromkeys(STYLES, new)
            else:
                original = archive.read(i)
                texture = gnf.Texture(original)
                variants = {}
                for style in STYLES:
                    key = (style, len(original), original[:gnf.HEADER])
                    if key not in encoded:
                        sheet = sheets[style]
                        if sheet.size != (texture.width, texture.height):
                            sheet = sheet.resize((texture.width, texture.height), Image.LANCZOS)
                        encoded[key] = texture.replaced(sheet)
                    variants[style] = encoded[key]
                offset = archive.entries[i]['offset']
                # Two names of one entry (the 2x and 1x sheets can share their bytes): one texture.
                if (archive_name, archive, i) == copies[-1] and all((a, o) != (archive_name, offset) for a, o, _l, _v in swaps):
                    swaps.append((archive_name, offset, sum(stored for stored, _length in archive.blocks(i)), variants))
            repacked = {}
            for style, data in variants.items():
                if data not in repacked:
                    repacked[data] = archive.repack(i, data)
                packed.setdefault(archive_name, {})[(name, style)] = parts = repacked[data]
                if parts is None and archive_name in newest:
                    raise ValueError(f'{archive_name} {name}: the {style} version does not fit in its place')
    result = {style: {} for style in STYLES}
    for archive_name, files in packed.items():
        missing = sorted({name for (name, _style), parts in files.items() if parts is None})
        if missing:
            print(f'Controller picture: {archive_name}: no room for {", ".join(missing)}; '
                  'its copies are left as they are', flush=True)
            continue
        for (_name, style), parts in files.items():
            kept = result[style].setdefault(archive_name, [])
            kept.extend(change for change in parts if change not in kept)  # one entry under two names
    return result, swaps


def build_dual(game, out, style, given=None, row=True):
    """(file patch lines of `style`, GOW3_GLYPH_SWAP lines of the sheets), cached in
    out/controls_dual (both styles at once) until the drawing, the rows or the archives change."""
    found = psarc.archives(game, given)

    def make(folder):
        changed, swaps = dual_changes(game, given, row)
        lines = []
        for each in STYLES:
            (folder / each).mkdir()
            lines += [f'{each}\t{line}' for name, parts in changed[each].items()
                      for line in psarc.patch_lines(name, parts, folder / each)]
        (folder / 'swap').mkdir()
        for archive_name, offset, length, variants in swaps:
            files = []
            for each, data in variants.items():
                file = folder / 'swap' / f'{Path(archive_name).stem}-{offset:x}-{each}.gnf'
                file.write_bytes(data)
                files.append(f'{each}={file.resolve()}')
            lines.append('swap\t' + '\t'.join([f'/app0/{archive_name}', f'{offset:x}', f'{length:x}',
                                                f'{gnf.HEADER:x}', *files]) + '\n')
        return lines
    key = [DUAL_VERSION, VERSION, controller_row.VERSION, row, psarc.stamp([SVG]), [name for name, _p in found],
           psarc.stamp(p for _n, p in found)]
    lines = psarc.cached_lines(Path(out) / 'controls_dual', key, make)
    split = [line.split('\t', 1) for line in lines]
    return [line for kind, line in split if kind == style], [line for kind, line in split if kind == 'swap']


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True, help='adds its changes to OUT/file_patches.txt')
    parser.add_argument('--no-row', action='store_true',
                        help='without the Controller type rows (the save cannot take their settings)')
    parser.add_argument('--style', choices=STYLES, default='xbox', help='the controller shown at start')
    parser.add_argument('--swap-config', type=Path,
                        help='both controllers: the picture follows the Controller type row while the game runs; '
                             'the sheets are added to this GOW3_GLYPH_SWAP file')
    psarc.add_archive_option(parser)
    args = parser.parse_args()
    row = not args.no_row
    if args.swap_config:
        try:
            lines, swaps = build_dual(args.game, args.out, args.style, args.archive, row)
        except Exception as error:  # noqa: BLE001  (a picture only: whatever fails, the game starts)
            print(f'Controller picture: {error}; it does not follow the Controller type row', flush=True)
        else:
            with open(args.out / 'file_patches.txt', 'a') as f:
                f.writelines(lines)
            with open(args.swap_config, 'a') as f:
                f.writelines(swaps)
            print(f'Controller picture: {args.style} at start, follows the Controller type row '
                  f'({len(lines)} changes, {len(swaps)} sheets; game files untouched)', flush=True)
            return
        if args.style == 'playstation':  # the game's own picture, with the rows (as before)
            if not row:
                return
            try:
                lines = controller_row.build(args.game, args.out, args.archive)
            except Exception as error:  # noqa: BLE001  (a menu row: whatever fails, the game starts)
                print(f'Controller type row: {error}; the Controls screens stay as they are', flush=True)
                return
            with open(args.out / 'file_patches.txt', 'a') as f:
                f.writelines(lines)
            print(f'Controller type row: on the Controls screens ({len(lines)} changes; game files untouched)', flush=True)
            return
    try:
        lines = build(args.game, args.out, args.archive, row=row)
    except Exception as error:  # noqa: BLE001  (a picture only: whatever fails, the game starts)
        print(f'Xbox controller: {error}; the Controls screen keeps the DualShock 4', flush=True)
        return
    with open(args.out / 'file_patches.txt', 'a') as f:
        f.writelines(lines)
    print(f'Xbox controller: on the Controls screen ({len(lines)} changes; game files untouched)', flush=True)


if __name__ == '__main__':
    main()
