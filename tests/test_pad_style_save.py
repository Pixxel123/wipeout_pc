import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from paths import ROOT  # noqa: F401
import pad_style_save as save


def record(name, value, capacity=0x40):
    header = bytearray(0x68)
    struct.pack_into('<I', header, 0, zlib.crc32(name.encode(), 0xffffffff))
    header[4:4 + len(name)] = name.encode()
    struct.pack_into('<III', header, 0x44, 0, len(value) + 1, capacity)
    return bytes(header) + value.encode().ljust(capacity, b'\0')


def file(*records):
    return struct.pack('<I', 0x2000) + bytes(0x3c0) + b''.join(records)


SAVE = file(record('Language', '1'), record('Motion Sensitivity', '50%'))


class SaveTests(unittest.TestCase):
    def test_records_are_read_in_order(self):
        self.assertEqual([(n, v) for _o, n, v in save.records(SAVE)], [('Language', '1'), ('Motion Sensitivity', '50%')])

    def test_key_is_the_games_hash(self):
        self.assertEqual(save.key('Motion Sensitivity'), 0x1b432e78)
        self.assertEqual(save.key('Pad Style'), 0xdde7131f)

    def test_missing_settings_are_appended(self):
        new = save.with_settings(SAVE, {'Pad Style P1': 'xbox', 'Pad Style P2': 'playstation'})
        self.assertEqual(new[:len(SAVE)], SAVE)
        self.assertEqual(len(new), len(SAVE) + 2 * (0x68 + 0x40))
        self.assertEqual([(n, v) for _o, n, v in save.records(new)][-2:],
                         [('Pad Style P1', 'xbox'), ('Pad Style P2', 'playstation')])
        self.assertEqual(new[len(SAVE):len(SAVE) + 0x68 + 0x40], record('Pad Style P1', 'xbox'))

    def test_existing_settings_are_kept(self):
        have = file(record('Pad Style P1', 'playstation'), record('Pad Style P2', 'xbox'))
        self.assertIs(save.with_settings(have, {'Pad Style P1': 'xbox', 'Pad Style P2': 'xbox'}), have)

    def test_other_formats_are_left_alone(self):
        for data in (b'', b'\0' * 0x3c4 + b'x', SAVE[:-1], struct.pack('<I', 0x2001) + SAVE[4:]):
            self.assertIsNone(save.records(data))
            self.assertIsNone(save.with_settings(data, {'Pad Style P1': 'xbox'}))

    def test_saves_on_disk(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / '1' / 'CUSA07671' / 'PROFILE03' / 'DATA.BIN'
            path.parent.mkdir(parents=True)
            path.write_bytes(SAVE)
            other = Path(folder) / '1' / 'CUSA07671' / 'PROFILE04' / 'DATA.BIN'
            other.parent.mkdir(parents=True)
            other.write_bytes(b'not a save')
            self.assertIsNone(save.saved_style(folder, 'CUSA07671', 'Pad Style P1'))
            self.assertEqual(save.add_settings(folder, 'CUSA07671', 'xbox'), [path])
            self.assertEqual(save.saved_style(folder, 'CUSA07671', 'Pad Style P1'), 'xbox')
            self.assertEqual(save.saved_style(folder, 'CUSA07671', 'Pad Style P2'), 'xbox')
            self.assertEqual(other.read_bytes(), b'not a save')
            self.assertEqual(save.add_settings(folder, 'CUSA07671', 'playstation'), [])  # already there


if __name__ == '__main__':
    unittest.main()
