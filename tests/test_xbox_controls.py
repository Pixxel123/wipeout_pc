import io
import json
import os
import re
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import gnf
import psarc
import xbox_controls as xc
from psarc_fixtures import apply, archive, texture


# As in the real file: CRLF, tabs for indentation, two screens with the same tags, and the panel's
# background (an unnamed image too) before the controller.
PANEL_TAG = ('\t\t<Image enabletransition="0.5" delay="0.25">\r\n\t\t\t<Values transitiontype="vwipe" x="5" y="25" '
             'color="FEGlobals->HD_LightGrey" width="790" height="502" txtrwidth="790" txtrheight="502" '
             'src="Data\\FE\\Images\\dot.gtf"></Values>\r\n\t\t</Image>\r\n')
BODY_TAG = ('\t\t<Image enabletransition="0.5" delay="0.7">\r\n\t\t\t<Values transitiontype="vwipe" AutoLoad="true" '
            'x="{x}" y="{y}" width="768.00" Height="417.00" U="0" V="270" txtrwidth="1024" txtrheight="556" '
            'src="Data\\FE\\Images\\controls_PS3.2x.gtf">\r\n\t\t\t</Values>\r\n\t\t</Image>\r\n')
LAYOUT_TAG = ('\t\t<Image name="{name}" delay="0.7" transition="0">\r\n\t\t\t<Values x="529.50" y="219.75" width="67.50" '
              'Height="57.75" U="692" V="889" color="FEGlobals->HD_Blue" TxtrWidth="170" TxtrHeight="115" '
              'src="Data\\FE\\Images\\controls_PS3.2x.gtf"></Values>\r\n\t\t</Image>\r\n')


def layout(names=(*xc.PARTS, 'SelectButton'), body=('10.50', '33.00')):
    """The layout file: two screens, each with the body tag at `body` (x and y as written; None
    leaves it out) and a highlight tag for each name."""
    screen = ('<Screen>\r\n\t<Item OffsetX="960" OffsetY="120">\r\n' + PANEL_TAG
              + (BODY_TAG.format(x=body[0], y=body[1]) if body else '')
              + ''.join(LAYOUT_TAG.format(name=n) for n in names) + '\t</Item>\r\n</Screen>\r\n')
    return (2 * screen).encode('latin1')


def read(path, name):
    with psarc.Archive(path) as a:
        return a.read(a.find(name))


def fast_replaced(texture, image, seed=None):
    """Stands in for gnf.Texture.replaced, which takes many seconds on a 1024x1024 BC7 sheet (its
    own tests are in test_gnf.py): the header kept, the data noise the size of the original's
    (seeded by its length unless told), which packs no smaller than the game's own textures do."""
    import random
    rng = random.Random(len(texture.data) if seed is None else seed)
    return texture.data[:gnf.HEADER] + bytes(rng.getrandbits(8) for _ in range(len(texture.data) - gnf.HEADER))


class NumberTest(unittest.TestCase):
    def test_number_rewrites_keep_the_length(self):
        span = 'x="108.00" y="78.00" width="127.50" Height="60.00" U="130" V="150"'
        new = xc._rewrite(span, {'x': 98.25, 'y': 102.5, 'width': 135, 'Height': 61.5, 'U': 2, 'V': 828})
        self.assertEqual(len(new), len(span))
        self.assertEqual(new.split(), 'x="98.25" y="102.5" width="135" Height="61.5" U="2" V="828"'.split())
        squeezed = xc._rewrite('x="1.5" y="2.5"', {'x': 123.25, 'y': 456.75})
        self.assertEqual(squeezed, 'x="123" y="457"')


