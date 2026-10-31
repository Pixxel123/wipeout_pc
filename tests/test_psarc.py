import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from paths import ROOT
import psarc

BLOCK = 16


def archive(path, files):
    """A PSARC 1.4 archive with 16-byte zlib blocks: the manifest, then `files` (name -> bytes).
    Blocks that do not shrink are stored, as the game's archives do."""
    names = list(files)
    contents = ['\n'.join(names).encode()] + [files[n] for n in names]
    hashes = [bytes(16)] + [hashlib.md5(n.upper().encode()).digest() for n in names]
    entry_size, sizes, body, entries = 30, [], b'', []
    blocks = sum((len(c) + BLOCK - 1) // BLOCK for c in contents)
    toc_len = 32 + entry_size * len(contents) + 2 * blocks
    for md5, data in zip(hashes, contents):
        entries.append(md5 + struct.pack('>I', len(sizes)) + len(data).to_bytes(5, 'big')
                       + (toc_len + len(body)).to_bytes(5, 'big'))
        for at in range(0, len(data), BLOCK):
            raw = data[at:at + BLOCK]
            packed = zlib.compress(raw, 9)
            stored = packed if len(packed) < len(raw) else raw
            sizes.append(len(stored) % 0x10000)
            body += stored
    head = struct.pack('>4sHH4sIIIII', b'PSAR', 1, 4, b'zlib', toc_len, entry_size, len(contents), BLOCK, 0)
    path.write_bytes(head + b''.join(entries) + b''.join(struct.pack('>H', s) for s in sizes) + body)


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


if __name__ == '__main__':
    unittest.main()
