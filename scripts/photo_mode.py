# SPDX-License-Identifier: GPL-2.0-or-later
"""Xbox controllers in photo mode's help, in place of the game's DualShock pictograms.

Photo mode's help shows the stick an action uses (Rotate and Pan the left, Zoom the right) as a
controller with that stick red, a sprite of data/fe/images/photo_mode.2x.gnf (512x512: the right
stick's at U 8..138 V 427..508, the left stick's at U 238..368; photo_mode.gnf is the same at half
size) and, for Fury, of photo_mode_fury.gnf (256x256: U 4..68 and 119..183, V 214..253). The Xbox
controllers (scripts/art/xbox_photo_*.svg, from scripts/art/photo_pads.py) are drawn into those
sprites; the rest of each sheet stays. The archives are not written: the sheets are packed into the
bytes the originals take and applied as runtime file patches (scripts/psarc.py), and with
--swap-config both versions go to the GOW3_GLYPH_SWAP file, so the help follows the Controller type
row while the game runs (src/runtime_glyphs.c)."""
import argparse
import contextlib
import hashlib
from pathlib import Path

import gnf
import psarc
import wipeout_glyphs
import xbox_controls

VERSION = 2  # of the drawing and the patches: a change rebuilds the cached ones
ART = Path(__file__).resolve().parent / 'art'
# Each sheet: its sprites (U, V, width, height, the stick drawn) in texels of a sheet `space` wide.
SHEETS = {
    'data/fe/images/photo_mode.2x.gnf': (512, [(8, 427, 131, 82, 'right'), (238, 427, 131, 82, 'left')], ''),
    # At 1x the 2x drawing's lines are one texel and its parts run together: the small one instead.
    'data/fe/images/photo_mode.gnf': (512, [(8, 427, 131, 82, 'right'), (238, 427, 131, 82, 'left')], 'small_'),
    'data/fe/images/photo_mode_fury.gnf': (256, [(4, 214, 65, 40, 'right'), (119, 214, 65, 40, 'left')], 'fury_'),
}
SUPER = 4  # drawn this many times larger, then reduced
MARGIN = 1.5  # texels left clear around a drawing: the help draws its sprites filtered, so lines
              # on a sprite's edge show as cut strokes and smear into what is next to them
REMNANT = 3   # texels around a sprite looked through for the game's own pictogram's outer pixels


def _color(text):
    return tuple(int(text[i:i + 2], 16) for i in (1, 3, 5)) + (255,) if text and text.startswith('#') else None


def draw(svg, width, height, margin=MARGIN):
    """The SVG's paths on a transparent width x height image, its viewBox centred in it `margin`
    texels from every edge: a path's fill and stroke as its inline style says (what photo_pads.py
    writes)."""
    from PIL import Image, ImageChops, ImageDraw
    import xml.etree.ElementTree as ET
    vx, vy, vw, vh = (float(v) for v in ET.parse(svg).getroot().get('viewBox').split())
    k = SUPER * min((width - 2 * margin) / vw, (height - 2 * margin) / vh)
    ox, oy = (width * SUPER - vw * k) / 2, (height * SUPER - vh * k) / 2
    big = Image.new('RGBA', (width * SUPER, height * SUPER), (0, 0, 0, 0))
    for _ident, style, subs in xbox_controls.svg_paths(svg):
        polygons = [[((x - vx) * k + ox, (y - vy) * k + oy) for x, y in pts] for pts, _closed in subs]
        polygons = [p for p in polygons if len(p) > 2]
        fill, stroke = _color(style.get('fill')), _color(style.get('stroke'))
        if not polygons or not (fill or stroke):
            continue
        if fill:
            mask = Image.new('1', big.size, 0)
            for p in polygons:  # even-odd: a ring's inside is a hole
                layer = Image.new('1', big.size, 0)
                ImageDraw.Draw(layer).polygon(p, fill=1)
                mask = ImageChops.logical_xor(mask, layer)
            big.paste(fill, (0, 0), mask)
        if stroke:
            line = max(1, round(float(style.get('stroke-width', 1)) * k))
            drawer = ImageDraw.Draw(big)
            for p in polygons:
                drawer.line(p + p[:1], fill=stroke, width=line, joint='curve')
    return gnf.reduce(big, width, height)


def _line_art(image, box):
    """Whether `box` of the sheet holds a pixel of the pictograms' white line art."""
    return any(a > 128 and min(r, g, b) > 128 for r, g, b, a in image.crop(box).getdata())


def _extent(image, box, most):
    """`box` grown, up to `most` texels a side, over the rows and columns just outside it that hold
    the game's pictogram's line art: what the notes' rectangle (and its rounding at 1x) leaves out."""
    x0, y0, x1, y1 = box
    for _ in range(most):
        grown = (x0 - (x0 > 0 and _line_art(image, (x0 - 1, y0, x0, y1))),
                 y0 - (y0 > 0 and _line_art(image, (x0, y0 - 1, x1, y0))),
                 x1 + (x1 < image.width and _line_art(image, (x1, y0, x1 + 1, y1))),
                 y1 + (y1 < image.height and _line_art(image, (x0, y1, x1, y1 + 1))))
        if grown == (x0, y0, x1, y1):
            break
        x0, y0, x1, y1 = grown
    return x0, y0, x1, y1


