# SPDX-License-Identifier: GPL-2.0-or-later
"""WipEout's PSARC archives (data00.psarc ... data09.psarc): the names they hold, their entries,
and runtime file patches that change an entry without writing the archive (GOW3_FILE_PATCHES,
src/runtime_file.c).

PSARC (big-endian): a 32-byte header (PSAR, version, compression zlib or lzma, TOC length, TOC
entry size, entry count, block size, flags), then the TOC: per entry the MD5 of its name, its
first block, its size (5 bytes) and its offset (5 bytes); then one stored size per block, as wide
as the block size needs (0 = a full block). A block whose stored size is its full size is not
compressed; the blocks of an entry follow each other from its offset. Entry 0 is the manifest: the
names of entries 1..n, one per line or (WipEout's archives) separated by NUL bytes.

Usage:
  python3 scripts/psarc.py list <archive>                        # every entry: size, stored or compressed, name
  python3 scripts/psarc.py find <game_dir> [regex]               # entries of every archive whose name matches
  python3 scripts/psarc.py extract <archive> <regex> <out_dir>   # writes the matching entries
Without a regex, find looks for names of button prompts and controller pictures.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
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
        self.width = 2 if self.block <= 0x10000 else 3 if self.block <= 0x1000000 else 4
        self.sizes_at = 32 + count * entry_size
        sizes = toc[count * entry_size:]
        self.sizes = [int.from_bytes(sizes[i:i + self.width], 'big') for i in range(0, len(sizes) - self.width + 1, self.width)]
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
        self.names = {}  # name without a leading slash, lower case -> entry
        for i, name in found.items():
            self.entries[i]['name'] = name
            self.names[name.lstrip('/').lower()] = i

    def close(self):
        self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()

    def find(self, name):
        """The entry of a name (any case, with or without a leading slash), or None."""
        return self.names.get(name.lstrip('/').lower())

    def blocks(self, i):
        """(stored size, uncompressed size) of each block of entry i."""
        e = self.entries[i]
        out = []
        for n in range((e['size'] + self.block - 1) // self.block):
            length = min(self.block, e['size'] - n * self.block)
            out.append((self.sizes[e['first'] + n] or self.block, length))
        return out

    def stored_size(self, i):
        """The bytes entry i takes in the archive."""
        return sum(stored for stored, _length in self.blocks(i))

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
            elif self.compression == b'lzma':  # imported here: WipEout's archives are zlib
                import lzma
                data += lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(raw)[:length]
            elif raw[:1] == b'\x78':
                data += zlib.decompress(raw)
            else:
                data += raw
        return bytes(data)

    def pack(self, raw):
        """A block compressed as small as this archive's compression makes it."""
        if self.compression == b'lzma':
            import lzma
            return lzma.compress(raw, format=lzma.FORMAT_ALONE, preset=9 | lzma.PRESET_EXTREME)
        packed = []
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
            z = zlib.compressobj(9, zlib.DEFLATED, 15, 9, strategy)
            packed.append(z.compress(raw) + z.flush())
        return min(packed, key=len)

    def repack(self, i, content):
        """[(offset, original, new)]: the changes of the archive's bytes that make entry i read as
        `content` (the same length), its blocks packed into the bytes the original's take. None
        when they do not fit."""
        e = self.entries[i]
        if len(content) != e['size']:
            raise ValueError(f'{len(content)} bytes for an entry of {e["size"]}')
        blocks, sizes, at = bytearray(), bytearray(), 0
        for _stored, length in self.blocks(i):
            raw = content[at:at + length]
            at += length
            packed = self.pack(raw)
            kept = packed if len(packed) < length else raw  # a packed block as long as a raw one would read as raw
            blocks += kept
            sizes += (len(kept) % (256 ** self.width)).to_bytes(self.width, 'big')
        if len(blocks) > sum(stored for stored, _length in self.blocks(i)):
            return None
        sizes_at = self.sizes_at + e['first'] * self.width
        return [(sizes_at, self._bytes(sizes_at, len(sizes)), bytes(sizes)),
                (e['offset'], self._bytes(e['offset'], len(blocks)), bytes(blocks))]

    def _bytes(self, offset, length):
        self.file.seek(offset)
        return self.file.read(length)

    def named(self):
        return [(i, e) for i, e in enumerate(self.entries) if 'name' in e]


def md5(name):
    return hashlib.md5(name.encode('utf-8')).digest()


def variants(name):
    """The spellings of a name an archive may have hashed: as written, upper case, with a leading slash."""
    bare = name.lstrip('/')
    return (name, name.upper(), bare, bare.upper(), '/' + bare, ('/' + bare).upper(), bare.lower(), '/' + bare.lower())


# ------------------------------------------------------------------------------- which archives

def add_archive_option(parser):
    parser.add_argument('--archive', nargs=2, action='append', metavar=('NAME', 'FILE'),
                        help='an archive the game opens as /app0/NAME, read from FILE (repeated; run.py '
                             'passes them, a drive without links keeps some outside the game folder); '
                             'by default those in the game folder')


def archives(game, given=None):
    """(name, file) of each archive by name: the name the game opens (/app0/NAME) and the file
    read; by default the game folder's."""
    if given is None:
        given = [(p.name, p) for p in Path(game).iterdir() if p.suffix.lower() == '.psarc' and p.is_file()]
    return sorted((name, Path(path)) for name, path in given)


# ------------------------------------------------------------------------------- runtime patches

def fnv1a(data):
    """FNV-1a, 64 bits: the runtime checks a large change's original bytes by it."""
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def patch_lines(name, changes, folder):
    """GOW3_FILE_PATCHES lines for (offset, original, new) changes of archive `name`: small ones
    inline in hex, larger ones from side files written in `folder`."""
    lines = []
    for offset, original, new in changes:
        if len(new) <= 512:
            lines.append(f'/app0/{name} {offset:x} {original.hex()} {new.hex()}\n')
        else:
            side = Path(folder) / f'{Path(name).stem}-{offset:x}.bin'
            side.write_bytes(new)
            lines.append(f'/app0/{name} {offset:x} {len(new):x} {fnv1a(original):x} @{side.resolve()}\n')
    return lines


def cached_lines(folder, key, make):
    """The patch lines make(folder) returns, kept in `folder` with `key` (anything JSON writes):
    made again only when the key changes. make raises to leave nothing behind."""
    folder = Path(folder)
    stamp_file, kept = folder / 'stamp.json', folder / 'patches.txt'
    key = json.dumps(key)
    if stamp_file.is_file() and stamp_file.read_text() == key and kept.is_file():
        return kept.read_text().splitlines(keepends=True)
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    lines = make(folder)
    kept.write_text(''.join(lines))
    stamp_file.write_text(key)  # last: an interrupted build is redone
    return lines


def stamp(paths):
    """What a cache key notes of files: path, size and modification time."""
    return [(str(p), p.stat().st_size, p.stat().st_mtime_ns) for p in paths]


# ------------------------------------------------------------------------------- command line

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
        a = Archive(args.archive)  # closed when the script ends, as the others below
        print(f'{a.path.name}: PSARC {a.major}.{a.minor}, {a.compression.decode()}, {len(a.entries)} entries, '
              f'{a.block} byte blocks, flags {a.flags:#x}, {a.unmatched} names without an entry')
        for i, _e in a.named():
            print(describe(a, i))
        return 0
    if args.command == 'find':
        pattern = re.compile(args.regex, re.I)
        for _name, path in archives(args.game):
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
