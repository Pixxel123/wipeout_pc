"""Generated WipEout resolution patches: the hand-made entries are pure functions of width x height,
so any even size gets the same sites with its own values."""
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from paths import ROOT
import graphics_options as g
import patches

WIPEOUT = ROOT / 'patches/WipEout_Omega_Collection.xml'


def hand_made():
    """{(W, H): [(Address, Value, Pattern, At)]} of the XML's Resolution Patch entries."""
    out = {}
    for meta in ET.parse(WIPEOUT).getroot().iter('Metadata'):
        name = meta.get('Name')
        if not name.startswith('Resolution Patch - '):
            continue
        lines = [(l.get('Address'), l.get('Value'), l.get('Pattern'), l.get('At')) for l in meta.iter('Line')]
        values = {a: v for a, v, _p, _t in lines}
        width = struct.unpack('<I', bytes.fromhex(values['0x00bfa2e8']))[0]
        height = struct.unpack('<I', bytes.fromhex(values['0x00bfa2f6']))[0]
        out[(width, height)] = lines
    return out


class ResolutionPatchTest(unittest.TestCase):
    def test_reproduces_every_hand_made_entry(self):
        entries = hand_made()
        self.assertEqual(len(entries), 15)
        for (width, height), lines in entries.items():
            with self.subTest(size=f'{width}x{height}'):
                self.assertEqual(g.resolution_patch_lines(width, height), lines)

    def test_steam_deck_size(self):
        lines = {a: v for a, v, _p, _t in g.resolution_patch_lines(1280, 800)}
        self.assertEqual(len(lines), 27)
        self.assertEqual(lines['0x00c60d00'], struct.pack('<f', 0.8).hex())
        self.assertEqual(lines['0x00c60d04'], struct.pack('<f', 3.2).hex())
        self.assertEqual(lines['0x00c60d08'], struct.pack('<f', 1.6).hex())
        self.assertEqual(lines['0x00c60d48'], lines['0x00c60d08'])
        self.assertEqual(lines['0x00a4b51b'], struct.pack('<H', 640).hex())
        self.assertEqual(lines['0x00a4b520'], struct.pack('<H', 400).hex())
        self.assertNotIn('0x00c6bb60', lines)

    def test_memory_line_from_four_and_a_half_megapixels(self):
        self.assertIn('0x00c6bb60', {a for a, *_r in g.resolution_patch_lines(3840, 2400)})
        self.assertNotIn('0x00c6bb60', {a for a, *_r in g.resolution_patch_lines(2560, 1600)})

    def test_memory_grows_in_steps(self):
        self.assertEqual([g.memory_mb(w, h) for w, h in ((2560, 1600), (3440, 1440), (5120, 2160), (5120, 2880),
                                                         (6016, 3384), (7680, 4320))],
                         [0, 1024, 1024, 2048, 2048, 4096])

    def test_narrower_than_16x9_is_vert_plus(self):
        def fov(width, height):
            lines = {a: v for a, v, _p, _t in g.resolution_patch_lines(width, height)}
            return lines.get('0x00c60d0c'), lines.get('0x00c60d4c')
        self.assertEqual(fov(3440, 1440), (None, None))  # wider: Hor+, the field of view is the game's
        steam_deck = fov(1280, 800)
        self.assertEqual(steam_deck, fov(1920, 1200))  # 16:10 at any size, both camera sets alike
        self.assertEqual(steam_deck[0], steam_deck[1])
        self.assertNotEqual(fov(2256, 1504), steam_deck)  # 3:2 has its own factor
        self.assertAlmostEqual(g.vert_plus(1920, 1440), 1.2271, places=4)
        self.assertAlmostEqual(g.vert_plus(1920, 1200), 1.0816, places=4)

    def test_sizes_must_be_even(self):
        for width, height in ((1365, 768), (1366, 767)):
            with self.assertRaises(ValueError):
                g.resolution_patch_lines(width, height)
        lines = {a: v for a, v, _p, _t in g.resolution_patch_lines(1366, 768)}
        self.assertEqual(lines['0x00a44654'], struct.pack('<I', 683).hex())
        self.assertEqual(lines['0x00a44659'], struct.pack('<I', 384).hex())

    def test_patch_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = g.resolution_patch_xml(2256, 1504, Path(directory) / 'generated.xml')
            self.assertEqual(patches.resolution_names([path]), ['2256x1504'])
            root = ET.parse(path).getroot()
            self.assertEqual([e.text for e in root.iter('ID')], ['CUSA07671', 'CUSA05670'])
            meta = next(root.iter('Metadata'))
            self.assertEqual(meta.get('AppVer'), '01.00')
            self.assertEqual(meta.get('isEnabled'), 'false')
            self.assertNotIn('DMEM', meta.get('Note'))
            self.assertEqual(len(list(meta.iter('Line'))), 27)
            big = g.resolution_patch_xml(6016, 3384, Path(directory) / 'big.xml')
            self.assertIn('(DMEM to 2048 MB)', next(ET.parse(big).getroot().iter('Metadata')).get('Note'))


if __name__ == '__main__':
    unittest.main()
