"""Summarise a precompile list (GOW3_PRECOMPILE_RECORD) and optionally write it without its code, or
fill in a code-free list's code from the game's own files.

Format: gpu/shadps4/video_core/renderer_vulkan/vk_precompile_list.h. A header (magic, version, regs,
user data and stage counts), then entries: u32 body size, FNV-1a 64 of the body, body. A body starts
with a u32 kind: 0 a Key (a pipeline), 1 a Code record (a program's shader words). In version 4 a
key ends with its fetch shader description (the parse of the vertex stage's fetch shader). In
version 3 a key holds the fetch shader's code words among its guest words.

  precompile-list-info.py <list> [--strip-code <out>]
  precompile-list-info.py <list> --add-code <out> <game dir>... [--write-index <file>] [--index <file>]
  precompile-list-info.py --convert-v4 <v3 list> <out>
  precompile-list-info.py --check <list>

Prints the version, the key entries and code records, and a count and the bytes per kind. The bytes
of the kinds add up to the file size. "header" counts the key entries, and its bytes are the file
header and the frame and fixed fields of each key entry (kind, hash, offsets, counts, stage ids and
hashes). The bytes of "fetch descriptions" include the flag byte of each version 4 key. "fetch
words" counts the guest words that are a fetch shader's code. The tool finds them with its decoder
in a version 3 list, and at the fetch shader's root in a version 4 list. The Code records are
counted whole, frames included. --strip-code writes the list with every Code record left out
and the other entries copied byte for byte, at the list's own version. Exits 1 on a bad magic,
version, layout or checksum, or an entry cut off. Prints counts only.
--add-code writes a code-free list with each program's Code record before the first key that uses
it, as the recorder does, the code read out of the game dirs' eboot.bin and *.psarc files (eboot.bin
first, then the archives highest dataNN first, a later dir first; it stops once every program has
code). Prints the programs found per file and the hashes of those without code, never the code;
an archive it cannot read is named and skipped. Exits 1 on a list that already holds Code records.
--write-index <file> (with --add-code) writes the files the code was found in, one sorted line
each: eboot.bin, or <archive file name> TAB <path inside the archive>. --index <file> reads only
those files, each once, and only the first eboot.bin that can be read (the game runs the update's;
a later dir's is the next tried); a program they do not hold is reported without code, never
searched for elsewhere. The report ends with the number of files read.
--convert-v4 reads a version 3 list and writes it as version 4. In each key it decodes the fetch
shader's code words, removes them and adds the fetch shader description. All other bytes stay the
same. A key without fetch shader words gets no description. The decoder knows four instructions:
s_load_dwordx4 with an immediate offset, buffer_load_format_x to _xyzw, s_waitcnt and s_setpc_b64.
When the tool cannot decode the words of a key, it names the key index, writes no file and exits 1.
--check makes sure that a list holds no instruction words. It prints the table and the counts of
each rule, and exits 1 with the reasons when one of these is true:
  1. The list version is below 4.
  2. The list holds a Code record.
  3. A key has a guest word at its fetch shader's root.
  4. A register, user-data word or guest word has the shape of s_setpc_b64, s_swappc_b64 or
     s_endpgm.
  5. The reassembled registers, user data or guest words hold the program start token or OrbShdr.
The tool reassembles registers by consecutive index, user data one stage at a time, and guest words
by base in offset order.
"""
import hashlib
import re
import struct
import sys
import time
import zlib
from pathlib import Path

MAGIC = b"GOW3PCL\n"
VERSION = 4
VERSION_STUB_WORDS = 3  # keys hold the fetch shader's code words, no description
HEADER = struct.Struct("<8s4I")
KEY, CODE = 0, 1
KINDS = ("header", "registers", "user data", "roots", "guest words", "fetch words", "pointers",
         "fetch descriptions", "code")
USER_DATA_BYTES = 16 * 4
VERTEX_STAGE = 3  # SwStage::Vertex
FETCH_FIXED = struct.Struct("<IbbBB")  # size, vertex and instance offset sgprs, sgpr, attributes
ATTRIBUTE_FIELDS = ("semantic", "dest_vgpr", "num_elements", "sgpr_base", "dword_offset",
                    "instance_data", "inst_offset", "data_format", "num_format")
