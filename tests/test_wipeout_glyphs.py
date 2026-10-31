import struct
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

from paths import ROOT  # noqa: F401  (scripts on the path)
import gnf
import psarc
import wipeout_glyphs
from psarc_fixtures import apply, archive, texture


def ring(size=96):
    """A face button glyph like the game's: a white ring around a white triangle, on black."""
    image = Image.new('L', (size, size), 0)
    draw = ImageDraw.Draw(image)
    draw.ellipse((2, 2, size - 3, size - 3), outline=255, width=size // 12)
    draw.polygon([(size / 2, size * 0.3), (size * 0.3, size * 0.66), (size * 0.7, size * 0.66)], outline=255, width=size // 16)
    return image


def oval(w=140, h=90):
    """A filled L3 glyph: a white oval with its label cut out in black."""
    image = Image.new('L', (w, h), 0)
    draw = ImageDraw.Draw(image)
    draw.ellipse((2, 2, w - 3, h - 3), fill=255)
    draw.rectangle((w * 0.35, h * 0.3, w * 0.45, h * 0.7), fill=0)
    draw.rectangle((w * 0.55, h * 0.3, w * 0.65, h * 0.7), fill=0)
    return image


def font(records):
    """A ps_buttons.fnt: the header the game's has, then 18-byte records from 0x240."""
    data = bytearray(0x240) + b''.join(struct.pack('<HBBHHHHhhh', ord(c), x1 - x0, y1 - y0, x0, x1, y0, y1, 0, -1, -1)
                                       for c, (x0, y0, x1, y1) in records.items())
    return bytes(data[:5].replace(bytes(5), b'\x01FNTX') + data[5:] + bytes(18))


class WipeoutGlyphTests(unittest.TestCase):
    def test_font_map(self):
        rects = {'0': (459, 8, 555, 115), 'A': (9, 240, 105, 347)}
        self.assertEqual(wipeout_glyphs.font_map(font(rects)), rects)

    def test_every_face_button_set_is_complete(self):
        faces = [c for c, b in wipeout_glyphs.CHARACTERS.items() if b in wipeout_glyphs.FACES]
        self.assertEqual(len(faces) % 4, 0)
        self.assertEqual(wipeout_glyphs.label('cross', 'xbox', ''), 'A')
        self.assertEqual(wipeout_glyphs.label('l2', 'xbox', ''), 'LT')
        self.assertEqual(wipeout_glyphs.label('cross', 'xbox', 'cross=b'), 'B')  # GOW3_PAD_MAP
        self.assertEqual(wipeout_glyphs.label('cross', 'nintendo', ''), 'B')  # the south button
        self.assertEqual(wipeout_glyphs.label('options', 'xbox', ''), 'menu')  # drawn as a symbol
        self.assertEqual(wipeout_glyphs.label('touchpad_right', 'nintendo', ''), 'minus')
        self.assertEqual(wipeout_glyphs.label('cross', 'xbox', 'cross=dpup'), 'dpup')  # an arrow head

    def test_a_ring_keeps_its_ring_and_gets_a_letter(self):
        glyph = ring()
        new = wipeout_glyphs.relabel(glyph, 'minus')  # a bar across the middle: the corners stay clear
        self.assertEqual(new.size, glyph.size)
        self.assertEqual(new.getpixel((48, 4)), glyph.getpixel((48, 4)))  # on the ring: unchanged
        self.assertEqual(new.getpixel((48, 91)), glyph.getpixel((48, 91)))
        self.assertGreater(new.getpixel((48, 48)), 128)  # the bar...
        self.assertLess(new.getpixel((30, 63)), 64)  # ...and the triangle's corners gone
        self.assertLess(new.getpixel((48, 30)), 64)
        a = wipeout_glyphs.relabel(glyph, 'A')
        self.assertGreater(sum(1 for v in a.crop((30, 30, 66, 66)).getdata() if v >= 128), 150)

    def test_a_filled_shape_is_filled_again_and_the_label_cut_out(self):
        glyph = oval()
        new = wipeout_glyphs.relabel(glyph, 'LS')
        self.assertEqual(new.getpixel((20, 45)), 255)  # the body
        self.assertEqual(new.getpixel((0, 0)), 0)  # outside
        dark = sum(1 for v in new.crop((30, 20, 110, 70)).getdata() if v < 128)
        self.assertGreater(dark, 200)  # the new label, cut out of the body

    def test_a_caption_changes_its_word_only(self):
        glyph = Image.new('L', (76, 82), 0)
        draw = ImageDraw.Draw(glyph)
        draw.rectangle((5, 2, 70, 14), fill=255)  # stands in for the word OPTIONS
        draw.rounded_rectangle((25, 30, 50, 76), 12, fill=255)  # the pill, blank rows below it
        new = wipeout_glyphs.relabel(glyph, 'MENU', 'captioned')
        self.assertEqual(new.crop((0, 20, 76, 82)).tobytes(), glyph.crop((0, 20, 76, 82)).tobytes())
        self.assertNotEqual(new.crop((0, 0, 76, 18)).tobytes(), glyph.crop((0, 0, 76, 18)).tobytes())

    def test_build_writes_patches_for_the_stored_entries(self):
        atlas = Image.new('L', (256, 128), 0)
        atlas.paste(ring(), (8, 8))
        atlas.paste(oval(), (110, 8))
        fnt = font({'0': (8, 8, 104, 104), 'e': (110, 8, 250, 98)})
        gnf_file = gnf.Texture(texture(1, 256, 128, 256, 1)).replaced(atlas)
        tab = gnf.Texture(texture(41, 88, 38, 128, 7))
        tab_image = Image.new('RGBA', (88, 38), (255, 255, 255, 0))
        ImageDraw.Draw(tab_image).rectangle((1, 1, 86, 36), outline=(255, 255, 255, 255), width=3)
        tab_file = tab.replaced(tab_image)
        with tempfile.TemporaryDirectory() as folder:
            game, out = Path(folder) / 'game', Path(folder) / 'out'
            game.mkdir()
            # As WipEout's: a NUL-separated manifest, the TOC in the order of the MD5s of the names
            # (upper case, with a leading slash), the entries stored but for one.
            archive(game / 'data00.psarc', {'Data/fe/fonts/ps_buttons.fnt': fnt,
                                            'Data/fe/fonts/ps_buttons.gnf': gnf_file,
                                            'Data/fe/images/but_l1_hd.gnf': tab_file,
                                            'Data/fe/images/but_r1_hd.gnf': tab_file},
                    separator=b'\0', prefix='', hashed=lambda n: ('/' + n).upper(), md5_order=True,
                    packed={'Data/fe/images/but_r1_hd.gnf'})
            # The atlas and the stored L1 tab change, each repacked in its own place. The compressed R1
            # tab is almost empty and packs smaller than its new picture can: it stays as it is.
            with redirect_stdout(StringIO()) as printed:
                lines = wipeout_glyphs.build(game, out, 'xbox')
            self.assertIn('but_r1_hd.gnf: no room for the new picture, left as it is', printed.getvalue())
            self.assertTrue(all(line.startswith('/app0/data00.psarc ') for line in lines))
            patched = apply((game / 'data00.psarc').read_bytes(), lines)
            (Path(folder) / 'patched.psarc').write_bytes(patched)
            a = psarc.Archive(Path(folder) / 'patched.psarc')
            self.addCleanup(a.close)
            drawn = gnf.Texture(a.read(a.find('data/fe/fonts/ps_buttons.gnf'))).image()
            self.assertNotEqual(ImageChops.difference(drawn, atlas).getbbox(), None)
            self.assertEqual(a.read(a.find('data/fe/fonts/ps_buttons.fnt')), fnt)
            self.assertNotEqual(a.read(a.find('data/fe/images/but_l1_hd.gnf')), tab_file)
            self.assertEqual(a.read(a.find('data/fe/images/but_r1_hd.gnf')), tab_file)
            # Cached: the same lines; another mapping draws again.
            self.assertEqual(wipeout_glyphs.build(game, out, 'xbox'), lines)
            remapped = wipeout_glyphs.build(game, out, 'xbox', 'triangle=a')
            self.assertNotEqual(remapped, lines)

    def test_no_archive_no_patches(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(wipeout_glyphs.build(folder, Path(folder) / 'out', 'xbox'), [])


if __name__ == '__main__':
    unittest.main()