def redraw(image, name):
    """The sheet image of `name` (SHEETS) with its sprites redrawn as Xbox controllers: the game's
    pictogram cleared, its outer pixels just outside the rectangle too, and the drawing put in the
    rectangle with a clear margin."""
    from PIL import Image
    space, sprites, prefix = SHEETS[name]
    k = image.width / space
    out = image.convert('RGBA')
    for u, v, w, h, stick in sprites:
        box = (round(u * k), round(v * k), round((u + w) * k), round((v + h) * k))
        cleared = _extent(out, box, max(1, round(REMNANT * k)))
        out.paste(Image.new('RGBA', (cleared[2] - cleared[0], cleared[3] - cleared[1]), (0, 0, 0, 0)), cleared[:2])
        sprite = draw(ART / f'xbox_photo_{prefix}{stick}_stick.svg', box[2] - box[0], box[3] - box[1])
        out.paste(sprite, box[:2], sprite)
    return out


def changes(game, given=None):
    """({'xbox': {archive: [(offset, original, new)]}}, swaps): the sheets' changes as
    xbox_controls.pack gives them, and swaps [(archive, offset, stored size, {style: bytes})] of the
    newest copy of each sheet, the original for playstation. ValueError when no sheet is found."""
    with contextlib.ExitStack() as opened:
        found = xbox_controls.holders(game, given, opened, tuple(SHEETS))
        if not any(found.values()):
            raise ValueError("photo mode's help sheets were not found")
        encoded, swaps = {}, []

        def variants(name, archive_name, archive, i):
            original = archive.read(i)
            if original not in encoded:
                texture = gnf.Texture(original)
                encoded[original] = texture.replaced(redraw(texture.image(), name))
            if (archive_name, archive, i) == found[name][-1]:
                swaps.append((archive_name, archive.entries[i]['offset'], archive.stored_size(i),
                              {'playstation': original, 'xbox': encoded[original]}))
            return {'xbox': encoded[original]}
        return xbox_controls.pack(found, variants, 'Photo mode'), swaps


def build(game, out, style, given=None):
    """(file patch lines of `style`: the Xbox sheets, or none for the game's own; GOW3_GLYPH_SWAP
    lines of the sheets), cached in out/photo_mode until the drawings or the archives change."""
    found = psarc.archives(game, given)
    svgs = sorted(ART.glob('xbox_photo_*.svg'))

    def make(folder):
        changed, swaps = changes(game, given)
        lines = [f'xbox\t{line}' for name, parts in changed.get('xbox', {}).items()
                 for line in psarc.patch_lines(name, parts, folder)]
        for archive_name, offset, stored, variants in swaps:
            files = {each: folder / f'{Path(archive_name).stem}-{offset:x}-{each}.gnf' for each in variants}
            for each, file in files.items():
                file.write_bytes(variants[each])
            files['nintendo'] = files['playstation']  # no Nintendo drawing: the runtime shows the PlayStation one
            lines.append('swap\t' + wipeout_glyphs.swap_line(archive_name, offset, stored, files))
        return lines
    key = [VERSION, psarc.stamp(svgs), [name for name, _p in found], psarc.stamp(p for _n, p in found)]
    lines = psarc.cached_lines(Path(out) / 'photo_mode', key, make)
    split = [line.split('\t', 1) for line in lines]
    return [line for kind, line in split if kind == style], [line for kind, line in split if kind == 'swap']


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True, help='adds its changes to OUT/file_patches.txt')
    parser.add_argument('--style', choices=('playstation', 'xbox', 'nintendo'), default='xbox',
                        help='the controller shown at start (Nintendo: the PlayStation one)')
    parser.add_argument('--swap-config', type=Path,
                        help='the help follows the Controller type row while the game runs; '
                             'the sheets are added to this GOW3_GLYPH_SWAP file')
    psarc.add_archive_option(parser)
    args = parser.parse_args()
    try:
        lines, swaps = build(args.game, args.out, args.style, args.archive)
    except Exception as error:  # noqa: BLE001  (pictures only: whatever fails, the game starts)
        print(f'Photo mode: {error}; its help keeps the DualShock pictograms', flush=True)
        return
    with open(args.out / 'file_patches.txt', 'a') as f:
        f.writelines(lines)
    if args.swap_config:
        with open(args.swap_config, 'a') as f:
            f.writelines(swaps)
    # The drawings' digest names which SVGs the sheets came from (a stale AppImage or cache shows).
    drawings = hashlib.md5(b''.join(p.read_bytes() for p in sorted(ART.glob('xbox_photo_*.svg')))).hexdigest()[:8]
    print(f'Photo mode: {args.style} controllers in its help ({len(lines)} changes'
          + (f', {len(swaps)} sheets follow the Controller type row' if args.swap_config else '')
          + f'; drawings {drawings}; game files untouched)', flush=True)


if __name__ == '__main__':
    main()
