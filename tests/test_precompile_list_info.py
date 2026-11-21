"""tools/deck/precompile-list-info.py on small lists built in memory (the layout of
vk_precompile_list.h: header, then entries of u32 size, FNV-1a 64, body)."""
import contextlib
import importlib.util
import io
import random
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

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


def program_key(*programs):
    """A key entry whose stages run `programs`."""
    body = struct.pack('<IQIII', 0, 7, 0, 0, 0) + struct.pack('<I', len(programs))
    for program in programs:
        body += struct.pack('<IIQQ', 0, 0, program, 2) + bytes(64) + struct.pack('<I', 0)
    return frame(body + struct.pack('<II', 0, 0))


def blob(program, body=(1, 2, 3, 4)):
    """A program's code as the game's files hold it: the start token, its second word putting the
    OrbShdr header after `body`, then the header with length, hash and crc32."""
    words = len(body) + 2
    start = struct.pack('<II', 0xBEEB03FF, words // 2 - 1) + struct.pack(f'<{len(body)}I', *body)
    length = (words + 8) * 4
    head = b'OrbShdr' + bytes(1) + struct.pack('<IIQ', length << 8, 0, program)
    return start + head + struct.pack('<II', zlib.crc32(start + head[:16]), 0)


def psarc(*files, block=64):
    """A PSARC holding `files`: each block zlib, or stored when that is not smaller (a full block
    stored is 0 in the size table, whose entries are as wide as the block size needs)."""
    width = 2 if block <= 1 << 16 else 3
    toc, sizes, data = [], [], b''
    for file in files:
        toc.append((len(sizes), len(file), len(data)))
        for at in range(0, len(file), block):
            part = file[at:at + block]
            packed = zlib.compress(part)
            if len(packed) >= len(part):
                packed = part
            sizes.append(0 if len(packed) == block else len(packed))
            data += packed
    toc_len = 32 + 30 * len(files) + width * len(sizes)
    out = struct.pack('>4sI4sIIIII', b'PSAR', 0x10004, b'zlib', toc_len, 30, len(files), block, 0)
    for first, size, offset in toc:
        out += bytes(16) + struct.pack('>I', first) + size.to_bytes(5, 'big')
        out += (offset + toc_len).to_bytes(5, 'big')
    return out + b''.join(s.to_bytes(width, 'big') for s in sizes) + data


def noise(size):
    """Bytes zlib cannot shrink: a block of them is stored."""
    return random.Random(size).randbytes(size)


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

    def add_code(self, data, *files):
        """Runs --add-code on list `data` with a game dir holding `files`, (name, bytes) pairs."""
        game = self.root / 'game'
        game.mkdir(exist_ok=True)
        for name, content in files:
            (game / name).write_bytes(content)
        dst = self.root / 'out.bin'
        status, out = run(self.write('in.bin', data), '--add-code', dst, game)
        return status, out, dst

    def test_add_code_roundtrip(self):
        k1, k2 = program_key(0x11, 0x22), program_key(0x22)
        data = header() + k1 + k2
        a, b = blob(0x11), blob(0x22, body=tuple(range(20)))
        file = noise(48) + a + bytes(8) + b + bytes(30)
        archive = psarc(file)
        table = archive[62:62 + 2 * 4]
        self.assertEqual(table[:2], bytes(2))  # the first block (noise and a's start) stored
        self.assertTrue(all(0 < int.from_bytes(table[i:i + 2], 'big') < 64 for i in (2, 4)))
        status, out, dst = self.add_code(data, ('data00.psarc', archive))
        self.assertEqual(status, 0, out)
        self.assertIn('programs found: 2', out)
        self.assertIn('programs without code: 0', out)
        self.assertNotIn('code records', out)  # only the add-code report
        result = dst.read_bytes()
        _, entries = info.read_list(result)
        self.assertEqual([e[0] for e in entries], [info.CODE, info.CODE, info.KEY, info.KEY])
        for (_, record, _), program, want in zip(entries, (0x11, 0x22), (a, b)):
            self.assertEqual(struct.unpack_from('<IQI', record, 12), (1, program, len(want) // 4))
            self.assertEqual(record[28:], want)
        stripped = self.root / 'stripped.bin'
        self.assertEqual(run(dst, '--strip-code', stripped)[0], 0)
        self.assertEqual(stripped.read_bytes(), data)

    def test_add_code_skips_bad_crc(self):
        good = blob(0x11)
        bad = bytearray(good)
        bad[8] ^= 1
        status, out, dst = self.add_code(header() + program_key(0x11),
                                         ('data00.psarc', psarc(bytes(bad) + bytes(4) + good)))
        self.assertEqual(status, 0, out)
        _, entries = info.read_list(dst.read_bytes())
        self.assertEqual(entries[0][1][28:], good)

    def test_add_code_from_eboot(self):
        status, out, dst = self.add_code(header() + program_key(0x11),
                                         ('eboot.bin', bytes(12) + blob(0x11)))
        self.assertEqual(status, 0, out)
        self.assertIn('programs found: 1', out)
        self.assertIn('eboot.bin: 1', out)

    def test_add_code_missing_program(self):
        data = header() + program_key(0x11, 0x33)
        status, out, dst = self.add_code(data, ('eboot.bin', blob(0x11)))
        self.assertEqual(status, 0, out)
        self.assertIn('programs without code: 1', out)
        self.assertIn('0x0000000000000033', out)
        _, entries = info.read_list(dst.read_bytes())
        self.assertEqual([e[0] for e in entries], [info.CODE, info.KEY])

    def test_add_code_skips_bad_archive(self):
        good = psarc(bytes(40) + blob(0x11))
        status, out, dst = self.add_code(header() + program_key(0x11), ('data00.psarc', good),
                                         ('data01.psarc', b''),
                                         ('data02.psarc', b'NOPE' + good[4:]),
                                         ('data03.psarc', good[:40]))
        self.assertEqual(status, 0, out)
        for name in ('data01', 'data02', 'data03'):
            self.assertIn(f'{name}.psarc', out)
        self.assertEqual(out.count('skipped: '), 3, out)
        self.assertIn('programs found: 1', out)

    def test_add_code_wide_block_table(self):
        a = blob(0x11)
        archive = psarc(bytes((128 << 10) - 24) + a + bytes(10), block=128 << 10)
        status, out, dst = self.add_code(header() + program_key(0x11), ('data00.psarc', archive))
        self.assertEqual(status, 0, out)
        self.assertIn('programs found: 1', out)
        _, entries = info.read_list(dst.read_bytes())
        self.assertEqual(entries[0][1][28:], a)

    def test_add_code_refuses_list_with_code(self):
        status, out, dst = self.add_code(header() + code() + program_key(0x11),
                                         ('eboot.bin', blob(0x11)))
        self.assertEqual(status, 1, out)
        self.assertFalse(dst.exists())


if __name__ == '__main__':
    unittest.main()