# Program code (vk_precompile_list.h): the start token, the OrbShdr header (8 words) at
# (code[1] + 1) * 2 words, its length in bits 8-31 of the u32 8 bytes in, its hash 16 bytes in and
# 24 bytes in the crc32 of the code from the start token through the header's first 16 bytes.
START_TOKEN = struct.pack("<I", 0xBEEB03FF)
SIGNATURE = b"OrbShdr"
HEADER_WORDS = 8
MAX_CODE_BYTES = 1 << 20
MAX_EBOOT_BYTES = 256 << 20  # an eboot.bin the engine reads whole, at most (MaxEbootBytes)


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def frame_body(body):
    return struct.pack("<IQ", len(body), fnv1a(body)) + body


class ListError(Exception):
    pass


class Key:
    """A key body's fields. `words` and `pointers` are (base, offset, value or target) tuples,
    `stages` (sw stage, hw stage, hash, spec digest, user data, roots) tuples, `words_at` the
    byte offset of the word count in the body and `fetch` the description or None."""


def parse_key(body, version):
    """The fields of a key body (with its kind). Raises ListError."""
    k = Key()
    try:
        at = 4 + 8 + 4 + 4  # kind, key hash, vertex and instance sgpr offsets
        n, = struct.unpack_from("<I", body, at)
        k.regs = list(struct.iter_unpack("<II", body[at + 4:at + 4 + n * 8]))
        at += 4 + n * 8
        n, = struct.unpack_from("<I", body, at)
        at += 4
        k.stages = []
        for _ in range(n):
            sw, hw, program, spec = struct.unpack_from("<IIQQ", body, at)
            user_data = struct.unpack_from("<16I", body, at + 24)
            at += 24 + USER_DATA_BYTES
            r, = struct.unpack_from("<I", body, at)
            roots = list(struct.iter_unpack("<IQ", body[at + 4:at + 4 + r * 12]))
            at += 4 + r * 12
            k.stages.append((sw, hw, program, spec, user_data, roots))
        k.words_at = at
        n, = struct.unpack_from("<I", body, at)
        k.words = list(struct.iter_unpack("<QII", body[at + 4:at + 4 + n * 16]))
        at += 4 + n * 16
        n, = struct.unpack_from("<I", body, at)
        k.pointers = list(struct.iter_unpack("<QIQ", body[at + 4:at + 4 + n * 20]))
        at += 4 + n * 20
        k.fetch = None
        if version >= VERSION:
            flag = body[at]
            at += 1
            if flag > 1:
                raise ListError("a key entry has a bad fetch shader flag")
            if flag:
                size, vertex, instance, sgpr, count = FETCH_FIXED.unpack_from(body, at)
                at += FETCH_FIXED.size
                if len(body) < at + 9 * count:
                    raise ListError("a key entry is cut short")
                attributes = [dict(zip(ATTRIBUTE_FIELDS, body[at + 9 * i:at + 9 * i + 9]))
                              for i in range(count)]
                at += 9 * count
                k.fetch = {"size": size, "vertex_offset_sgpr": vertex,
                           "instance_offset_sgpr": instance, "sgpr": sgpr,
                           "attributes": attributes}
    except (struct.error, IndexError):
        raise ListError("a key entry is cut short")
    if at != len(body):
        raise ListError("a key entry has the wrong length")
    k.fetch_bytes = 0 if version < VERSION else 1 + (
        FETCH_FIXED.size + 9 * len(k.fetch["attributes"]) if k.fetch else 0)
    return k


def encode_fetch(fetch):
    """The description's bytes after the pointers: the flag, then the description."""
    if fetch is None:
        return b"\0"
    out = b"\1" + FETCH_FIXED.pack(fetch["size"], fetch["vertex_offset_sgpr"],
                                    fetch["instance_offset_sgpr"], fetch["sgpr"],
                                    len(fetch["attributes"]))
    return out + b"".join(bytes(a[f] for f in ATTRIBUTE_FIELDS) for a in fetch["attributes"])