class LayoutTest(unittest.TestCase):
    sprites = {name: (2 + 100 * n, 828, 80, 70, 300 + 10 * n, 500) for n, name in enumerate(xc.PARTS)}

    def test_highlights_move_to_their_sprites_and_share_disappears(self):
        old = layout()
        new = xc.relayout(old, self.sprites)
        self.assertEqual(len(new), len(old))
        text = new.decode('latin1')
        cross = re.search(r'name="CrossButton".*?<Values ([^\r]*)', text, re.S).group(1)
        u, v, w, h, x, y = self.sprites['CrossButton']
        self.assertIn(f'x="{10.5 + x * 0.75:g}"', cross)
        self.assertIn(f'y="{33 + (y - 270) * 0.75:g}"', cross)
        self.assertIn(f'U="{u}"', cross)
        self.assertIn(f'TxtrWidth="{w}"', cross)
        # Share keeps its size (the fake tag's) and goes off the screen.
        select = re.search(r'name="SelectButton".*?<Values ([^\r]*)', text, re.S).group(1)
        self.assertEqual(select.split()[:4], ['x="-999"', 'y="-999"', 'width="67.50"', 'Height="57.75"'])
        self.assertIn('color="FEGlobals->HD_Blue"', select)
        # Both screens change; the other lines only lose their tabs.
        self.assertEqual(text.count('x="-999"'), 2)
        old_lines, new_lines = old.decode('latin1').replace('\t', ' ').split('\r\n'), text.split('\r\n')
        self.assertEqual([a == b for a, b in zip(old_lines, new_lines)].count(False), 2 * 11)

    def test_layout_has_row_and_xbox_highlights(self):
        import controller_row
        from test_controller_row import P1, P2
        first, second, _rest = layout().split(b'</Screen>\r\n')
        both = first + P1 + b'</Screen>\r\n' + second + P2 + b'</Screen>\r\n'
        new = xc.relayout(controller_row.add_row(both), self.sprites)
        self.assertEqual(len(new), len(both))
        self.assertIn(b'name="Pad Style P1"', new)
        self.assertIn(b'name="Pad Style P2"', new)
        self.assertIn(b'x="-999"', new)  # the Share highlight moved off the screen, as before

    def test_a_layout_without_every_highlight_is_refused(self):
        with self.assertRaises(ValueError):
            xc.relayout(layout(('CrossButton',)), self.sprites)

    def test_the_body_tag_places_the_controller(self):
        placement = xc.body_placement(layout().decode('latin1'))
        self.assertEqual(placement, (0, 270, 1024, 556, 10.5, 33.0, 0.75))
        self.assertEqual((placement[:4], placement[4:]), (xc.BODY_BOX, xc.SCREEN))

    def test_a_body_moved_on_the_screen_moves_every_highlight_with_it(self):
        def places(xml):
            new = xc.relayout(xml, self.sprites)
            self.assertEqual(len(new), len(xml))
            found = re.findall(r'name="(\w+)"[^>]*>\s*<Values x="([^"]*)"\s+y="([^"]*)"', new.decode('latin1'))
            return {name: (float(x), float(y)) for name, x, y in found if name in self.sprites}
        before, after = places(layout()), places(layout(body=('20.50', '43.00')))
        self.assertEqual(set(before), set(self.sprites))
        for name in self.sprites:
            self.assertAlmostEqual(after[name][0] - before[name][0], 10, msg=name)
            self.assertAlmostEqual(after[name][1] - before[name][1], 10, msg=name)

    def test_a_layout_without_its_body_tag_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'body tag'):
            xc.body_placement(layout(body=None).decode('latin1'))
        with self.assertRaisesRegex(ValueError, 'body tag'):
            xc.relayout(layout(body=None), self.sprites)
        stretched = layout().replace(b'Height="417.00" U="0"', b'Height="500.00" U="0"')
        with self.assertRaisesRegex(ValueError, 'wide and'):
            xc.body_placement(stretched.decode('latin1'))

    def test_a_tab_inside_a_value_is_refused(self):
        tabbed = layout().replace(b'controls_PS3.2x.gtf"></Values>', b'controls\tPS3.2x.gtf"></Values>', 1)
        self.assertNotEqual(tabbed, layout())
        with self.assertRaisesRegex(ValueError, 'tab inside'):
            xc.relayout(tabbed, self.sprites)
        # The indentation's tabs are not refused.
        self.assertIn(b'\t', layout())
        self.assertNotIn(b'\t', xc.relayout(layout(), self.sprites))


