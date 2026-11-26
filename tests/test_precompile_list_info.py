"""tools/deck/precompile-list-info.py on small lists built in memory (the layout of
vk_precompile_list.h: header, then entries of u32 size, FNV-1a 64, body)."""
import contextlib
import hashlib
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


def key(regs=2, stages=1, roots=1, words=2, pointers=1, version=3):
    body = struct.pack('<IQIII', 0, 7, 0, 0, regs) + bytes(8 * regs) + struct.pack('<I', stages)
    for _ in range(stages):
        body += struct.pack('<IIQQ', 0, 0, 1, 2) + bytes(64) + struct.pack('<I', roots)
        body += bytes(12 * roots)
    body += struct.pack('<I', words) + bytes(16 * words) + struct.pack('<I', pointers)
    return frame(body + bytes(20 * pointers) + (b'\0' if version == 4 else b''))


def code(words=4):
    return frame(struct.pack('<IQI', 1, 9, words) + bytes(4 * words))


def program_key(*programs, version=3):
    """A key entry whose stages run `programs`."""
    body = struct.pack('<IQIII', 0, 7, 0, 0, 0) + struct.pack('<I', len(programs))
    for program in programs:
        body += struct.pack('<IIQQ', 0, 0, program, 2) + bytes(64) + struct.pack('<I', 0)
    return frame(body + struct.pack('<II', 0, 0) + (b'\0' if version == 4 else b''))


# Invented fetch shaders in the shapes the engine's parser documents (fetch_shader.cpp).
def load_x4(sdst, sbase_pair, offset, imm=1):
    return 0xC0000000 | (2 << 22) | (sdst << 15) | (sbase_pair << 9) | (imm << 8) | offset


WAIT_LGKM, WAIT_ALL, RETURN = 0xBF8C007F, 0xBF8C0000, 0xBE802000


def buffer_load(count, vdata, srsrc, vaddr=0):
    return [0xE0000000 | ((count - 1) << 18) | (1 << 13), vaddr | (vdata << 8) | (srsrc << 16)]


def typed_load(count, vdata, srsrc):
    return [0xE8000000 | (13 << 19) | ((count - 1) << 16) | (1 << 13), (vdata << 8) | (srsrc << 16)]


def grouped_stub():
    """Two attributes: V#s at dword 0 and 8 of the table at s[2:3]."""
    return ([load_x4(8, 1, 0), load_x4(12, 1, 8), WAIT_LGKM] + buffer_load(4, 4, 2) +
            buffer_load(2, 8, 3) + [WAIT_ALL, RETURN])


TABLE, FETCH, BUFFERS = 0x2000_0000, 0x3000_0000, 0x4000_0000
GROUPED_ATTRIBUTES = [(0, 4, 4, 2, 0, 0, 0, 0, 0), (1, 8, 2, 2, 8, 0, 0, 0, 0)]


def stub_key(stub=None, version=3, extra_words=(), regs=(), user_data=bytes(64), fetch=None,
             vsharps=(0, 8)):
    """A vertex + fragment key whose vertex stage has a walker table (user data 0), the fetch
    shader at user data 4 and a vertex buffer table at user data 2. Version 3 holds `stub`'s
    words after the walker's, version 4 the description `fetch` instead."""
    body = struct.pack('<IQIII', 0, 7, 0, 0, len(regs)) + b''.join(
        struct.pack('<II', *r) for r in regs) + struct.pack('<I', 2)
    body += struct.pack('<IIQQ', 0, 0, 0x11, 2) + bytes(64) + struct.pack('<I', 0)
    roots = [(0, TABLE), (4, FETCH), (2, BUFFERS)]
    body += struct.pack('<IIQQ', 3, 1, 0x22, 3) + user_data + struct.pack('<I', len(roots))
    body += b''.join(struct.pack('<IQ', *r) for r in roots)
    words = [(TABLE, 0, 5), (TABLE, 4, 6), *extra_words]
    words += [(FETCH, 4 * i, w) for i, w in enumerate(stub or [])]
    words += [(BUFFERS, (d + w) * 4, 0x100 + w) for d in vsharps for w in range(4)]
    body += struct.pack('<I', len(words)) + b''.join(struct.pack('<QII', *w) for w in words)
    body += struct.pack('<I', 0)
    if version == 4:
        body += info.encode_fetch(fetch)
    return frame(body)


def grouped_fetch():
    return {'size': 4 * len(grouped_stub()), 'vertex_offset_sgpr': -1,
            'instance_offset_sgpr': -1, 'sgpr': 4,
            'attributes': [dict(zip(info.ATTRIBUTE_FIELDS, a)) for a in GROUPED_ATTRIBUTES]}