def vertex_roots(key):
    """sgpr -> target of the vertex stage's user-data pointers."""
    return {sgpr: target for sw, _, _, _, _, roots in key.stages if sw == VERTEX_STAGE
            for sgpr, target in roots}


def stub_words(key):
    """Indices of the key's guest words at its fetch shader's root (version 4): a fetch shader's
    code."""
    if key.fetch is None or key.fetch["sgpr"] not in vertex_roots(key):
        return []
    target = vertex_roots(key)[key.fetch["sgpr"]]
    end = target + max(key.fetch["size"], 4)
    return [i for i, (base, offset, _) in enumerate(key.words)
            if base == target or target <= base + offset < end]


class StubError(Exception):
    pass


def decode_fetch(values):
    """(size, attributes) of a fetch shader, as ParseFetchShader (fetch_shader.cpp) parses it.
    Raises StubError for an instruction other than s_load_dwordx4 with an immediate offset,
    buffer_load_format_x to _xyzw, s_waitcnt and s_setpc_b64, or for words after s_setpc_b64."""
    loads, attributes, size, k = {}, [], 0, 0
    while k < len(values):
        w = values[k]
        if w >> 23 == 0x17D:  # SOP1
            if (w >> 8) & 0xFF != 0x20 or w & 0xFF == 0xFF:
                raise StubError("a scalar instruction other than s_setpc_b64")
            if k + 1 != len(values):
                raise StubError("words after s_setpc_b64")
            return size + 4, attributes
        if w >> 23 == 0x17F:  # SOPP
            if (w >> 16) & 0x7F != 12:
                raise StubError("a program control instruction other than s_waitcnt")
            size, k = size + 4, k + 1
        elif w >> 27 == 0x18:  # SMRD
            sdst = (w >> 15) & 0x7F
            if (w >> 22) & 0x1F != 2 or not (w >> 8) & 1 or sdst >= 104:
                raise StubError("a scalar load other than s_load_dwordx4 with an offset")
            loads[sdst] = (w & 0xFF, ((w >> 9) & 0x3F) * 2)
            size, k = size + 4, k + 1
        elif w >> 26 == 0x38:  # MUBUF
            if k + 1 >= len(values) or (w >> 18) & 0x7F > 3:
                raise StubError("a buffer instruction other than buffer_load_format")
            w1 = values[k + 1]
            load = loads.get(((w1 >> 16) & 0x1F) * 4)
            if load is None:
                raise StubError("a buffer load without its descriptor load")
            attributes.append(dict(zip(ATTRIBUTE_FIELDS, (
                len(attributes), (w1 >> 8) & 0xFF, ((w >> 18) & 0x7F) + 1, load[1] & 0xFF,
                load[0], w1 & 0xFF, w & 0xFF, 0, 0))))
            size, k = size + 8, k + 2
        else:
            raise StubError("an instruction the decoder does not know")
    raise StubError("no s_setpc_b64")


def ends_in_return(values):
    """Whether the words end in an s_setpc_b64-shaped word: a fetch shader's last."""
    return bool(values) and values[-1] >> 23 == 0x17D and (values[-1] >> 8) & 0xFF == 0x20