class DrawingTest(unittest.TestCase):
    def test_sheet_and_sprites(self):
        sheet, sprites = xc.draw_sheet()
        self.assertEqual(sheet.size, (1024, 1024))
        self.assertEqual(set(sprites), set(xc.PARTS))
        bu, bv, bw, bh = xc.BODY_BOX
        boxes = []
        for name, (u, v, w, h, x, y) in sprites.items():
            self.assertTrue(0 <= u and u + w <= 1024 and 0 <= v and v + h <= 1024, name)
            self.assertTrue(v + h <= bv or v >= bv + bh, f'{name} overlaps the controller')
            self.assertTrue(bu <= x and x + w <= bu + bw and bv <= y and y + h <= bv + bh, name)
            self.assertGreater(sheet.crop((u, v, u + w, v + h)).getchannel('A').getextrema()[1], 0, name)
            for other in boxes:
                self.assertFalse(u < other[2] and other[0] < u + w and v < other[3] and other[1] < v + h, name)
            boxes.append((u, v, u + w, v + h))
        # The controller is drawn; the room around it is clear.
        self.assertEqual(sheet.getpixel((512, 600))[3], 255)
        self.assertEqual(sheet.getpixel((1020, 500))[3], 0)


TEXTURE = 'data/fe/images/controls_ps3.2x.gnf'


class BuildTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(gnf.Texture, 'replaced', fast_replaced)
        patcher.start()
        self.addCleanup(patcher.stop)

    def game(self, folder):
        game = Path(folder) / 'game'
        game.mkdir()
        archive(game / 'data08.psarc', {'data/fe/images/controls_ps3.2x.gnf': texture(41, 1024, 1024, fill=None),
                                        'data/fe/images/other.gnf': bytes(5000)})
        archive(game / 'data09.psarc', {'data/plugins/frontend/gui/controls_definition_ps4.xml': layout()}, packed=False)
        return game

    def test_build_changes_the_picture_and_the_layout(self):
        with tempfile.TemporaryDirectory() as folder:
            game, out = self.game(folder), Path(folder) / 'out'
            out.mkdir()
            (out / 'file_patches.txt').write_text('/app0/data09.psarc 10 00 01\n')  # menu_patches.py's
            with redirect_stdout(io.StringIO()) as printed, mock.patch.object(sys, 'argv', ['x', str(game), '--out', str(out)]):
                xc.main()
            self.assertIn('Xbox controller: on the Controls screen', printed.getvalue())
            lines = (out / 'file_patches.txt').read_text().splitlines()
            self.assertEqual(lines[0], '/app0/data09.psarc 10 00 01')
            for name in ('data08.psarc', 'data09.psarc'):
                mine = [line for line in lines[1:] if line.startswith(f'/app0/{name} ')]
                self.assertTrue(mine, name)
                patched = Path(folder) / name
                patched.write_bytes(apply((game / name).read_bytes(), mine))
            drawn = read(Path(folder) / 'data08.psarc', TEXTURE)
            self.assertEqual(drawn, fast_replaced(gnf.Texture(read(game / 'data08.psarc', TEXTURE)), None))
            self.assertEqual(read(Path(folder) / 'data08.psarc', 'data/fe/images/other.gnf'), bytes(5000))
            xml = read(Path(folder) / 'data09.psarc', xc.LAYOUT)
            self.assertEqual(len(xml), len(layout()))
            self.assertIn(b'x="-999"', xml)
            # Cached: the same lines, nothing drawn again.
            with mock.patch.object(xc, 'draw_sheet', side_effect=AssertionError('drawn again')):
                self.assertEqual(xc.build(game, out), [line + '\n' for line in lines[1:]])

    def test_the_layout_carries_the_rows_unless_told_not_to(self):
        from test_controller_row import P1, P2
        first, second, _rest = layout().split(b'</Screen>\r\n')
        sliders = first + P1 + b'</Screen>\r\n' + second + P2 + b'</Screen>\r\n'
        with tempfile.TemporaryDirectory() as folder:
            game, out = self.game(folder), Path(folder) / 'out'
            out.mkdir()
            archive(game / 'data09.psarc', {xc.LAYOUT: sliders}, packed=False)

            def patched_layout(*options):
                # The same out folder each time: the cache must tell the two apart.
                (out / 'file_patches.txt').write_text('')
                argv = ['x', str(game), '--out', str(out), *options]
                with redirect_stdout(io.StringIO()) as printed, mock.patch.object(sys, 'argv', argv):
                    xc.main()
                self.assertIn('Xbox controller: on the Controls screen', printed.getvalue())
                mine = [line for line in (out / 'file_patches.txt').read_text().splitlines()
                        if line.startswith('/app0/data09.psarc ')]
                self.assertTrue(mine)
                (Path(folder) / 'data09.psarc').write_bytes(apply((game / 'data09.psarc').read_bytes(), mine))
                xml = read(Path(folder) / 'data09.psarc', xc.LAYOUT)
                self.assertEqual(len(xml), len(sliders))
                self.assertEqual(xml.count(b'x="-999"'), 2)  # the highlights moved either way
                return xml
            with_rows = patched_layout()
            self.assertIn(b'name="Pad Style P1"', with_rows)
            self.assertIn(b'name="Pad Style P2"', with_rows)
            self.assertNotIn(b'aSlider', with_rows)
            without = patched_layout('--no-row')
            self.assertNotIn(b'Pad Style', without)
            self.assertEqual(without.count(b'<aSlider name="Thrust Sensitivity"'), 2)
            self.assertEqual(patched_layout(), with_rows)

    def test_an_older_copy_without_room_is_left_but_the_newest_must_fit(self):
        with tempfile.TemporaryDirectory() as folder:
            game, out = self.game(folder), Path(folder) / 'out'
            out.mkdir()
            # data00's packed copy: identical tags pack tighter than the changed ones.
            archive(game / 'data00.psarc', {xc.LAYOUT: layout()})
            with redirect_stdout(io.StringIO()) as printed:
                parts = xc.changes(game)
            self.assertIn(f'data00.psarc: no room for {xc.LAYOUT}', printed.getvalue())
            self.assertEqual(sorted(parts), ['data08.psarc', 'data09.psarc'])
            # A layout that fits (stored raw) and a picture that does not (zeros, which pack to
            # almost nothing): neither changes, so the archive's picture and highlights agree.
            archive(game / 'data00.psarc', {xc.LAYOUT: layout(), TEXTURE: texture(41, 1024, 1024)},
                    packed={TEXTURE})
            with psarc.Archive(game / 'data00.psarc') as a:
                i = a.find(xc.LAYOUT)
                self.assertIsNotNone(a.repack(i, xc.relayout(a.read(i), LayoutTest.sprites)))
            with redirect_stdout(io.StringIO()) as printed:
                parts = xc.changes(game)
            self.assertIn(f'data00.psarc: no room for {TEXTURE}; its copies are left as they are', printed.getvalue())
            self.assertEqual(sorted(parts), ['data08.psarc', 'data09.psarc'])
            (game / 'data09.psarc').unlink()  # now data00's is the newest copy
            with redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, 'does not fit'):
                xc.changes(game)

    def test_the_sheet_is_drawn_where_the_layout_places_the_controller(self):
        with tempfile.TemporaryDirectory() as folder:
            game = self.game(folder)
            with mock.patch.object(xc, 'draw_sheet', wraps=xc.draw_sheet) as draw:
                xc.changes(game)
            self.assertEqual(draw.call_args.kwargs['box'], xc.BODY_BOX)
            # Two copies that disagree: refused before anything is drawn.
            archive(game / 'data00.psarc', {xc.LAYOUT: layout(body=('20.50', '33.00'))}, packed=False)
            with mock.patch.object(xc, 'draw_sheet', side_effect=AssertionError('drawn')), \
                    self.assertRaisesRegex(ValueError, 'differently'):
                xc.changes(game)

    def test_nothing_changes_without_the_layout(self):
        with tempfile.TemporaryDirectory() as folder:
            game, out = self.game(folder), Path(folder) / 'out'
            out.mkdir()
            (game / 'data09.psarc').unlink()
            with redirect_stdout(io.StringIO()) as printed, mock.patch.object(sys, 'argv', ['x', str(game), '--out', str(out)]):
                xc.main()
            self.assertIn('keeps the DualShock 4', printed.getvalue())
            self.assertFalse((out / 'file_patches.txt').exists())


