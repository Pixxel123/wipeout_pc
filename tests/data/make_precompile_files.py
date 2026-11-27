"""Writes tests/data/precompile_files/: two small PSARC archives of invented bytes, built the way
the game builds its archives, for tests/test_precompile_files.cpp. Run by hand from the repository
root (python3 -I tests/data/make_precompile_files.py); the output is committed.

data00.psarc: 64-byte blocks, flags 1 (ignore case), the manifest first (NUL separated), then the
entries sorted by the MD5 of their upper-cased name:
  a.bin  a full block of noise (stored, 0 in the size table), then program 0x11
  b.bin  programs 0x22 and 0x33 with zeros around them (zlib blocks); its TOC entry twice, both
         pointing at the same bytes, as data08.psarc holds some
  c.bin  noise only (a short stored block)
data01.psarc: 128 KiB blocks (a 3-byte size table), big.bin: zeros, then program 0x44 across the
first block's end.
"""
import hashlib
import random
import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent / 'precompile_files'


def blob(program, body=(1, 2, 3, 4)):
    """A program's code as the game's files hold it: the start token, its second word putting the
    OrbShdr header after `body`, then the header with length, hash and crc32."""
    words = len(body) + 2
    start = struct.pack('<II', 0xBEEB03FF, words // 2 - 1) + struct.pack(f'<{len(body)}I', *body)
    length = (words + 8) * 4
    head = b'OrbShdr' + bytes(1) + struct.pack('<IIQ', length << 8, 0, program)
    return start + head + struct.pack('<II', zlib.crc32(start + head[:16]), 0)


def noise(size):
    """Bytes zlib cannot shrink: a block of them is stored."""
    return random.Random(size).randbytes(size)


def psarc(names, files, block, twice=()):
    """A PSARC holding `files` named `names` (manifest first, then sorted by MD5 of the upper-cased
    name, flags 1); each block zlib, or stored when that is not smaller (a full block stored is 0
    in the size table). The entries named in `twice` have a second TOC entry, same bytes.
    Returns the archive and its size table."""
    md5 = {n: hashlib.md5(n.upper().encode()).digest() for n in names}
    order = sorted(range(len(files)), key=lambda i: md5[names[i]])
    entries = [(bytes(16), '\0'.join(names).encode())]
    entries += [(md5[names[i]], files[i]) for i in order]
    width = 2 if block <= 1 << 16 else 3
    toc, sizes, data = [], [], b''
    for hash_, file in entries:
        toc.append((hash_, len(sizes), len(file), len(data)))
        for at in range(0, len(file), block):
            part = file[at:at + block]
            packed = zlib.compress(part)
            if len(packed) >= len(part):
                packed = part
            sizes.append(0 if len(packed) == block else len(packed))
            data += packed
    toc = [t for t in toc for _ in range(2 if t[0] in {md5[n] for n in twice} else 1)]
    toc_len = 32 + 30 * len(toc) + width * len(sizes)
    out = struct.pack('>4sI4sIIIII', b'PSAR', 0x10004, b'zlib', toc_len, 30, len(toc), block, 1)
    for hash_, first, size, offset in toc:
        out += hash_ + struct.pack('>I', first) + size.to_bytes(5, 'big')
        out += (offset + toc_len).to_bytes(5, 'big')
    return out + b''.join(s.to_bytes(width, 'big') for s in sizes) + data, sizes


def main():
    OUT.mkdir(exist_ok=True)
    a = noise(64) + blob(0x11)
    b = bytes(40) + blob(0x22) + bytes(8) + blob(0x33, body=tuple(range(20))) + bytes(30)
    c = noise(50)
    small, sizes = psarc(['a.bin', 'b.bin', 'c.bin'], [a, b, c], 64, twice=['b.bin'])
    assert 0 in sizes and any(0 < s < 64 for s in sizes), sizes
    big, sizes = psarc(['big.bin'], [bytes((128 << 10) - 24) + blob(0x44) + bytes(10)], 128 << 10)
    assert len(sizes) == 3 and all(0 < s < 1000 for s in sizes[1:]), sizes
    for name, data in (('data00.psarc', small), ('data01.psarc', big)):
        assert len(data) < 20 << 10
        (OUT / name).write_bytes(data)
        print(f'{name}: {len(data)} bytes')


if __name__ == '__main__':
    main()
