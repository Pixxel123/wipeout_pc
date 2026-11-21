"""Summarise a precompile list (GOW3_PRECOMPILE_RECORD) and optionally write it without its code, or
fill in a code-free list's code from the game's own files.

Format: gpu/shadps4/video_core/renderer_vulkan/vk_precompile_list.h. A header (magic, version, regs,
user data and stage counts), then entries: u32 body size, FNV-1a 64 of the body, body. A body starts
with a u32 kind: 0 a Key (a pipeline), 1 a Code record (a program's shader words).

  precompile-list-info.py <list> [--strip-code <out>]
  precompile-list-info.py <list> --add-code <out> <game dir>...

Prints the version, the key entries and code records, and the bytes per kind. The kinds add up to
the file size: header counts the file header, every key entry's frame and fixed fields (kind, hash,
offsets, counts, stage ids and hashes); the Code records are counted whole, frames included.
--strip-code writes the list with every Code record left out and the other entries copied byte for
byte. Exits 1 on a bad magic, version, layout or checksum, or an entry cut off. Prints counts only.
--add-code writes a code-free list with each program's Code record before the first key that uses
it, as the recorder does, the code read out of the game dirs' eboot.bin and *.psarc files (eboot.bin
first, then the archives highest dataNN first, a later dir first; it stops once every program has
code). Prints the programs found per file and the hashes of those without code, never the code.
Exits 1 on a list that already holds Code records.
"""
import re
import struct
import sys
import time
import zlib
from pathlib import Path

MAGIC = b"GOW3PCL\n"
VERSION = 3
HEADER = struct.Struct("<8s4I")
KEY, CODE = 0, 1
KINDS = ("header", "registers", "user data", "roots", "guest words", "pointers", "code")
USER_DATA_BYTES = 16 * 4
# Program code (vk_precompile_list.h): the start token, the OrbShdr header (8 words) at
# (code[1] + 1) * 2 words, its length in bits 8-31 of the u32 8 bytes in, its hash 16 bytes in and
# 24 bytes in the crc32 of the code from the start token through the header's first 16 bytes.
START_TOKEN = struct.pack("<I", 0xBEEB03FF)
SIGNATURE = b"OrbShdr"
HEADER_WORDS = 8
MAX_CODE_BYTES = 1 << 20


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class ListError(Exception):
    pass


def key_bytes(body, programs=None):
    """Bytes per kind of one key body (after its kind u32), counted from its fields. Appends each
    stage's program hash to `programs`."""
    try:
        at = 4 + 8 + 4 + 4  # kind, key hash, vertex and instance sgpr offsets
        regs, = struct.unpack_from("<I", body, at)
        at += 4 + regs * 8
        stages, = struct.unpack_from("<I", body, at)
        at += 4
        roots = 0
        for _ in range(stages):
            if programs is not None:
                programs.append(struct.unpack_from("<Q", body, at + 8)[0])
            at += 4 + 4 + 8 + 8 + USER_DATA_BYTES
            n, = struct.unpack_from("<I", body, at)
            at += 4 + n * 12
            roots += n
        words, = struct.unpack_from("<I", body, at)
        at += 4 + words * 16
        pointers, = struct.unpack_from("<I", body, at)
        at += 4 + pointers * 20
    except struct.error:
        raise ListError("a key entry is cut short")
    if at != len(body):
        raise ListError("a key entry has the wrong length")
    sizes = {"registers": regs * 8, "user data": stages * USER_DATA_BYTES, "roots": roots * 12,
             "guest words": words * 16, "pointers": pointers * 20}
    sizes["header"] = len(body) - sum(sizes.values()) + 12
    return sizes


def read_list(data):
    """(version, entries); entries are (kind, frame bytes, per-kind sizes). Raises ListError."""
    if len(data) < 8 or data[:8] != MAGIC:
        raise ListError("not a precompile list (bad magic)")
    if len(data) < HEADER.size:
        raise ListError("header cut short")
    _, version, regs, user_data, stages = HEADER.unpack_from(data)
    if version != VERSION:
        raise ListError(f"version {version}, this tool reads {VERSION}")
    if (regs, user_data, stages) != (0xD000, 16, 6):
        raise ListError("other layout")
    entries, at = [], HEADER.size
    while at < len(data):
        if len(data) - at < 12:
            raise ListError(f"entry {len(entries)} cut off")
        size, checksum = struct.unpack_from("<IQ", data, at)
        frame = data[at:at + 12 + size]
        body = frame[12:]
        if len(body) < size:
            raise ListError(f"entry {len(entries)} cut off")
        if fnv1a(body) != checksum:
            raise ListError(f"entry {len(entries)}: checksum mismatch")
        kind = struct.unpack_from("<I", body)[0] if size >= 4 else None
        if kind == KEY:
            sizes = key_bytes(body)
        elif kind == CODE:
            sizes = {"code": len(frame)}
        else:
            raise ListError(f"entry {len(entries)}: unknown kind")
        entries.append((kind, frame, sizes))
        at += len(frame)
    return version, entries


def summary(data):
    version, entries = read_list(data)
    sizes = dict.fromkeys(KINDS, 0)
    sizes["header"] = HEADER.size
    for _, _, s in entries:
        for k, n in s.items():
            sizes[k] += n
    return version, sum(e[0] == KEY for e in entries), sum(e[0] == CODE for e in entries), sizes