# Where a DualShock 4's highlights could be on the screen (x y width height), all different.
PS_PLACES = {'CrossButton': (570, 223, 45, 45), 'CircleButton': (615, 183, 45, 45), 'SquareButton': (525, 183, 45, 45),
             'TriangleButton': (570, 143, 45, 45), 'L1Button': (110, 73, 110, 30), 'R1Button': (570, 73, 110, 30),
             'L2Button': (120, 33, 90, 40), 'R2Button': (580, 33, 90, 40), 'L3Button': (230, 263, 90, 90),
             'R3Button': (470, 263, 90, 90), 'SelectButton': (240, 123, 30, 40)}
# Their sprites on the game's sheet (U V TxtrWidth TxtrHeight): above and below the controller.
PS_SPRITES = {name: (10 + 90 * (n % 10), 10 if n < 10 else 900, 60, 60) for n, name in enumerate(sorted(PS_PLACES))}


def ps_layout(body=('10.50', '33.00'), places=PS_PLACES):
    """layout() with each highlight at its own place and sprite, as the game's file has them."""
    text = layout(body=body).decode('latin1')
    old = 'x="529.50" y="219.75" width="67.50" Height="57.75" U="692" V="889"'

    def place(match):
        x, y, w, h = places[match.group(1)]
        u, v, tw, th = PS_SPRITES[match.group(1)]
        return (match.group(0).replace(old, f'x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" Height="{h:.2f}" U="{u}" V="{v}"')
                .replace('TxtrWidth="170" TxtrHeight="115"', f'TxtrWidth="{tw}" TxtrHeight="{th}"'))
    return re.sub(r'name="(\w+)"[^\r]*\r\n[^\r]*', place, text).encode('latin1')


def ps_sheet():
    """The game's sheet, as an image: the controller grey, each sprite its own colour."""
    from PIL import Image, ImageDraw
    sheet = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
    draw = ImageDraw.Draw(sheet)
    draw.rectangle((0, 270, 1023, 825), fill=(128, 128, 128, 255))
    for n, (u, v, w, h) in enumerate(PS_SPRITES.values()):
        draw.rectangle((u, v, u + w - 1, v + h - 1), fill=(20 * n, 255, 0, 255))
    return sheet


def image_replaced(texture, image):
    """fast_replaced seeded by the image, so each style's sheet has its own noise."""
    import hashlib
    return fast_replaced(texture, image, hashlib.md5(image.tobytes()).digest())