def find_stub(key):
    """(first, end, description) of the version 3 key's fetch shader words, the run of words the
    recorder took after the walker's: one base, offsets 0, 4, 8 and on, which decodes to a fetch
    shader whose vertex buffer descriptors are the 4 words per attribute that follow. None
    without one. Raises StubError for a run that ends in s_setpc_b64 and does not decode."""
    roots = vertex_roots(key)
    words = key.words
    found, refused = [], None
    i = 0
    while i < len(words):
        base = words[i][0]
        if words[i][1] != 0 or base not in roots.values():
            i += 1
            continue
        j = i
        while j < len(words) and words[j][0] == base and words[j][1] == 4 * (j - i):
            j += 1
        values = [w[2] for w in words[i:j]]
        try:
            size, attributes = decode_fetch(values)
        except StubError as e:
            if ends_in_return(values):
                refused = e
            i = j
            continue
        tail = words[j:]
        ok = size == 4 * (j - i) and len(tail) == 4 * len(attributes)
        for n, a in enumerate(attributes):
            for w in range(4):
                ok = ok and tail[4 * n + w][:2] == (roots.get(a["sgpr_base"]),
                                                    (a["dword_offset"] + w) * 4)
        if ok:
            found.append((i, j, attributes))
        elif ends_in_return(values):
            refused = StubError("fetch shader words without their vertex buffer descriptors")
        i = j
    if refused or len(found) > 1:
        raise refused or StubError("more than one run of fetch shader words")
    if not found:
        return None
    i, j, attributes = found[0]
    base = words[i][0]
    sgprs = [sgpr for sgpr, target in roots.items() if target == base]
    if len(sgprs) != 1:
        raise StubError("the fetch shader's address is not one vertex-stage pointer")
    if any(b == base for b, _, _ in words[:i] + words[j:]):
        raise StubError("other words at the fetch shader's address")
    return i, j, {"size": 4 * (j - i), "vertex_offset_sgpr": -1, "instance_offset_sgpr": -1,
                  "sgpr": sgprs[0], "attributes": attributes}


def key_bytes(body, programs=None, version=VERSION):
    """(count, bytes) per kind of one key body (with its kind). Appends each stage's program hash
    to `programs`."""
    k = parse_key(body, version)
    if programs is not None:
        programs += [stage[2] for stage in k.stages]
    if version >= VERSION:
        stubs = len(stub_words(k))
    else:
        try:
            run = find_stub(k)
        except StubError:
            run = None
        stubs = run[1] - run[0] if run else 0
    roots = sum(len(stage[5]) for stage in k.stages)
    sizes = {"registers": (len(k.regs), len(k.regs) * 8),
             "user data": (len(k.stages), len(k.stages) * USER_DATA_BYTES),
             "roots": (roots, roots * 12),
             "guest words": (len(k.words) - stubs, (len(k.words) - stubs) * 16),
             "fetch words": (stubs, stubs * 16),
             "pointers": (len(k.pointers), len(k.pointers) * 20),
             "fetch descriptions": (k.fetch is not None, k.fetch_bytes)}
    sizes["header"] = (1, len(body) - sum(b for _, b in sizes.values()) + 12)
    return sizes


def read_list(data):
    """(version, entries); entries are (kind, frame bytes, per-kind (count, bytes)). Raises
    ListError."""
    if len(data) < 8 or data[:8] != MAGIC:
        raise ListError("not a precompile list (bad magic)")
    if len(data) < HEADER.size:
        raise ListError("header cut short")
    _, version, regs, user_data, stages = HEADER.unpack_from(data)
    if version not in (VERSION_STUB_WORDS, VERSION):
        raise ListError(f"version {version}, this tool reads {VERSION_STUB_WORDS} and {VERSION}")
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
            sizes = key_bytes(body, version=version)
        elif kind == CODE:
            sizes = {"code": (1, len(frame))}
        else:
            raise ListError(f"entry {len(entries)}: unknown kind")
        entries.append((kind, frame, sizes))
        at += len(frame)
    return version, entries


def summary(data):
    """(version, key entries, code records, kind -> [count, bytes])."""
    version, entries = read_list(data)
    sizes = {k: [0, 0] for k in KINDS}
    sizes["header"][1] = HEADER.size
    for _, _, s in entries:
        for k, (n, b) in s.items():
            sizes[k][0] += n
            sizes[k][1] += b
    return version, sum(e[0] == KEY for e in entries), sum(e[0] == CODE for e in entries), sizes


def print_summary(version, keys, codes, sizes):
    print(f"version: {version}")
    print(f"key entries: {keys}")
    print(f"code records: {codes}")
    for k in KINDS:
        print(f"{k}: {sizes[k][0]} records, {sizes[k][1]} bytes")
    print(f"total: {sum(b for _, b in sizes.values())} bytes")


