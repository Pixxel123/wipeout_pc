import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import gnf
import photo_mode as pm
import psarc
from psarc_fixtures import apply, archive, texture
from test_xbox_controls import fast_replaced, read

HD, HALF, FURY = pm.SHEETS


class DrawingTest(unittest.TestCase):
    def test_sprites_redrawn_the_rest_kept(self):
        from PIL import Image
        for name, size in ((HD, 512), (HALF, 256), (FURY, 256)):
            sheet = Image.new('RGBA', (size, size), (10, 20, 30, 255))
            out = pm.redraw(sheet, name)
            space, sprites, _prefix = pm.SHEETS[name]
            k = size / space
            for u, v, w, h, stick in sprites:
                box = (round(u * k), round(v * k), round((u + w) * k), round((v + h) * k))
                sprite = out.crop(box)
                alpha = sprite.getchannel('A')
                self.assertEqual(alpha.getextrema(), (0, 255), (name, stick))  # drawn on a cleared sprite
                # The active stick is red: a red cap in the left or right half.
                reds = [(x, y) for x in range(sprite.width) for y in range(sprite.height)
                        if (lambda p: p[3] > 200 and p[0] > 150 and p[1] < 80)(sprite.getpixel((x, y)))]
                self.assertGreater(len(reds), 20, (name, stick))
                mean_x = sum(x for x, _y in reds) / len(reds)
                self.assertEqual(mean_x > sprite.width / 2, stick == 'right', (name, stick))
                # The rest of the sheet is as it was.
                self.assertEqual(out.getpixel((box[0] - 2, box[1] - 2)), (10, 20, 30, 255), name)
            self.assertEqual(out.getpixel((size // 2, size // 4)), (10, 20, 30, 255), name)


    def test_drawings_keep_clear_of_the_sprite_edges(self):
        # The help draws its sprites filtered: a line on the edge shows as a cut stroke.
        for svg, (w, h) in (('xbox_photo_left_stick.svg', (131, 82)), ('xbox_photo_small_right_stick.svg', (65, 41)),
                            ('xbox_photo_fury_left_stick.svg', (65, 40))):
            alpha = pm.draw(pm.ART / svg, w, h).getchannel('A')
            edges = [(x, 0) for x in range(w)] + [(x, h - 1) for x in range(w)] + \
                    [(0, y) for y in range(h)] + [(w - 1, y) for y in range(h)]
            self.assertFalse([p for p in edges if alpha.getpixel(p)], svg)

    def test_the_games_pictogram_is_cleared_past_the_rectangle_its_neighbours_kept(self):
        from PIL import Image, ImageDraw
        sheet = Image.new('RGBA', (512, 512), (0, 0, 0, 0))
        draw = ImageDraw.Draw(sheet)
        draw.rectangle((6, 425, 140, 510), outline=(255, 255, 255, 255), width=2)  # 2 texels past (8, 427)-(139, 509)
        draw.rectangle((150, 430, 160, 440), fill=(255, 255, 255, 255))          # another sprite, 11 texels away
        out = pm.redraw(sheet, HD)
        self.assertEqual(out.getpixel((6, 470)), (0, 0, 0, 0))
        self.assertEqual(out.getpixel((70, 510)), (0, 0, 0, 0))
        self.assertEqual(out.getpixel((155, 435)), (255, 255, 255, 255))


class BuildTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(gnf.Texture, 'replaced', fast_replaced)
        patcher.start()
        self.addCleanup(patcher.stop)

    def game(self, folder):
        game = Path(folder) / 'game'
        game.mkdir()
        archive(game / 'data00.psarc', {HD: texture(41, 512, 512, fill=None), FURY: texture(41, 256, 256, fill=None),
                                        'data/fe/images/other.gnf': bytes(5000)})
        archive(game / 'data08.psarc', {HD: texture(41, 512, 512, fill=None), HALF: texture(41, 256, 256, fill=None)})
        return game

    def run_main(self, game, out, *options):
        (out / 'file_patches.txt').write_text('')
        with redirect_stdout(io.StringIO()) as printed, \
                mock.patch.object(sys, 'argv', ['x', str(game), '--out', str(out), *options]):
            pm.main()
        return printed.getvalue(), (out / 'file_patches.txt').read_text().splitlines()

    def test_the_sheets_change_and_follow_the_row(self):
        with tempfile.TemporaryDirectory() as folder:
            game, out = self.game(folder), Path(folder) / 'out'
            out.mkdir()
            swap = out / 'glyph_swap.txt'
            swap.write_text('')
            printed, lines = self.run_main(game, out, '--style', 'xbox', '--swap-config', str(swap))
            self.assertIn('Photo mode: xbox controllers in its help', printed)
            self.assertIn('3 sheets follow the Controller type row', printed)
            # Every copy of every sheet changes; the other file does not.
            for name in ('data00.psarc', 'data08.psarc'):
                mine = [line for line in lines if line.startswith(f'/app0/{name} ')]
                self.assertTrue(mine, name)
                (Path(folder) / name).write_bytes(apply((game / name).read_bytes(), mine))
            for name, sheet in (('data00.psarc', HD), ('data00.psarc', FURY), ('data08.psarc', HD), ('data08.psarc', HALF)):
                self.assertNotEqual(read(Path(folder) / name, sheet), read(game / name, sheet), (name, sheet))
            self.assertEqual(read(Path(folder) / 'data00.psarc', 'data/fe/images/other.gnf'), bytes(5000))
            # The swap file: the newest copy of each sheet, the original for playstation (and
            # nintendo), the new one for xbox.
            entries = [line.split('\t') for line in swap.read_text().splitlines()]
            self.assertEqual(sorted(e[0] for e in entries), ['/app0/data00.psarc', '/app0/data08.psarc', '/app0/data08.psarc'])
            for fields in entries:
                files = dict(field.split('=', 1) for field in fields[4:])
                self.assertEqual(set(files), {'playstation', 'xbox', 'nintendo'})
                self.assertEqual(files['nintendo'], files['playstation'])
                with psarc.Archive(game / Path(fields[0]).name) as a:
                    i = next(i for i, e in a.named() if e['offset'] == int(fields[1], 16))
                    self.assertEqual(Path(files['playstation']).read_bytes(), a.read(i))
                    self.assertEqual(int(fields[2], 16), a.stored_size(i))
                self.assertNotEqual(Path(files['xbox']).read_bytes(), Path(files['playstation']).read_bytes())
            # PlayStation at start: the game's own sheets, the same swap lines; cached.
            with mock.patch.object(pm, 'redraw', side_effect=AssertionError('drawn again')):
                ps_lines, ps_swaps = pm.build(game, out, 'playstation')
                self.assertEqual(ps_lines, [])
                self.assertEqual(ps_swaps, [line + '\n' for line in swap.read_text().splitlines()])
                self.assertEqual(pm.build(game, out, 'nintendo'), (ps_lines, ps_swaps))

    def test_without_the_sheets_nothing_changes(self):
        with tempfile.TemporaryDirectory() as folder:
            game, out = Path(folder) / 'game', Path(folder) / 'out'
            game.mkdir()
            out.mkdir()
            archive(game / 'data00.psarc', {'data/fe/images/other.gnf': bytes(5000)})
            printed, lines = self.run_main(game, out)
            self.assertIn('keeps the DualShock pictograms', printed)
            self.assertEqual(lines, [])


if __name__ == '__main__':
    unittest.main()