class DualTest(unittest.TestCase):
    """Both controllers on one layout: the picture follows the Controller type row (--swap-config)."""

    def test_each_highlight_covers_its_button_on_both_controllers(self):
        text = ps_layout().decode('latin1')
        _sheet, sprites = xc.draw_sheet()
        slots = xc.dual_layout(text, sprites)
        self.assertEqual(set(slots), set(xc.PARTS) | {'SelectButton'})
        bu, bv, bw, bh, x0, y0, k = xc.body_placement(text)
        taken = []
        for name, ((x, y, w, h), (u, v, tw, th), boxes) in slots.items():
            # The place covers the DualShock 4's highlight and the Xbox one.
            px, py, pw, ph = PS_PLACES[name]
            self.assertTrue(x <= px + 1e-6 and y <= py + 1e-6 and px + pw <= x + w + 1e-6 and py + ph <= y + h + 1e-6, name)
            if name in sprites:
                _u, _v, sw, sh, sx, sy = sprites[name]
                xx, xy = x0 + (sx - bu) * k, y0 + (sy - bv) * k
                self.assertTrue(x <= xx + 1e-6 and y <= xy + 1e-6 and xx + sw * k <= x + w + 1e-6 and xy + sh * k <= y + h + 1e-6, name)
                self.assertEqual(set(boxes), {'playstation', 'xbox'})
            else:
                self.assertEqual(set(boxes), {'playstation'})  # Share: the Xbox has none
            # Texels and screen units as the controller's (k), the slot clear of it and the others.
            self.assertAlmostEqual(w / tw, k, places=6)
            self.assertAlmostEqual(h / th, k, places=6)
            self.assertTrue(v + th <= bv or v >= bv + bh, f'{name} overlaps the controller')
            self.assertTrue(0 <= u and u + tw <= 1024 and 0 <= v and v + th <= 1024, name)
            for other in taken:
                self.assertFalse(u < other[2] and other[0] < u + tw and v < other[3] and other[1] < v + th, name)
            taken.append((u, v, u + tw, v + th))
            for dx, dy, bw_, bh_ in boxes.values():
                self.assertTrue(0 <= dx and dx + bw_ <= tw and 0 <= dy and dy + bh_ <= th, name)

    def test_highlights_drawn_smaller_when_they_do_not_fit_at_full_size(self):
        # Every DualShock 4 highlight far from its Xbox button: big places, which fit only smaller.
        far = {name: (5, 380, 40, 40) if n % 2 else (740, 40, 30, 30) for n, name in enumerate(sorted(PS_PLACES))}
        text = ps_layout(places=far).decode('latin1')
        _sheet, sprites = xc.draw_sheet()
        slots = xc.dual_layout(text, sprites)
        k = xc.body_placement(text)[6]
        ratios = {round(w / tw, 6) for (_x, _y, w, _h), (_u, _v, tw, _th), _b in slots.values()}
        self.assertEqual(len(ratios), 1)
        self.assertGreater(ratios.pop(), k)  # fewer texels a screen unit

    def test_the_two_sheets(self):
        text = ps_layout().decode('latin1')
        slots, sheets = xc.dual(text, ps_sheet())
        self.assertEqual(set(sheets), {'playstation', 'xbox'})
        ps, xbox = sheets['playstation'], sheets['xbox']
        # The game's controller on one, the Xbox drawing on the other, in the same place.
        self.assertEqual(ps.getpixel((512, 600)), (128, 128, 128, 255))
        self.assertEqual(xbox.getpixel((512, 600))[3], 255)
        self.assertNotEqual(xbox.getpixel((512, 600)), (128, 128, 128, 255))
        # The game's sprites moved into their slots; nothing left where they were.
        for n, name in enumerate(PS_SPRITES):
            (_x, _y, _w, _h), (u, v, _tw, _th), boxes = slots[name]
            dx, dy, w, h = boxes['playstation']
            self.assertEqual(ps.getpixel((u + dx + w // 2, v + dy + h // 2)), (20 * n, 255, 0, 255), name)
        for u, v, w, h in PS_SPRITES.values():
            centre = (u + w // 2, v + h // 2)
            if not any(su <= centre[0] < su + tw and sv <= centre[1] < sv + th
                       for _s, (su, sv, tw, th), _b in slots.values()):
                self.assertEqual(ps.getpixel(centre)[3], 0, centre)
        # The Xbox sheet's Share slot is empty: its highlight shows nothing.
        _s, (u, v, tw, th), _b = slots['SelectButton']
        self.assertEqual(xbox.crop((u, v, u + tw, v + th)).getchannel('A').getextrema(), (0, 0))
        _s, (u, v, tw, th), boxes = slots['CrossButton']
        self.assertGreater(xbox.crop((u, v, u + tw, v + th)).getchannel('A').getextrema()[1], 0)
        # The layout's tags take the places and slots, the same length.
        new = xc.relayout_dual(text.encode('latin1'), slots)
        self.assertEqual(len(new), len(text))
        moved = xc.highlights(new.decode('latin1'))
        for name, ((x, y, w, h), (u, v, tw, th), _b) in slots.items():
            for got, want in zip(moved[name], (x, y, w, h, u, v, tw, th)):
                self.assertAlmostEqual(got, want, delta=0.01, msg=name)

    def test_screens_that_place_a_highlight_differently_are_refused(self):
        text = ps_layout().decode('latin1')
        first, second, _rest = text.split('</Screen>\r\n')
        other = second.replace('x="580.00" y="33.00"', 'x="581.00" y="33.00"')
        with self.assertRaisesRegex(ValueError, 'differently'):
            xc.highlights(first + '</Screen>\r\n' + other + '</Screen>\r\n')


class DualBuildTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(gnf.Texture, 'replaced', image_replaced)
        patcher.start()
        self.addCleanup(patcher.stop)
        image = mock.patch.object(gnf.Texture, 'image', lambda texture, level=0: ps_sheet())
        image.start()
        self.addCleanup(image.stop)

    def game(self, folder, xml=None):
        game = Path(folder) / 'game'
        game.mkdir()
        archive(game / 'data08.psarc', {TEXTURE: texture(41, 1024, 1024, fill=None),
                                        'data/fe/images/other.gnf': bytes(5000)})
        archive(game / 'data09.psarc', {xc.LAYOUT: xml or rows(ps_layout())}, packed=False)
        return game

    def run_main(self, game, out, *options):
        (out / 'file_patches.txt').write_text('')
        with redirect_stdout(io.StringIO()) as printed, \
                mock.patch.object(sys, 'argv', ['x', str(game), '--out', str(out), *options]):
            xc.main()
        return printed.getvalue(), (out / 'file_patches.txt').read_text().splitlines()

    def patched(self, game, folder, lines, name):
        mine = [line for line in lines if line.startswith(f'/app0/{name} ')]
        target = Path(folder) / name
        target.write_bytes(apply((game / name).read_bytes(), mine))
        return target

    def test_both_styles_share_the_layout_and_the_swap_file_has_both_sheets(self):
        with tempfile.TemporaryDirectory() as folder:
            game, out = self.game(folder), Path(folder) / 'out'
            out.mkdir()
            swap = out / 'glyph_swap.txt'
            swap.write_text('glyphs line\n')  # wipeout_glyphs.py's, kept
            seen = {}
            for style in ('playstation', 'xbox'):
                printed, lines = self.run_main(game, out, '--style', style, '--swap-config', str(swap))
                self.assertIn(f'Controller picture: {style} at start, follows the Controller type row', printed)
                sheet = read(self.patched(game, folder, lines, 'data08.psarc'), TEXTURE)
                xml = read(self.patched(game, folder, lines, 'data09.psarc'), xc.LAYOUT)
                seen[style] = sheet, xml
                self.assertIn(b'name="Pad Style P1"', xml)
                self.assertNotIn(b'x="-999"', xml)  # Share stays where it is on both
            self.assertEqual(seen['playstation'][1], seen['xbox'][1])
            self.assertNotEqual(seen['playstation'][0], seen['xbox'][0])
            entries = swap.read_text().splitlines()
            self.assertEqual(entries[0], 'glyphs line')
            # Two runs appended one line each; both name the newest sheet and each style's file,
            # which is what the game reads once that style's patches are in.
            self.assertEqual(len(entries), 3)
            self.assertEqual(entries[1], entries[2])
            fields = entries[1].split('\t')
            with psarc.Archive(game / 'data08.psarc') as a:
                i = a.find(TEXTURE)
                self.assertEqual(fields[:4], ['/app0/data08.psarc', f"{a.entries[i]['offset']:x}",
                                              f'{sum(s for s, _l in a.blocks(i)):x}', f'{gnf.HEADER:x}'])
            files = dict(field.split('=', 1) for field in fields[4:])
            self.assertEqual(set(files), {'playstation', 'xbox'})
            for style, path in files.items():
                self.assertEqual(Path(path).read_bytes(), seen[style][0], style)
            # Cached: both styles from one drawing.
            with mock.patch.object(xc, 'draw_sheet', side_effect=AssertionError('drawn again')):
                lines, swaps = xc.build_dual(game, out, 'xbox')
            self.assertEqual(swaps, [entries[1] + '\n'])
            self.assertEqual(read(self.patched(game, folder, lines, 'data08.psarc'), TEXTURE), seen['xbox'][0])

    def test_a_layout_it_cannot_use_falls_back_to_the_rows_or_the_xbox_picture(self):
        with tempfile.TemporaryDirectory() as folder:
            # The fixture's highlights all share one sprite: the slots fit, but a missing tag does not.
            game, out = self.game(folder, rows(layout(names=('CrossButton',) + tuple(xc.PARTS)[1:]))), Path(folder) / 'out'
            out.mkdir()
            swap = out / 'glyph_swap.txt'
            printed, lines = self.run_main(game, out, '--style', 'playstation', '--swap-config', str(swap))
            self.assertIn('Controller picture:', printed)
            self.assertIn('Controller type row: on the Controls screens', printed)
            xml = read(self.patched(game, folder, lines, 'data09.psarc'), xc.LAYOUT)
            self.assertIn(b'name="Pad Style P1"', xml)
            self.assertFalse(any(line.startswith('/app0/data08.psarc ') for line in lines))  # the game's picture
            self.assertFalse(swap.exists() and swap.read_text())


def rows(xml):
    """A layout with the Thrust Sensitivity sliders the rows replace (tests/test_controller_row.py)."""
    from test_controller_row import P1, P2
    first, second, _rest = xml.split(b'</Screen>\r\n')
    return first + P1 + b'</Screen>\r\n' + second + P2 + b'</Screen>\r\n'


class ArchiveListTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(gnf.Texture, 'replaced', fast_replaced)
        patcher.start()
        self.addCleanup(patcher.stop)

    """The archives run.py passes: on a drive without links large ones stay outside the game view."""

    def test_archives_outside_the_game_folder_are_used_under_their_game_names(self):
        with tempfile.TemporaryDirectory() as folder:
            game = BuildTest.game(self, folder)
            kept, out = Path(folder) / 'base', Path(folder) / 'out'
            kept.mkdir()
            out.mkdir()
            (game / 'data08.psarc').rename(kept / 'data08.psarc')
            given = [('data09.psarc', game / 'data09.psarc'), ('data08.psarc', kept / 'data08.psarc')]
            self.assertEqual(psarc.archives(game, given), sorted(given))
            self.assertEqual([name for name, _p in psarc.archives(game)], ['data09.psarc'])
            argv = ['x', str(game), '--out', str(out)] + [part for name, path in given for part in ('--archive', name, str(path))]
            with redirect_stdout(io.StringIO()) as printed, mock.patch.object(sys, 'argv', argv):
                xc.main()
            self.assertIn('Xbox controller: on the Controls screen', printed.getvalue())
            lines = (out / 'file_patches.txt').read_text().splitlines()
            mine = [line for line in lines if line.startswith('/app0/data08.psarc ')]
            self.assertTrue(mine)
            patched = apply((kept / 'data08.psarc').read_bytes(), mine)
            (Path(folder) / 'data08.psarc').write_bytes(patched)
            self.assertNotEqual(read(Path(folder) / 'data08.psarc', TEXTURE), read(kept / 'data08.psarc', TEXTURE))
            # The cache follows the real files.
            self.assertIn(json.dumps(str(kept / 'data08.psarc')), (out / 'xbox_controls' / 'stamp.json').read_text())


if __name__ == '__main__':
    unittest.main()
