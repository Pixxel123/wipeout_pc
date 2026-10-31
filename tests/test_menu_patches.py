import tempfile
import sys
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest import mock

from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import menu_patches
from psarc_fixtures import archive as fixture

MENU = (b'<Screen>\n    <Slider name="Eliminations" global="NumberOfEliminations">\n'
        b'      <Values minSlide="50" maxSlide="600" default="200" incSlide="25"></Values>\n</Screen>\n')


def archive(path, data, compressed=False):
    """data09.psarc holding the racebox menu, stored as the game's is, or compressed."""
    fixture(path, {menu_patches.MENU: data}, packed=compressed)
    return [(path.name, path)]


class MenuPatchTests(unittest.TestCase):
    def test_eliminator_slider_goes_to_2000_in_the_same_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            archives = archive(Path(folder) / 'data09.psarc', MENU)
            [(guest, offset, old, new)] = menu_patches.eliminator_patches(folder, given=archives)
            self.assertEqual(guest, '/app0/data09.psarc')
            self.assertEqual(len(old), len(new))
            raw = (Path(folder) / 'data09.psarc').read_bytes()
            self.assertEqual(raw[offset:offset + len(old)], old)
            patched = raw[:offset] + new + raw[offset + len(new):]
            self.assertIn(b'\n   <Slider name="Eliminations"', patched)
            self.assertIn(b'maxSlide="2000" default="200"', patched)

    def test_limit_takes_one_indentation_space_per_extra_digit(self):
        with tempfile.TemporaryDirectory() as folder:
            archives = archive(Path(folder) / 'data09.psarc', MENU)
            [(_g, _o, old, new)] = menu_patches.eliminator_patches(folder, 750, archives)
            self.assertEqual((len(old), new), (len(new), old.replace(b'"600"', b'"750"')))
            [(_g, _o, old, new)] = menu_patches.eliminator_patches(folder, 5000, archives)
            self.assertEqual(new, old[1:].replace(b'"600"', b'"5000"'))
            [(_g, _o, old, new)] = menu_patches.eliminator_patches(folder, 12000, archives)
            self.assertEqual(new, old[2:].replace(b'"600"', b'"12000"'))
            self.assertEqual(menu_patches.eliminator_patches(folder, 600, archives), [])

    def test_compressed_or_missing_menus_are_left_alone(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(menu_patches.eliminator_patches(folder, given=[]), [])
            archives = archive(Path(folder) / 'data09.psarc', MENU, compressed=True)
            self.assertEqual(menu_patches.eliminator_patches(folder, given=archives), [])

    def test_an_archive_outside_the_game_folder_is_read_under_its_game_name(self):
        # A drive without links keeps large archives out of the game view; run.py passes them.
        with tempfile.TemporaryDirectory() as folder:
            game, kept = Path(folder) / 'game', Path(folder) / 'base'
            game.mkdir()
            kept.mkdir()
            archive(kept / 'data09.psarc', MENU)
            self.assertEqual(menu_patches.eliminator_patches(game), [])
            [(guest, offset, old, _new)] = menu_patches.eliminator_patches(game, given=[('data09.psarc', kept / 'data09.psarc')])
            self.assertEqual(guest, '/app0/data09.psarc')
            self.assertEqual((kept / 'data09.psarc').read_bytes()[offset:offset + len(old)], old)
            argv = ['x', str(game), '--out', folder, '--archive', 'data09.psarc', str(kept / 'data09.psarc')]
            with redirect_stdout(StringIO()), mock.patch.object(sys, 'argv', argv):
                menu_patches.main()
            self.assertTrue((Path(folder) / 'file_patches.txt').read_text().startswith(f'/app0/data09.psarc {offset:x} '))

    def test_without_a_list_the_game_folder_is_read(self):
        with tempfile.TemporaryDirectory() as folder:
            archive(Path(folder) / 'data09.psarc', MENU)
            [(guest, _o, _old, _new)] = menu_patches.eliminator_patches(folder)
            self.assertEqual(guest, '/app0/data09.psarc')


if __name__ == '__main__':
    unittest.main()