def convert_v4(data):
    """The version 3 list `data` as version 4 bytes, and the counts of the conversion. Raises
    ListError naming the key index for a fetch shader it cannot decode."""
    version, entries = read_list(data)
    if version != VERSION_STUB_WORDS:
        raise ListError(f"version {version}: --convert-v4 reads version {VERSION_STUB_WORDS}")
    out = [data[:8] + struct.pack("<I", VERSION) + data[12:HEADER.size]]
    counts = {"descriptions": 0, "keys without": 0, "words removed": 0}
    key_index = 0
    for index, (kind, frame, _) in enumerate(entries):
        if kind != KEY:
            out.append(frame)
            continue
        body = frame[12:]
        key = parse_key(body, version)
        try:
            run = find_stub(key)
        except StubError as e:
            raise ListError(f"key {key_index} (entry {index}): {e}, no file written") from None
        key_index += 1
        if run is None:
            counts["keys without"] += 1
            out.append(frame_body(body + encode_fetch(None)))
            continue
        i, j, fetch = run
        at = key.words_at
        words = struct.unpack_from("<I", body, at)[0]
        new = (body[:at] + struct.pack("<I", words - (j - i)) + body[at + 4:at + 4 + 16 * i] +
               body[at + 4 + 16 * j:] + encode_fetch(fetch))
        counts["descriptions"] += 1
        counts["words removed"] += j - i
        out.append(frame_body(new))
    return b"".join(out), counts


START_TOKEN_WORD = 0xBEEB03FF


def instruction_shaped(w):
    """s_setpc_b64 or s_swappc_b64 (any registers), or s_endpgm."""
    return (w >> 23 == 0x17D and (w >> 8) & 0xFF in (0x20, 0x21)) or w == 0xBF810000


def runs(pairs, step):
    """The values of (position, value) pairs in position order, split where a position is not
    the previous one plus `step`."""
    out, run, last = [], [], None
    for position, value in sorted(pairs):
        if last is not None and position != last + step:
            out.append(run)
            run = []
        run.append(value)
        last = position
    if run:
        out.append(run)
    return out


def check(data):
    """(summary, rule counts, reasons it fails); no reasons for a clean list."""
    version, keys, codes, sizes = summary(data)
    _, entries = read_list(data)
    counts = {"code records": codes, "fetch words": sizes["fetch words"][0],
              "instruction-shaped words": 0, "start tokens": 0, "OrbShdr": 0}
    reasons = []
    if version < VERSION:
        reasons.append(f"list version {version} is below {VERSION}: it holds fetch shader code")
    if codes:
        reasons.append(f"{codes} Code records")
    first_hit = {}
    for index, (kind, frame, _) in enumerate(entries):
        if kind != KEY:
            continue
        key = parse_key(frame[12:], version)
        streams = runs(key.regs, 1) + [list(stage[4]) for stage in key.stages]
        by_base = {}
        for base, offset, value in key.words:
            by_base.setdefault(base, []).append((offset, value))
        for pairs in by_base.values():
            streams += runs(pairs, 4)
        for stream in streams:
            raw = struct.pack(f"<{len(stream)}I", *stream)
            for name, n in (("instruction-shaped words", sum(map(instruction_shaped, stream))),
                            ("start tokens", raw.count(START_TOKEN)),
                            ("OrbShdr", raw.count(SIGNATURE))):
                counts[name] += n
                if n:
                    first_hit.setdefault(name, index)
        if version >= VERSION and stub_words(key):
            first_hit.setdefault("fetch words", index)
    if counts["fetch words"] and version >= VERSION:
        reasons.append(f"{counts['fetch words']} guest words at a fetch shader's root (first in "
                       f"entry {first_hit['fetch words']})")
    for name in ("instruction-shaped words", "start tokens", "OrbShdr"):
        if counts[name]:
            reasons.append(f"{counts[name]} {name} (first in entry {first_hit[name]})")
    return (version, keys, codes, sizes), counts, reasons


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


def find_code(data, wanted, found, name, source):
    """Adds the code of each program in `wanted` that `data` holds to `found`: hash -> (code,
    `name`, `source`)."""
    at = data.find(SIGNATURE)
    while at != -1 and len(found) < len(wanted):
        if at + HEADER_WORDS * 4 <= len(data):
            program = struct.unpack_from("<Q", data, at + 16)[0]
            if program in wanted and program not in found:
                code = program_code(data, at)
                if code is not None:
                    found[program] = (code, name, source)
        at = data.find(SIGNATURE, at + 1)


