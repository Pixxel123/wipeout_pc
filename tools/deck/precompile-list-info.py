"""Summarise a precompile list (GOW3_PRECOMPILE_RECORD) and optionally write it without its code.

Format: gpu/shadps4/video_core/renderer_vulkan/vk_precompile_list.h. A header (magic, version, regs,
user data and stage counts), then entries: u32 body size, FNV-1a 64 of the body, body. A body starts
with a u32 kind: 0 a Key (a pipeline), 1 a Code record (a program's shader words).

  precompile-list-info.py <list> [--strip-code <out>]

Prints the version, the key entries and code records, and the bytes per kind. The kinds add up to
the file size: header counts the file header, every key entry's frame and fixed fields (kind, hash,
offsets, counts, stage ids and hashes); the Code records are counted whole, frames included.
--strip-code writes the list with every Code record left out and the other entries copied byte for
byte. Exits 1 on a bad magic, version, layout or checksum, or an entry cut off. Prints counts only.
"""
import struct
import sys

MAGIC = b"GOW3PCL\n"
VERSION = 3
HEADER = struct.Struct("<8s4I")
KEY, CODE = 0, 1
KINDS = ("header", "registers", "user data", "roots", "guest words", "pointers", "code")
USER_DATA_BYTES = 16 * 4


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class ListError(Exception):
    pass


def key_bytes(body):
    """Bytes per kind of one key body (after its kind u32), counted from its fields."""
    try:
        at = 4 + 8 + 4 + 4  # kind, key hash, vertex and instance sgpr offsets
        regs, = struct.unpack_from("<I", body, at)
        at += 4 + regs * 8
        stages, = struct.unpack_from("<I", body, at)
        at += 4
        roots = 0
        for _ in range(stages):
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


def main(argv):
    args = list(argv)
    out = None
    if "--strip-code" in args:
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
        if out:
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