def blob(program, body=(1, 2, 3, 4)):
    """A program's code as the game's files hold it: the start token, its second word putting the
    OrbShdr header after `body`, then the header with length, hash and crc32."""
    words = len(body) + 2
    start = struct.pack('<II', 0xBEEB03FF, words // 2 - 1) + struct.pack(f'<{len(body)}I', *body)
    length = (words + 8) * 4
    head = b'OrbShdr' + bytes(1) + struct.pack('<IIQ', length << 8, 0, program)
    return start + head + struct.pack('<II', zlib.crc32(start + head[:16]), 0)


def psarc(*files, block=64, names=None):
    """A PSARC holding `files`: each block zlib, or stored when that is not smaller (a full block
    stored is 0 in the size table, whose entries are as wide as the block size needs). With `names`
    the first file is the manifest listing them, NUL separated as the game's are, and the other
    entries follow it sorted by the MD5 of their upper-cased name, which the TOC holds (flags 1)."""
    hashes = [bytes(16)] * len(files)
    if names:
        md5 = {n: hashlib.md5(n.upper().encode()).digest() for n in names}
        order = sorted(range(len(files)), key=lambda i: md5[names[i]])
        files = ('\0'.join(names).encode(), *(files[i] for i in order))
        hashes = [bytes(16)] + [md5[names[i]] for i in order]
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
    out = struct.pack('>4sI4sIIIII', b'PSAR', 0x10004, b'zlib', toc_len, 30, len(files), block,
                      1 if names else 0)
    for (first, size, offset), md5 in zip(toc, hashes):
        out += md5 + struct.pack('>I', first) + size.to_bytes(5, 'big')
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
        for line in ('version: 3', 'key entries: 2', 'code records: 2',
                     'registers: 2 records, 16 bytes', 'user data: 2 records, 128 bytes',
                     'roots: 1 records, 12 bytes', 'guest words: 2 records, 32 bytes',
                     'pointers: 1 records, 20 bytes', 'fetch words: 0 records, 0 bytes',
                     f'code: 2 records, {len(code()) + len(code(8))} bytes',
                     f'total: {len(data)} bytes'):
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
        self.assertIn('code: 0 records, 0 bytes', out)
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

    def indexed(self, wanted, *programs):
        """A game dir with data00.psarc holding a, b (as a.bin, b.bin) and a list needing `wanted`;
        returns the list data."""
        game = self.root / 'game'
        game.mkdir(exist_ok=True)
        (game / 'data00.psarc').write_bytes(psarc(blob(0x11), blob(0x22), names=['a.bin', 'b.bin']))
        (game / 'eboot.bin').write_bytes(blob(0x33))
        return header() + program_key(*wanted)

    def full_scan(self, data, *extra):
        dst = self.root / 'out.bin'
        status, out = run(self.write('in.bin', data), '--add-code', dst, self.root / 'game', *extra)
        self.assertEqual(status, 0, out)
        return out, dst

    def test_write_index_lists_each_source_once(self):
        data = self.indexed((0x11, 0x22, 0x33))
        index = self.root / 'index.txt'
        out, _ = self.full_scan(data, '--write-index', index)
        self.assertEqual(index.read_text(), 'data00.psarc\ta.bin\ndata00.psarc\tb.bin\neboot.bin\n')
        self.assertIn('files read: 3', out)
        self.assertNotIn('index:', out)

    def test_index_reads_only_listed_entries(self):
        data = self.indexed((0x11, 0x22))
        index = self.write('index.txt', b'data00.psarc\ta.bin\n')
        out, dst = self.full_scan(data, '--index', index)
        self.assertIn('programs found: 1', out)
        self.assertIn('programs without code: 1', out)
        self.assertIn('without code: 0x0000000000000022', out)
        self.assertIn('files read: 1', out)
        self.assertIn(f'index: {index}', out)
        _, entries = info.read_list(dst.read_bytes())
        self.assertEqual([e[0] for e in entries], [info.CODE, info.KEY])

    def test_index_output_equals_full_scan(self):
        data = self.indexed((0x11, 0x22, 0x33))
        index = self.root / 'index.txt'
        _, full = self.full_scan(data, '--write-index', index)
        full_bytes = full.read_bytes()
        out, dst = self.full_scan(data, '--index', index)
        self.assertEqual(dst.read_bytes(), full_bytes)
        self.assertIn('files read: 3', out)

    def test_index_missing_archive_reports_without_code(self):
        data = self.indexed((0x11, 0x33))
        index = self.write('index.txt', b'data09.psarc\ta.bin\neboot.bin\n')
        out, _ = self.full_scan(data, '--index', index)
        self.assertIn('programs found: 1', out)
        self.assertIn('without code: 0x0000000000000011', out)
        self.assertIn('files read: 1', out)

    def test_index_names_the_entry_holding_the_program(self):
        names = ['a.bin', 'b.bin', 'c.bin', 'd.bin']
        by_hash = sorted(names, key=lambda n: hashlib.md5(n.upper().encode()).digest())
        self.assertNotEqual(by_hash, names)
        game = self.root / 'game'
        game.mkdir()
        (game / 'data00.psarc').write_bytes(
            psarc(noise(50), blob(0x11), noise(60), blob(0x22), names=names))
        index = self.root / 'index.txt'
        out, _ = self.full_scan(header() + program_key(0x11, 0x22), '--write-index', index)
        self.assertEqual(index.read_text(), 'data00.psarc\tb.bin\ndata00.psarc\td.bin\n')
        out, _ = self.full_scan(header() + program_key(0x11, 0x22), '--index', index)
        self.assertIn('files read: 2', out)

    def test_write_index_reports_an_entry_without_a_name(self):
        game = self.root / 'game'
        game.mkdir()
        archive = bytearray(psarc(blob(0x11), noise(50), names=['a.bin', 'b.bin']))
        for entry in (1, 2):
            archive[32 + 30 * entry] ^= 1  # the hashes of both files match no name
        (game / 'data00.psarc').write_bytes(bytes(archive))
        index = self.root / 'index.txt'
        status, out = run(self.write('in.bin', header() + program_key(0x11)), '--add-code',
                          self.root / 'out.bin', game, '--write-index', index)
        self.assertEqual(status, 1, out)
        self.assertIn('has no name', out)
        self.assertIn('index incomplete', out)
        self.assertIn('programs found: 1', out)
        self.assertEqual(index.read_text(), '')

    def indexed_copies(self, wanted, update_archive, base_archive, index):
        """--add-code --index with data00.psarc in an update and a base dir; the tool reads the
        later dir (update) first."""
        for name, archive in (('base', base_archive), ('update', update_archive)):
            (self.root / name).mkdir()
            (self.root / name / 'data00.psarc').write_bytes(archive)
        status, out = run(self.write('in.bin', header() + program_key(*wanted)), '--add-code',
                          self.root / 'out.bin', self.root / 'base', self.root / 'update',
                          '--index', self.write('index.txt', index))
        self.assertEqual(status, 0, out)
        return out

    def test_index_keeps_only_wanted_programs(self):
        out = self.indexed_copies((0x22, 0x99), psarc(blob(0x22) + blob(0x33), names=['b.bin']),
                                  b'', b'data00.psarc\tb.bin\n')
        self.assertIn('programs found: 1', out)
        _, entries = info.read_list((self.root / 'out.bin').read_bytes())
        self.assertEqual([e[0] for e in entries], [info.CODE, info.KEY])
        self.assertEqual(struct.unpack_from('<Q', entries[0][1], 16)[0], 0x22)

    def test_index_reads_a_twice_listed_entry_once(self):
        archive = (ROOT / 'tests/data/precompile_files/data00.psarc').read_bytes()
        out = self.indexed_copies((0x22, 0x99), archive, archive, b'data00.psarc\tb.bin\n')
        self.assertIn('programs found: 1', out)
        self.assertIn('files read: 2', out)  # b.bin from each copy, once each

    def test_index_second_copy_not_opened_when_all_found(self):
        good = psarc(blob(0x11), blob(0x22), names=['a.bin', 'b.bin'])
        out = self.indexed_copies((0x11, 0x22), good, good[:20],
                                  b'data00.psarc\ta.bin\ndata00.psarc\tb.bin\n')
        self.assertNotIn('skipped', out)  # the cut copy, if read: "skipped: ...: cut off"
        self.assertIn('programs found: 2', out)
        self.assertIn('files read: 2', out)

    def test_index_program_only_in_second_copy_is_found(self):
        out = self.indexed_copies((0x11, 0x22), psarc(blob(0x11), names=['a.bin']),
                                  psarc(blob(0x11), blob(0x22), names=['a.bin', 'b.bin']),
                                  b'data00.psarc\ta.bin\ndata00.psarc\tb.bin\n')
        self.assertIn('programs found: 2', out)
        self.assertIn('programs without code: 0', out)
        self.assertIn('files read: 2', out)  # a.bin from the update's copy, b.bin from the base's

    def eboots(self, wanted, update_eboot, base_eboot, *extra):
        """--add-code with eboot.bin in an update and a base dir; the tool reads the later dir
        (update) first."""
        for name, eboot in (('base', base_eboot), ('update', update_eboot)):
            (self.root / name).mkdir()
            (self.root / name / 'eboot.bin').write_bytes(eboot)
        status, out = run(self.write('in.bin', header() + program_key(*wanted)), '--add-code',
                          self.root / 'out.bin', self.root / 'base', self.root / 'update', *extra)
        self.assertEqual(status, 0, out)
        return out

    def test_index_reads_the_first_readable_eboot_only(self):
        # The game runs the update's eboot.bin, so the base one is not read: a program only in it
        # is not found. That is the rule's cost.
        out = self.eboots((0x11, 0x22), blob(0x11), blob(0x22),
                          '--index', self.write('index.txt', b'eboot.bin\n'))
        self.assertIn('programs found: 1', out)
        self.assertIn('without code: 0x0000000000000022', out)
        self.assertIn('files read: 1', out)

    def test_index_eboot_falls_through_when_unreadable(self):
        out = self.eboots((0x22,), b'', blob(0x22),
                          '--index', self.write('index.txt', b'eboot.bin\n'))
        self.assertIn('eboot.bin: cannot read', out)
        self.assertIn('programs found: 1', out)
        self.assertIn('files read: 1', out)

    def test_full_scan_reads_every_eboot(self):
        out = self.eboots((0x11, 0x22), blob(0x11), blob(0x22))
        self.assertIn('programs found: 2', out)
        self.assertIn('files read: 2', out)

    def test_summary_v4_counts_fetch_descriptions(self):
        data = header(4) + stub_key(version=4, fetch=grouped_fetch()) + key(version=4)
        status, out = run(self.write('a.bin', data))
        self.assertEqual(status, 0, out)
        for line in ('version: 4', 'key entries: 2', 'fetch words: 0 records, 0 bytes',
                     f'fetch descriptions: 1 records, {2 + 8 + 2 * 9} bytes',
                     'guest words: 12 records, 192 bytes', f'total: {len(data)} bytes'):
            self.assertIn(line, out)

    def test_summary_v3_counts_fetch_words(self):
        data = header() + stub_key(grouped_stub())
        status, out = run(self.write('a.bin', data))
        self.assertEqual(status, 0, out)
        self.assertIn(f'fetch words: {len(grouped_stub())} records, {16 * len(grouped_stub())} '
                      'bytes', out)
        self.assertIn(f'total: {len(data)} bytes', out)

    def convert(self, data):
        dst = self.root / 'v4.bin'
        status, out = run('--convert-v4', self.write('v3.bin', data), dst)
        return status, out, dst

    def test_convert_v4_replaces_the_words_with_the_description(self):
        plain = key()
        status, out, dst = self.convert(header() + code() + stub_key(grouped_stub()) + plain)
        self.assertEqual(status, 0, out)
        self.assertIn('fetch descriptions written: 1', out)
        self.assertIn('keys without a fetch shader: 1', out)
        self.assertIn(f'fetch words removed: {len(grouped_stub())}', out)
        result = dst.read_bytes()
        version, entries = info.read_list(result)
        self.assertEqual(version, 4)
        self.assertEqual(result[:info.HEADER.size], header(4))
        self.assertEqual(entries[0][1], code())  # copied as it is
        self.assertEqual(entries[1][1], stub_key(version=4, fetch=grouped_fetch()))
        self.assertEqual(entries[2][1], frame(plain[12:] + b'\0'))
        converted = info.parse_key(entries[1][1][12:], 4)
        self.assertEqual(converted.fetch, grouped_fetch())

    def test_convert_v4_refuses_what_it_cannot_decode(self):
        refused = (
            [load_x4(8, 1, 0, imm=0), WAIT_LGKM] + buffer_load(4, 4, 2) + [RETURN],  # sgpr offset
            [load_x4(8, 1, 0), WAIT_LGKM] + typed_load(4, 4, 2) + [RETURN],  # typed load
            [load_x4(8, 1, 0), WAIT_LGKM] + buffer_load(4, 4, 3) + [RETURN],  # no V# load
            [0x7E000280, RETURN],  # a vector instruction
        )
        for stub in refused:
            data = header() + key() + stub_key(stub, vsharps=(0,))
            status, out, dst = self.convert(data)
            self.assertEqual(status, 1, out)
            self.assertIn('key 1 (entry 1)', out)
            self.assertIn('no file written', out)
            self.assertFalse(dst.exists())
        # Already version 4.
        status, out, dst = self.convert(header(4) + key(version=4))
        self.assertEqual(status, 1, out)
        self.assertFalse(dst.exists())

    def test_convert_v4_needs_the_vertex_buffer_descriptors(self):
        status, out, dst = self.convert(header() + stub_key(grouped_stub(), vsharps=(0,)))
        self.assertEqual(status, 1, out)
        self.assertIn('without their vertex buffer descriptors', out)

    def check(self, data):
        return run('--check', self.write('c.bin', data))

    def test_check_passes_a_clean_list(self):
        status, out, dst = self.convert(header() + stub_key(grouped_stub()) + key())
        self.assertEqual(status, 0, out)
        status, out = run('--check', dst)
        self.assertEqual(status, 0, out)
        for line in ('check passed', 'check, code records: 0', 'check, fetch words: 0',
                     'check, instruction-shaped words: 0', 'check, start tokens: 0',
                     'check, OrbShdr: 0', 'fetch descriptions: 1 records'):
            self.assertIn(line, out)

    def test_check_fails_each_rule(self):
        clean = stub_key(version=4, fetch=grouped_fetch())
        orb = bytearray(64)
        orb[20:27] = b'OrbShdr'
        cases = (
            (header() + stub_key(grouped_stub()), 'list version 3 is below 4'),
            (header(4) + code() + clean, '1 Code records'),
            (header(4) + stub_key(version=4, fetch=grouped_fetch(),
                                  extra_words=[(FETCH, 0, 0)]),
             '1 guest words at a fetch shader\'s root'),
            (header(4) + stub_key(version=4, fetch=grouped_fetch(),
                                  extra_words=[(TABLE, 8, 0xBE822004)]),
             '1 instruction-shaped words'),
            (header(4) + stub_key(version=4, fetch=grouped_fetch(),
                                  extra_words=[(TABLE, 8, 0xBF810000)]),
             '1 instruction-shaped words'),
            (header(4) + stub_key(version=4, fetch=grouped_fetch(),
                                  regs=[(0x2C00, 1), (0x2C01, 0xBEEB03FF)]), '1 start tokens'),
            (header(4) + stub_key(version=4, fetch=grouped_fetch(), user_data=bytes(orb)),
             '1 OrbShdr'),
        )
        for data, reason in cases:
            status, out = self.check(data)
            self.assertEqual(status, 1, out)
            self.assertIn(f'check failed: {reason}', out)
            self.assertNotIn('check passed', out)

    def test_check_reads_streams_across_words(self):
        # OrbShdr split over two consecutive registers is found; two registers apart it is not.
        word = struct.unpack('<2I', b'xOrbShdr')
        status, out = self.check(header(4) + stub_key(version=4, fetch=grouped_fetch(),
                                                      regs=[(0x10, word[0]), (0x11, word[1])]))
        self.assertIn('check failed: 1 OrbShdr', out)
        status, out = self.check(header(4) + stub_key(version=4, fetch=grouped_fetch(),
                                                      regs=[(0x10, word[0]), (0x12, word[1])]))
        self.assertEqual(status, 0, out)

    def test_strip_and_add_code_work_on_v4(self):
        k1 = stub_key(version=4, fetch=grouped_fetch())
        src, dst = self.write('a.bin', header(4) + code() + k1), self.root / 'b.bin'
        status, out = run(src, '--strip-code', dst)
        self.assertEqual(status, 0, out)
        self.assertEqual(dst.read_bytes(), header(4) + k1)
        status, out, dst = self.add_code(header(4) + program_key(0x11, version=4),
                                         ('eboot.bin', blob(0x11)))
        self.assertEqual(status, 0, out)
        self.assertIn('programs found: 1', out)
        version, entries = info.read_list(dst.read_bytes())
        self.assertEqual((version, [e[0] for e in entries]), (4, [info.CODE, info.KEY]))

    def test_shipped_list_holds_no_instruction_words(self):
        status, out = run('--check', ROOT / 'gpu/precompile/shaders.list')
        self.assertEqual(status, 0, out)
        self.assertIn('check passed', out)
        self.assertIn('fetch words: 0 records, 0 bytes', out)

    def test_index_options_need_add_code(self):
        with self.assertRaises(SystemExit):
            run(self.write('in.bin', header()), '--index', self.root / 'i.txt')


if __name__ == '__main__':
    unittest.main()