def psarc_files(path, want=None):
    """Each file a PSARC (zlib, 30-byte TOC entries) holds as (entry number, name, unpacked bytes),
    one at a time; the manifest (entry 0, name None) first. A file's name is the manifest name whose
    MD5 (of the upper-cased name when the archive's flags have bit 0, ignore case) is the entry's
    hash, None when no name has it. With `want` (a set of names) only those files are read, each
    once as the engine reads them (data08.psarc lists some TOC entries twice). Raises ListError
    naming the file when it is not such an archive or is cut off."""
    try:
        with open(path, "rb") as f:
            head = f.read(32)
            if len(head) < 32:
                raise ListError(f"{path}: cut off")
            magic, _, compression, toc_len, entry_size, count, block, flags = struct.unpack(
                ">4sI4sIIIII", head)
            if magic != b"PSAR" or compression != b"zlib" or entry_size != 30:
                raise ListError(f"{path}: not a zlib PSARC")
            if not 32 + count * entry_size <= toc_len <= Path(path).stat().st_size:
                raise ListError(f"{path}: bad table of contents")
            toc = f.read(toc_len - 32)
            entries = []
            for i in range(count):
                e = toc[i * entry_size:(i + 1) * entry_size]
                entries.append((e[:16], struct.unpack_from(">I", e, 16)[0],
                                int.from_bytes(e[20:25], "big"), int.from_bytes(e[25:30], "big")))
            # A block size table entry is as wide as a block's size needs.
            width = 2 if block <= 1 << 16 else 3 if block <= 1 << 24 else 4
            table = toc[count * entry_size:]
            sizes = [int.from_bytes(table[i:i + width], "big")
                     for i in range(0, len(table) - width + 1, width)]

            def read(i):
                _, first, size, offset = entries[i]
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
                return b"".join(parts)[:size]

            manifest = read(0) if count else b""
            by_hash = {}
            for raw in re.split(b"[\n\0]", manifest):
                try:
                    name = raw.decode("utf-8")
                except UnicodeDecodeError:
                    continue
                if name:
                    key = name.upper() if flags & 1 else name
                    by_hash[hashlib.md5(key.encode("utf-8")).digest()] = name
            done = set()
            for i in range(count):
                name = None if i == 0 else by_hash.get(entries[i][0])
                if want is None:
                    yield i, name, manifest if i == 0 else read(i)
                elif i and name in want and name not in done:
                    done.add(name)
                    yield i, name, read(i)
    except (struct.error, IndexError):
        raise ListError(f"{path}: cut off") from None


def game_files(dirs, index=None):
    """eboot.bin files first, then the archives highest dataNN first; a later dir before an
    earlier one. With `index` (see read_index) only eboot.bin when listed and the archives it lists:
    (path, the file names to read from it, None for all) pairs."""
    eboots, archives = [], []
    for order, d in enumerate(dirs):
        if (Path(d) / "eboot.bin").is_file() and (index is None or "eboot.bin" in index):
            eboots.append((-order, Path(d) / "eboot.bin"))
        for path in Path(d).glob("*.psarc"):
            if index is not None and path.name not in index:
                continue
            number = re.search(r"(\d+)\.psarc$", path.name)
            archives.append((-(int(number.group(1)) if number else -1), -order, path))
    return [(p, None if index is None else index.get(p.name))
            for *_, p in sorted(eboots) + sorted(archives)]


def read_index(path):
    """The index file as {"eboot.bin": None, archive file name: set of paths inside it}."""
    index = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if line == "eboot.bin":
            index[line] = None
        elif line:
            archive, _, name = line.partition("\t")
            index.setdefault(archive, set()).add(name)
    return index


