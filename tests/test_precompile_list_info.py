"""tools/deck/precompile-list-info.py on small lists built in memory (the layout of
vk_precompile_list.h: header, then entries of u32 size, FNV-1a 64, body)."""
import contextlib
import importlib.util
import io
import struct
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

spec = importlib.util.spec_from_file_location('precompile_list_info',
                                              ROOT / 'tools/deck/precompile-list-info.py')
info = importlib.util.module_from_spec(spec)
spec.loader.exec_module(info)


def frame(body):
    return struct.pack('<IQ', len(body), info.fnv1a(body)) + body


def header(version=3):
    return b'GOW3PCL\n' + struct.pack('<4I', version, 0xD000, 16, 6)


def key(regs=2, stages=1, roots=1, words=2, pointers=1):
    body = struct.pack('<IQIII', 0, 7, 0, 0, regs) + bytes(8 * regs) + struct.pack('<I', stages)
    for _ in range(stages):
        body += struct.pack('<IIQQ', 0, 0, 1, 2) + bytes(64) + struct.pack('<I', roots)
        body += bytes(12 * roots)
    body += struct.pack('<I', words) + bytes(16 * words) + struct.pack('<I', pointers)
    return frame(body + bytes(20 * pointers))


def code(words=4):
    return frame(struct.pack('<IQI', 1, 9, words) + bytes(4 * words))


def run(*args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = info.main([str(a) for a in args])
    return status, out.getvalue()


class ListInfoTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = Path(self.dir.name)

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_summary_counts_match(self):
        keys = [key(), key(regs=0, roots=0, words=0, pointers=0)]
        data = header() + keys[0] + code() + keys[1] + code(8)
        status, out = run(self.write('a.bin', data))
        self.assertEqual(status, 0, out)
        for line in ('version: 3', 'key entries: 2', 'code records: 2', 'bytes, registers: 16',
                     'bytes, user data: 128', 'bytes, roots: 12', 'bytes, guest words: 32',
                     'bytes, pointers: 20', f'bytes, code: {len(code()) + len(code(8))}',
                     f'bytes, total: {len(data)}'):
            self.assertIn(line, out)

    def test_strip_code_keeps_the_keys_byte_for_byte(self):
        k1, k2 = key(), key(stages=2, roots=0)
        data = header() + k1 + code() + k2 + code(8)
        src, dst = self.write('a.bin', data), self.root / 'b.bin'
        status, out = run(src, '--strip-code', dst)
        self.assertEqual(status, 0, out)
        self.assertEqual(dst.read_bytes(), header() + k1 + k2)
        status, out = run(dst)
        self.assertEqual(status, 0, out)
        self.assertIn('code records: 0', out)
        self.assertIn('bytes, code: 0', out)
        self.assertIn('key entries: 2', out)

    def test_a_corrupted_checksum_exits_1(self):
        data = bytearray(header() + key())
        data[-1] ^= 1
        status, out = run(self.write('a.bin', bytes(data)))
        self.assertEqual(status, 1, out)

    def test_bad_magic_and_version_exit_1(self):
        self.assertEqual(run(self.write('a.bin', b'NOTALIST' + bytes(20)))[0], 1)
        self.assertEqual(run(self.write('b.bin', header(2) + key()))[0], 1)


if __name__ == '__main__':
    unittest.main()
