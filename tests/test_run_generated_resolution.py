"""run.py's generated render resolution: an even size nobody provides gets a patch file of its own,
fed through the mod-patch path, so it is applied and listed like a mod's size."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
import game_profiles
import graphics_options as g
import patches

sys.path.insert(0, str(ROOT))
import run  # noqa: E402

WIPEOUT = ROOT / 'patches/WipEout_Omega_Collection.xml'
OWN = game_profiles.PROFILES['CUSA07671']['env']['GOW3_ALLOWED_RESOLUTIONS'].split(';')


class GeneratedResolutionTest(unittest.TestCase):
    def test_generated_size_is_applied_through_the_mod_path(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            ini = {'render_resolution': '2256x1504'}
            files, extra = run.generated_resolution_files(ini, [], [], out)
            self.assertEqual(extra, ['2256x1504'])
            self.assertEqual(files, [out / 'generated-resolution.xml'])
            merged = patches.merge_patch_files(WIPEOUT, files, out / 'merged.xml')
            names = patches.selected_patches(merged, '01.00', '', only=False)
            names = g.patch_names(names, g.startup_options(ini, extra))
            self.assertIn('Resolution Patch - 2256x1504', names)
            self.assertIn('Native 4K rendering (no checkerboard)', names)
            self.assertEqual(patches.patch_requirements(merged, names, '01.00'), (None, None))

    def test_big_generated_size_asks_for_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            files, _extra = run.generated_resolution_files({'render_resolution': '5120x2880'}, [], [], out)
            merged = patches.merge_patch_files(WIPEOUT, files, out / 'merged.xml')
            dmem, _vblank = patches.patch_requirements(merged, ['Resolution Patch - 5120x2880'], '01.00')
            self.assertEqual(dmem, patches.DMEM_RETAIL_MB + 1024)

    def test_known_sizes_generate_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            for value in ('native', '1440p', '3440x1440'):
                self.assertEqual(run.generated_resolution_files({'render_resolution': value}, [], [], out), ([], []), value)
            self.assertEqual(run.generated_resolution_files({'render_resolution': '3840x1080'}, ['3840x1080'], [], out), ([], []))
            self.assertEqual(run.generated_resolution_files({'render_resolution': '2880x1800'}, [], ['2880x1800'], out), ([], []))
            self.assertFalse((out / 'generated-resolution.xml').exists())

    def test_wipeout_list_replaces_the_shared_one(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            path = out / 'generated-resolution.xml'
            self.assertEqual(run.generated_resolution_files({'render_resolution': '2048x1536'}, [], OWN, out),
                             ([path], ['2048x1536']))
            self.assertIn('Resolution Patch - 2048x1536', path.read_text())
            path.unlink()
            for value in ('3440x1440', 'native', '1440p'):
                self.assertEqual(run.generated_resolution_files({'render_resolution': value}, [], OWN, out), ([], []), value)
            self.assertFalse(path.exists())

    def test_below_the_games_own_size_generates_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            for value in ('1280x800', '1366x768'):
                self.assertEqual(run.generated_resolution_files({'render_resolution': value}, [], OWN, out), ([], []), value)
            self.assertFalse((out / 'generated-resolution.xml').exists())

    def test_named_size_wipeout_lacks_is_generated_under_its_name(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            files, extra = run.generated_resolution_files({'render_resolution': '1800p'}, [], OWN, out)
            self.assertEqual(extra, ['1800p'])
            merged = patches.merge_patch_files(WIPEOUT, files, out / 'merged.xml')
            names = patches.selected_patches(merged, '01.00', '', only=False)
            names = g.patch_names(names, g.startup_options({'render_resolution': '1800p'}, extra))
            self.assertIn('Resolution Patch - 1800p', names)
            text = merged.read_text()
            self.assertIn('Resolution Patch - 1800p', text)
            self.assertIn(struct.pack('<I', 3200).hex(), text)

    def test_no_own_list_means_the_shared_one_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(run.generated_resolution_files({'render_resolution': '2048x1536'}, [], [''], Path(directory)),
                             ([], []))

    def test_only_wipeout_generates(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            ini = {'render_resolution': '2256x1504'}
            self.assertEqual(run.generated_resolution_files(ini, [], [], out, 'CUSA01623'), ([], []))
            self.assertFalse((out / 'generated-resolution.xml').exists())
            self.assertEqual(run.generated_resolution_files(ini, [], [], out, 'CUSA07671')[1], ['2256x1504'])


if __name__ == '__main__':
    unittest.main()
