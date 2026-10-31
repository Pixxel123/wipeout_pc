"""Fake PSARC archives and GNF textures for the Python tests (the formats: scripts/psarc.py and
scripts/gnf.py)."""
import hashlib
from pathlib import Path
import lzma
import random
import struct
import zlib

import gnf


def archive(path, files, *, block=65536, compression=b'zlib', separator=b'\n', prefix='/', hashed=str.upper,
            packed=True, md5_order=False):
    """A PSARC 1.4 archive (`compression` b'zlib' or b'lzma') of {name: bytes} with its manifest
    (prefix + each name, joined by `separator`), entries found by the MD5 of hashed(name). A block is stored compressed when that
    makes it smaller; `packed` False stores every block raw, or a set of names packs only those.
    md5_order puts the entries in the order of their MD5s, as WipEout's archives are. The stored
    sizes take as many bytes as the block size needs, as in scripts/psarc.py."""
    pack = (lambda b: zlib.compress(b, 1)) if compression == b'zlib' else \
        (lambda b: lzma.compress(b, format=lzma.FORMAT_ALONE))
    width = 2 if block <= 0x10000 else 3 if block <= 0x1000000 else 4
    manifest = separator.join((prefix + name).encode() for name in files)
    stored = [(name, hashlib.md5(hashed(name).encode()).digest(), data) for name, data in files.items()]
    if md5_order:
        stored.sort(key=lambda item: item[1])
    stored.insert(0, (None, bytes(16), manifest))
    entry_size, count = 30, len(stored)
    blocks = [[data[i:i + block] for i in range(0, len(data), block)] for _n, _h, data in stored]
    toc_len = 32 + count * entry_size + width * sum(len(b) for b in blocks)
    entries, sizes, body, first = b'', b'', b'', 0
    for (name, digest, data), parts in zip(stored, blocks):
        packs = packed if isinstance(packed, bool) else name in packed
        entries += digest + struct.pack('>I', first) + len(data).to_bytes(5, 'big') + (toc_len + len(body)).to_bytes(5, 'big')
        for part in parts:
            small = pack(part) if packs else part
            kept = small if len(small) < len(part) else part
            sizes += (len(kept) % block).to_bytes(width, 'big')  # 0 = a full raw block
            body += kept
        first += len(parts)
    head = struct.pack('>4sHH4sIIIII', b'PSAR', 1, 4, compression, toc_len, entry_size, count, block, 1)
    path.write_bytes(head + entries + sizes + body)


def texture(fmt, width, height, pitch=None, mips=1, fill=b'\0'):
    """A GNF file as the game's are: 1D thin tiling, every level padded to whole 8x8-element tiles,
    its data `fill` repeated (None: random bytes, which do not compress)."""
    pitch = pitch or width
    side, block = gnf.FORMATS[fmt][1:]
    size = 0
    for m in range(mips):
        pe = -(-(-(-max(1, pitch >> m) // side)) // 8) * 8
        he = -(-(-(-max(1, height >> m) // side)) // 8) * 8
        size += pe * he * block
    words = [0, fmt << 20 | 8, (width - 1) | (height - 1) << 14 | 0x70000000, 13 << 20 | (mips - 1) << 16 | 0x96000000,
             (pitch - 1) << 13, 0, 0, size >> 8]
    head = b'GNF ' + struct.pack('<I', 0xf8) + bytes([2, 1, 8, 0]) + struct.pack('<I', size + 0x100)
    rng = random.Random(1)
    data = bytes(rng.getrandbits(8) for _ in range(size)) if fill is None else (fill * size)[:size]
    return (head + struct.pack('<8I', *words)).ljust(0x100, b'\0') + data


def apply(data, lines):
    """What the runtime reads after GOW3_FILE_PATCHES lines (src/runtime_file.c): `data` with each
    change, its original bytes checked."""
    import psarc
    data = bytearray(data)
    for line in lines:
        parts = line.rstrip('\n').split(' ', 4)
        offset = int(parts[1], 16)
        if len(parts) == 5 and parts[4].startswith('@'):  # length fnv @side-file
            length, fnv, new = int(parts[2], 16), int(parts[3], 16), Path(parts[4][1:]).read_bytes()
            assert len(new) == length and psarc.fnv1a(bytes(data[offset:offset + length])) == fnv, line[:80]
        else:  # original new, in hex
            original, new = bytes.fromhex(parts[2]), bytes.fromhex(parts[3])
            assert bytes(data[offset:offset + len(original)]) == original, line[:80]
        data[offset:offset + len(new)] = new
    return bytes(data)