def program_code(data, header):
    """The code of the program whose OrbShdr header is at `header` in `data`, cut as
    ReadProgramCode does, or None: the nearest start token below whose second word puts the
    header there and whose code passes CheckCode and CheckCodeBody (the hash is the header's)."""
    start = header
    while True:
        start = data.rfind(START_TOKEN, max(0, header - MAX_CODE_BYTES), start)
        if start < 0:
            return None
        at = (struct.unpack_from("<I", data, start + 4)[0] + 1) * 2
        if at * 4 != header - start or (at + HEADER_WORDS) * 4 > MAX_CODE_BYTES:
            continue
        length = struct.unpack_from("<I", data, header + 8)[0] >> 8
        size = max(at + HEADER_WORDS, (length + 3) // 4) * 4
        if size > MAX_CODE_BYTES or start + size > len(data):
            continue
        code = data[start:start + size]
        if length == 0 or length > size:
            continue
        if zlib.crc32(code[:at * 4 + 16]) != struct.unpack_from("<I", code, at * 4 + 24)[0]:
            continue
        return code


def find_code(data, wanted, found, name):
    """Adds the code of each program in `wanted` that `data` holds to `found`: hash -> (code,
    `name`)."""
    at = data.find(SIGNATURE)
    while at != -1 and len(found) < len(wanted):
        if at + HEADER_WORDS * 4 <= len(data):
            program = struct.unpack_from("<Q", data, at + 16)[0]
            if program in wanted and program not in found:
                code = program_code(data, at)
                if code is not None:
                    found[program] = (code, name)
        at = data.find(SIGNATURE, at + 1)


def psarc_files(path):
    """Each file a PSARC (version 1.4, zlib) holds, unpacked, one at a time."""
    with open(path, "rb") as f:
        _, _, _, toc_len, entry_size, count, block, _ = struct.unpack(">4sI4sIIIII", f.read(32))
        toc = f.read(toc_len - 32)
        entries = []
        for i in range(count):
            e = toc[i * entry_size:(i + 1) * entry_size]
            entries.append((struct.unpack_from(">I", e, 16)[0], int.from_bytes(e[20:25], "big"),
                            int.from_bytes(e[25:30], "big")))
        table = toc[count * entry_size:]
        sizes = [int.from_bytes(table[i:i + 2], "big") for i in range(0, len(table) - 1, 2)]
        for first, size, offset in entries:
            f.seek(offset)
            parts, got, index = [], 0, first
            while got < size:
                stored = sizes[index] or block
                part = f.read(stored)
                if stored != block:
                    try:
                        part = zlib.decompress(part)
                    except zlib.error:  # a short last block kept as it is
                        pass
                parts.append(part)
                got += len(part)
                index += 1
            yield b"".join(parts)[:size]


def game_files(dirs):
    """eboot.bin files first, then the archives highest dataNN first; a later dir before an
    earlier one."""
    eboots, archives = [], []
    for order, d in enumerate(dirs):
        if (Path(d) / "eboot.bin").is_file():
            eboots.append((-order, Path(d) / "eboot.bin"))
        for path in Path(d).glob("*.psarc"):
            number = re.search(r"(\d+)\.psarc$", path.name)
            archives.append((-(int(number.group(1)) if number else -1), -order, path))
    return [p for *_, p in sorted(eboots)] + [p for *_, p in sorted(archives)]


def add_code(data, out, dirs):
    """Writes `data` (a list without code) to `out` with the code the game files hold."""
    t0 = time.time()
    _, entries = read_list(data)
    if any(kind == CODE for kind, _, _ in entries):
        raise ListError("the list already holds Code records")
    programs = []
    keys = []
    for _, entry, _ in entries:
        stages = []
        key_bytes(entry[12:], stages)
        keys.append((entry, stages))
        programs += stages
    wanted = set(programs)
    found = {}
    for path in game_files(dirs):
        if len(found) == len(wanted):
            break
        if path.name == "eboot.bin":
            find_code(path.read_bytes(), wanted, found, str(path))
        else:
            for file in psarc_files(path):
                find_code(file, wanted, found, str(path))
                if len(found) == len(wanted):
                    break
    with open(out, "wb") as f:
        f.write(data[:HEADER.size])
        written = set()
        for entry, stages in keys:
            for program in stages:
                if program in found and program not in written:
                    written.add(program)
                    code = found[program][0]
                    body = struct.pack("<IQI", CODE, program, len(code) // 4) + code
                    f.write(struct.pack("<IQ", len(body), fnv1a(body)) + body)
            f.write(entry)
    print(f"programs named: {len(wanted)}")
    print(f"programs found: {len(found)}")
    per_file = {}
    for _, name in found.values():
        per_file[name] = per_file.get(name, 0) + 1
    for name, n in per_file.items():
        print(f"found in {name}: {n}")
    missing = sorted(wanted - found.keys())
    print(f"programs without code: {len(missing)}")
    for program in missing:
        print(f"without code: 0x{program:016x}")
    print(f"seconds: {time.time() - t0:.0f}")


def main(argv):
    args = list(argv)
    out = None
    dirs = None
    if "--add-code" in args:
        i = args.index("--add-code")
        if i + 2 >= len(args):
            sys.exit(__doc__)
        out, dirs = args[i + 1], args[i + 2:]
        del args[i:]
    elif "--strip-code" in args:
        i = args.index("--strip-code")
        if i + 1 >= len(args):
            sys.exit(__doc__)
        out = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    try:
        with open(args[0], "rb") as f:
            data = f.read()
        version, keys, codes, sizes = summary(data)
        if dirs:
            add_code(data, out, dirs)
        elif out:
            _, entries = read_list(data)
            with open(out, "wb") as f:
                f.write(data[:HEADER.size])
                for kind, frame, _ in entries:
                    if kind != CODE:
                        f.write(frame)
    except (ListError, OSError) as e:
        print(f"error: {e}")
        return 1
    print(f"version: {version}")
    print(f"key entries: {keys}")
    print(f"code records: {codes}")
    for k in KINDS:
        print(f"bytes, {k}: {sizes[k]}")
    print(f"bytes, total: {sum(sizes.values())}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