def add_code(data, out, dirs, index_in=None, index_out=None):
    """Writes `data` (a list without code) to `out` with the code the game files hold. With
    `index_in` only the files that index lists are read; `index_out` gets the index of the files
    the code was found in."""
    t0 = time.time()
    version, entries = read_list(data)
    if any(kind == CODE for kind, _, _ in entries):
        raise ListError("the list already holds Code records")
    programs = []
    keys = []
    for _, entry, _ in entries:
        stages = []
        key_bytes(entry[12:], stages, version)
        keys.append((entry, stages))
        programs += stages
    wanted = set(programs)
    index = read_index(index_in) if index_in else None
    found = {}
    read = 0
    unnamed = []
    eboot_read = False
    for path, want in game_files(dirs, index):
        if len(found) == len(wanted):
            break
        if path.name == "eboot.bin" and index is None:
            read += 1
            find_code(path.read_bytes(), wanted, found, str(path), "eboot.bin")
        elif path.name == "eboot.bin":
            # With an index, as the engine: the first eboot.bin that can be read, and only it.
            if eboot_read:
                continue
            try:
                eboot = path.read_bytes() if path.stat().st_size <= MAX_EBOOT_BYTES else b""
            except OSError:
                eboot = b""
            if not eboot:
                print(f"skipped: {path}: cannot read")
                continue
            eboot_read = True
            read += 1
            find_code(eboot, wanted, found, str(path), "eboot.bin")
        else:
            try:
                for number, name, file in psarc_files(path, want):
                    read += number > 0
                    before = len(found)
                    find_code(file, wanted, found, str(path),
                              f"{path.name}\t{name}" if name else None)
                    if name is None and number and len(found) > before:
                        print(f"skipped: {path}: entry {number} has no name")
                        unnamed.append(number)
                    if len(found) == len(wanted):
                        break
            except ListError as e:
                print(f"skipped: {e}")
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
    for _, name, _ in found.values():
        per_file[name] = per_file.get(name, 0) + 1
    for name, n in per_file.items():
        print(f"found in {name}: {n}")
    missing = sorted(wanted - found.keys())
    print(f"programs without code: {len(missing)}")
    for program in missing:
        print(f"without code: 0x{program:016x}")
    print(f"files read: {read}")
    if index_in:
        print(f"index: {index_in}")
    if index_out:
        Path(index_out).write_text("".join(f"{line}\n" for line in sorted(
            {source for *_, source in found.values() if source})), encoding="utf-8")
        if unnamed:
            print(f"index incomplete: {len(unnamed)} files with code have no name")
    print(f"seconds: {time.time() - t0:.0f}")
    return 1 if index_out and unnamed else 0


def main(argv):
    args = list(argv)
    if args[:1] == ["--convert-v4"]:
        if len(args) != 3:
            sys.exit(__doc__)
        try:
            with open(args[1], "rb") as f:
                converted, counts = convert_v4(f.read())
            with open(args[2], "wb") as f:
                f.write(converted)
            print_summary(*summary(converted))
        except (ListError, OSError) as e:
            print(f"error: {e}")
            return 1
        print(f"fetch descriptions written: {counts['descriptions']}")
        print(f"keys without a fetch shader: {counts['keys without']}")
        print(f"fetch words removed: {counts['words removed']}")
        return 0
    if args[:1] == ["--check"]:
        if len(args) != 2:
            sys.exit(__doc__)
        try:
            with open(args[1], "rb") as f:
                totals, counts, reasons = check(f.read())
        except (ListError, OSError) as e:
            print(f"error: {e}")
            return 1
        print_summary(*totals)
        for name, n in counts.items():
            print(f"check, {name}: {n}")
        for reason in reasons:
            print(f"check failed: {reason}")
        if not reasons:
            print("check passed: no instruction words")
        return 1 if reasons else 0
    out = None
    dirs = None
    index_in = index_out = None
    for flag in ("--index", "--write-index"):
        if flag in args:
            i = args.index(flag)
            if i + 1 >= len(args):
                sys.exit(__doc__)
            if flag == "--index":
                index_in = args[i + 1]
            else:
                index_out = args[i + 1]
            del args[i:i + 2]
    if (index_in or index_out) and "--add-code" not in args:
        sys.exit(__doc__)
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
        if dirs:
            return add_code(data, out, dirs, index_in, index_out)
        totals = summary(data)
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
    print_summary(*totals)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
