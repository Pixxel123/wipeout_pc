import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
import psarc
from psarc_fixtures import archive as fixture



def archive(path, files, separator='\n', md5_order=False):
    """16-byte blocks, names as written: every block of a short file is its own."""
    fixture(path, files, block=16, separator=separator.encode(), prefix='', md5_order=md5_order)


FILES = {'data/ui/buttons/cross.tex': b'\0' * 40 + b'X',
         'data/ui/menu.xml': b'<Menu>abcdefghijklmnopqrstuvwxyz</Menu>'}


class PsarcTests(unittest.TestCase):
    def test_names_and_contents_compressed_or_stored(self):
        with tempfile.TemporaryDirectory() as folder:
            archive(Path(folder) / 'data00.psarc', FILES)
            a = psarc.Archive(Path(folder) / 'data00.psarc')
            self.assertEqual([e['name'] for _i, e in a.named()], list(FILES))
            for i, e in a.named():
                self.assertEqual(a.read(i), FILES[e['name']])
            self.assertTrue(a.compressed(1))
            self.assertFalse(a.compressed(2))

    def test_names_go_to_the_entries_with_their_md5(self):
        # WipEout's TOC is in the order of the MD5s of the names, not in the manifest's order.
        files = {f'data/fe/images/icon_{n}.gnf': f'contents of {n}'.encode() for n in range(12)}
        with tempfile.TemporaryDirectory() as folder:
            archive(Path(folder) / 'data00.psarc', files, separator='\0', md5_order=True)
            a = psarc.Archive(Path(folder) / 'data00.psarc')
            self.assertNotEqual([e['name'] for _i, e in a.named()], list(files))  # the test shuffles them
            self.assertEqual(a.unmatched, 0)
            for i, e in a.named():
                self.assertEqual(a.read(i), files[e['name']])

    def test_a_manifest_separated_by_nul_bytes(self):
        # WipEout's archives separate the names of their manifest by NUL bytes, not newlines.
        with tempfile.TemporaryDirectory() as folder:
            archive(Path(folder) / 'data00.psarc', FILES, separator='\0')
            a = psarc.Archive(Path(folder) / 'data00.psarc')
            self.assertEqual([e['name'] for _i, e in a.named()], list(FILES))
            self.assertEqual(a.read(2), FILES['data/ui/menu.xml'])

    def test_find_and_extract_from_the_command_line(self):
        with tempfile.TemporaryDirectory() as folder:
            archive(Path(folder) / 'data09.psarc', FILES)
            script = str(ROOT / 'scripts' / 'psarc.py')
            found = subprocess.run([sys.executable, script, 'find', folder], capture_output=True, text=True, check=True)
            self.assertIn('data/ui/buttons/cross.tex', found.stdout)
            self.assertNotIn('menu.xml\n', found.stdout)
            out = Path(folder) / 'out'
            subprocess.run([sys.executable, script, 'extract', str(Path(folder) / 'data09.psarc'), r'\.xml$', str(out)],
                           capture_output=True, check=True)
            self.assertEqual((out / 'data/ui/menu.xml').read_bytes(), FILES['data/ui/menu.xml'])


class RepackTests(unittest.TestCase):
    """Entry changes packed into the bytes the original takes (Archive.repack)."""

    def changed(self, path, changes):
        data = bytearray(path.read_bytes())
        for offset, original, new in changes:
            self.assertEqual(bytes(data[offset:offset + len(original)]), original)
            data[offset:offset + len(new)] = new
        path.write_bytes(data)
        return psarc.Archive(path)

    def test_a_changed_entry_reads_back_compressed_or_raw(self):
        content = b'<Screen>' + b' pad ' * 30000 + b'</Screen>'
        for packed in (True, False):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'data09.psarc'
                fixture(path, {'data/a.xml': content, 'data/b.bin': bytes(range(256)) * 4}, packed=packed)
                a = psarc.Archive(path)
                self.assertEqual(a.read(a.find('DATA/A.XML')), content)
                new = content.replace(b' pad ', b' PAD ')
                again = self.changed(path, a.repack(a.find('data/a.xml'), new))
                self.assertEqual(again.read(again.find('/data/a.xml')), new)
                self.assertEqual(again.read(again.find('data/b.bin')), bytes(range(256)) * 4)

    def test_what_does_not_fit_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'data09.psarc'
            fixture(path, {'data/a.bin': bytes(100000)})
            a = psarc.Archive(path)
            noise = bytes((i * 7919 + i // 3) % 251 for i in range(100000))
            self.assertIsNone(a.repack(a.find('data/a.bin'), noise))
            with self.assertRaises(ValueError):
                a.repack(a.find('data/a.bin'), bytes(10))

    def test_lzma_and_other_name_separators(self):
        for compression, separator in ((b'lzma', b'\n'), (b'zlib', b'\r'), (b'zlib', b'\r\n')):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'data00.psarc'
                fixture(path, FILES, block=64, compression=compression, separator=separator, md5_order=True)
                a = psarc.Archive(path)
                self.assertEqual({e['name'].lstrip('/'): a.read(i) for i, e in a.named()}, FILES, (compression, separator))

    def test_patch_lines(self):
        with tempfile.TemporaryDirectory() as folder:
            small = (16, b'ab', b'cd')
            large = (4096, bytes(600), bytes(range(256)) * 2 + bytes(88))
            lines = psarc.patch_lines('data08.psarc', [small, large], folder)
            self.assertEqual(lines[0], '/app0/data08.psarc 10 6162 6364\n')
            guest, offset, length, fnv, side = lines[1].rstrip('\n').split(' ', 4)
            self.assertEqual((guest, offset, length, int(fnv, 16)), ('/app0/data08.psarc', '1000', '258', psarc.fnv1a(bytes(600))))
            self.assertEqual(Path(side[1:]).read_bytes(), large[2])

    def test_cached_lines_are_made_once_per_key(self):
        made = []

        def make(folder):
            made.append(folder)
            return ['line\n']
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / 'cache'
            self.assertEqual(psarc.cached_lines(target, [1], make), ['line\n'])
            self.assertEqual(psarc.cached_lines(target, [1], make), ['line\n'])
            psarc.cached_lines(target, [2], make)
            self.assertEqual(len(made), 2)


if __name__ == '__main__':
    unittest.main()
