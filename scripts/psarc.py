# SPDX-License-Identifier: GPL-2.0-or-later
"""Reads WipEout's PSARC archives (data00.psarc ... data09.psarc): the names they hold, and entries.

PSARC (big-endian): a 32-byte header (PSAR, version, compression zlib or lzma, TOC length, TOC
entry size, entry count, block size, flags), then the TOC: per entry the MD5 of its name, its
first block, its size (5 bytes) and its offset (5 bytes); then one stored size per block, as wide
as the block size needs (0 = a full block). A block whose stored size is its full size is not
compressed. Entry 0 is the manifest: the names of entries 1..n, one per line or (WipEout's
archives) separated by NUL bytes.

Usage:
  python3 scripts/psarc.py list <archive>                        # every entry: size, stored or compressed, name
  python3 scripts/psarc.py find <game_dir> [regex]               # entries of every archive whose name matches
  python3 scripts/psarc.py extract <archive> <regex> <out_dir>   # writes the matching entries
Without a regex, find looks for names of button prompts and controller pictures.
"""
import argparse
import hashlib
import lzma
from pathlib import Path
import re
import struct
import sys
import zlib

BUTTONS = r'button|btn|pad|glyph|prompt|icon|cross|circle|square|triangle|controller|dualshock|input'


class Archive:
    def __init__(self, path):
        self.path = Path(path)
        self.file = open(self.path, 'rb')
        head = self.file.read(32)
        (magic, self.major, self.minor, self.compression, toc_len, entry_size, count, self.block,
         self.flags) = struct.unpack('>4sHH4sIIIII', head)
        if magic != b'PSAR':
            raise ValueError(f'{self.path.name}: not a PSARC archive')
        toc = self.file.read(toc_len - 32)
        self.entries = []
        for i in range(count):
            e = toc[i * entry_size:(i + 1) * entry_size]
            self.entries.append({'md5': e[:16], 'first': struct.unpack('>I', e[16:20])[0],
                                 'size': int.from_bytes(e[20:25], 'big'), 'offset': int.from_bytes(e[25:30], 'big')})
        width = 2 if self.block <= 0x10000 else 3 if self.block <= 0x1000000 else 4
        sizes = toc[count * entry_size:]
        self.sizes = [int.from_bytes(sizes[i:i + width], 'big') for i in range(0, len(sizes) - width + 1, width)]
        names = re.split(r'[\r\n\0]+', self.read(0).decode('utf-8', 'replace')) if count else []
        names = [n for n in names if n]
        # The TOC is in the order of the MD5s of the names, not of the manifest: a name goes to the entry
        # with its MD5 (of the name as written, in upper case or with a leading slash, as the archive
        # hashed it). Only an archive none of whose MD5s match a name is read in the manifest's order.
        by_md5 = {e['md5']: i for i, e in enumerate(self.entries) if i}
        found = {}
        for name in names:
            i = next((by_md5[m] for m in map(md5, variants(name)) if m in by_md5), None)
            if i is not None:
                found.setdefault(i, name)
        if not found:
            found = {i: name for i, name in enumerate(names[:count - 1], 1)}
        self.unmatched = len(names) - len(found)
        self.names = {}
        for i, name in found.items():
            self.entries[i]['name'] = name
            self.names[name.lower()] = i

    def blocks(self, i):
        """(stored size, uncompressed size) of each block of entry i."""
        e = self.entries[i]
        out = []
        for n in range((e['size'] + self.block - 1) // self.block):
            length = min(self.block, e['size'] - n * self.block)
            out.append((self.sizes[e['first'] + n] or self.block, length))
        return out

    def compressed(self, i):
        return any(stored != length for stored, length in self.blocks(i))

    def read(self, i):
        e = self.entries[i]
        self.file.seek(e['offset'])
        data = bytearray()
        for stored, length in self.blocks(i):
            raw = self.file.read(stored)
            if stored == length:
                data += raw
            elif self.compression == b'lzma':
                data += lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(raw)[:length]
            elif raw[:1] == b'\x78':
                data += zlib.decompress(raw)
            else:
                data += raw
        return bytes(data)

    def named(self):
        return [(i, e) for i, e in enumerate(self.entries) if 'name' in e]


def md5(name):
    return hashlib.md5(name.encode('utf-8')).digest()


def variants(name):
    """The spellings of a name an archive may have hashed: as written, upper case, with a leading slash."""
    bare = name.lstrip('/')
    return (name, name.upper(), bare, bare.upper(), '/' + bare, ('/' + bare).upper(), bare.lower(), '/' + bare.lower())


def archives(game):
    return sorted(p for p in Path(game).iterdir() if p.suffix.lower() == '.psarc')


def describe(a, i):
    e = a.entries[i]
    kind = 'compressed' if a.compressed(i) else 'stored'
    return f"{e['size']:>12}  {kind:<10}  {e['name']}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    p = sub.add_parser('list')
    p.add_argument('archive')
    p = sub.add_parser('find')
    p.add_argument('game')
    p.add_argument('regex', nargs='?', default=BUTTONS)
    p.add_argument('--magic', action='store_true', help='also print the first bytes of each entry')
    p = sub.add_parser('extract')
    p.add_argument('archive')
    p.add_argument('regex')
    p.add_argument('out')
    args = ap.parse_args()

    if args.command == 'list':
        a = Archive(args.archive)
        print(f'{a.path.name}: PSARC {a.major}.{a.minor}, {a.compression.decode()}, {len(a.entries)} entries, '
              f'{a.block} byte blocks, flags {a.flags:#x}, {a.unmatched} names without an entry')
        for i, _e in a.named():
            print(describe(a, i))
        return 0
    if args.command == 'find':
        pattern = re.compile(args.regex, re.I)
        for path in archives(args.game):
            a = Archive(path)
            found = [(i, e) for i, e in a.named() if pattern.search(e['name'])]
            kinds = {}
            for _i, e in a.named():
                ext = Path(e['name']).suffix.lower() or '(none)'
                kinds[ext] = kinds.get(ext, 0) + 1
            top = ', '.join(f'{k} {v}' for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])[:8])
            print(f'== {path.name}: {len(a.entries)} entries ({top}); {len(found)} match'
                  + (f'; {a.unmatched} names without an entry' if a.unmatched else ''))
            for i, _e in found:
                line = describe(a, i)
                if args.magic:
                    line += '  ' + a.read(i)[:16].hex(' ')
                print(line)
        return 0
    a = Archive(args.archive)
    pattern = re.compile(args.regex, re.I)
    out = Path(args.out)
    written = 0
    for i, e in a.named():
        if pattern.search(e['name']):
            target = out / e['name'].lstrip('/')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(a.read(i))
            written += 1
    print(f'{written} entries written to {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
