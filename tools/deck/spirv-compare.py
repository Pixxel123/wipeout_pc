"""Compare the SPIR-V of two shader stores per program hash (precompile gate 1): a store built
by racing and one built at start from a precompile list (GOW3_PRECOMPILE_BOOT).

Permutation numbers differ between the two, so each program is compared as the set of its
permutations, each one the pair (its spec, its SPIR-V): <hash>_<index>.spv with the spec bytes
of its meta, <HashCombine(hash, index)>.meta. A meta whose spec sits with the wrong SPIR-V
therefore counts as a difference. The rest of a meta (the program's Info, with flattened user
data and walker bytes from whichever draw came first) is left out.
A boot-built pair that the race-built store lacks for the same program is a difference, and so
is a program only the boot-built store has; race-only ones are permutations the list did not
cover. A meta without its SPIR-V (a write cut short) is named and fails the check.

Reading a meta (vk_pipeline_serialization.cpp RegisterShaderMeta, StageSpecialization and
Info::Serialize, common/serdes.h): meta version u32, binary version u32, perm hash u64, perm
index u64, then the spec: Bindings and RuntimeInfo raw, the resource bitset as a string (u64
length, '0'/'1' bytes), the fetch data (u64 size, 0 for none; else u32 size, two s8, u64 count
and the attributes raw), five vectors (u64 count, elements raw); then the Info: InfoPersistent
raw, the flattened user data (u64 count, u32 words), PersistentSrtInfo raw (walker pointer,
walker size u64, flat size u32) and the walker bytes. A meta with fetch data may end in spare
zero bytes (the Grow in FetchShaderData::Serialize; one in this build), which broke the
earlier fixed-offset reading of every vertex shader meta. Several structs are written raw
without their size, so no size is assumed here: the bitset string and the walker tail anchor
each meta, and the raw sizes are measured over all metas of both stores, which must agree on
one layout. The search tries element sizes up to MAX_ELEMENT; a vector whose count is 0 in
every meta has no size to measure ('-'), and sizes the metas cannot tell apart stop the tool.

Exits 1 on a difference, a boot-only program, an unreadable meta, a meta without its SPIR-V,
an empty boot store or no common program, and 2 when the metas do not give one layout (nothing
is compared then). Prints hashes and counts only.

usage: python3 -I spirv-compare.py <race-built store dir> <boot-built store dir> [--all]"""
import hashlib, os, re, struct, sys

MASK = (1 << 64) - 1
HEADER = 24  # meta version, binary version, perm hash, perm index
SPEC_VECTORS = ("fetch attribute", "vertex attribute", "buffer", "image", "fmask", "sampler")
MAX_ELEMENT = 64  # largest element size tried for a raw vector element
MAX_PAD = 16  # spare bytes at the end of a meta
MAX_SRT = 64  # largest PersistentSrtInfo tried
MAX_SEARCH = 200000  # layout search steps before giving up


class LayoutError(Exception):
    pass


def hash_combine(seed, value):
    return seed ^ ((value + 0x9E3779B9 + ((seed << 12) & MASK) + (seed >> 4)) & MASK)


def u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def u64(data, at):
    return struct.unpack_from("<Q", data, at)[0]


class Meta:
    """A SPIR-V file and its meta, read as far as one meta alone allows."""

    def __init__(self, store, name, data):
        self.store, self.name, self.data = store, name, data
        self.program_text, _, index_text = name[:-4].rpartition("_")
        self.program, self.index = int(self.program_text, 16), int(index_text)
        if len(data) < HEADER or u64(data, 8) != hash_combine(self.program, self.index) or \
                u64(data, 16) != self.index:
            raise ValueError("header does not name this SPIR-V")
        self.versions = (u32(data, 0), u32(data, 4))
        # Bitset string: a u64 length L, then L bytes of '0'/'1'; possibly more such bytes
        # after it, from the next field.
        self.anchors = set()
        for run in re.finditer(rb"[01]{8,}", data):
            at = run.start() - 8
            if at >= HEADER and 0 < u64(data, at) <= len(run.group()):
                self.anchors.add(at)
        if not self.anchors:
            raise ValueError("no resource bitset")
        # Walker tail candidates: (flat at, SRT info at, bytes after the walker).
        self.tails = []
        for at in range(HEADER, len(data) - 20):
            walker = u64(data, at + 8)
            rest = len(data) - at - walker
            if not 20 <= rest < MAX_SRT + MAX_PAD:  # walker pointer, size, flat size
                continue
            words = u32(data, at + 16)
            flat = at - 8 - 4 * words
            if flat >= HEADER and u64(data, flat) == words:
                self.tails.append((flat, at, rest))
        if not self.tails:
            raise ValueError("no flattened user data and walker at the end")

    def place(self, prefix):
        """Reads the bitset and the fetch data header at the common bitset offset."""
        if prefix not in self.anchors:
            raise ValueError("resource bitset not where the other metas have it")
        self.bits = u64(self.data, prefix)
        at = prefix + 8 + self.bits
        self.fetch = u64(self.data, at)
        # FetchShaderData::Serialize: u32 size, s8 vertex offset sgpr, s8 instance offset sgpr
        self.vectors_at = at + 8 + (4 + 1 + 1 if self.fetch else 0)

    def end(self, srt_size):
        """The walker tails that fit the common SRT info size, by where their flattened user
        data starts: right at the end without fetch data; with it, a few spare zero bytes may
        follow (FetchShaderData::Serialize's Grow leaves room that later writes smaller than it
        fill). The common Info size picks one (element_sizes)."""
        self.flats = {}
        for flat, _, rest in self.tails:
            pad = rest - srt_size
            if 0 <= pad < (MAX_PAD if self.fetch else 1) and \
                    not any(self.data[len(self.data) - pad:]):
                self.flats[flat] = pad
        if not self.flats:
            raise ValueError("no walker tail fits the other metas")


def most_allowed(allowed, what):
    """The value the most metas allow (each gives a set); those without it are dropped later."""
    counts = {}
    for values in allowed:
        for value in values:
            counts[value] = counts.get(value, 0) + 1
    if not counts:
        raise LayoutError(f"no meta gives a {what}")
    return max(counts, key=counts.get)


def element_sizes(metas):
    """The raw element size of each spec vector (None: every count is 0), the Info size and
    where each meta's spec ends: the sizes under which every meta's Info has one size."""
    found, steps = [], [0]

    def walk(level, ends, sizes):
        steps[0] += 1
        if steps[0] > MAX_SEARCH:
            raise LayoutError("the element size search did not narrow down")
        if level == len(SPEC_VECTORS):
            infos = set.intersection(*({flat - end for flat in m.flats}
                                       for m, end in zip(metas, ends)))
            found.extend((sizes, ends, info) for info in infos)
            return
        uses = [i for i, m in enumerate(metas) if level > 0 or m.fetch]
        counts = {}
        for i in uses:
            if ends[i] + 8 > max(metas[i].flats):
                return
            counts[i] = u64(metas[i].data, ends[i])
            if counts[i] > metas[i].bits:  # never more elements than resource bits
                return
        candidates = range(1, MAX_ELEMENT + 1) if any(counts.values()) else [None]
        for size in candidates:
            after = list(ends)
            for i in uses:
                after[i] = ends[i] + 8 + counts[i] * (size or 0)
            if all(after[i] <= max(metas[i].flats) for i in uses):
                walk(level + 1, after, sizes + [size])

    walk(0, [m.vectors_at for m in metas], [])
    if len({(tuple(ends), info) for _, ends, info in found}) != 1:
        raise LayoutError(f"{len(found)} sets of element sizes fit the metas, not one")
    return found[0]


def measure(metas, unreadable):
    """Measures the layout over all metas; drops those that do not fit it into `unreadable`."""
    def keep(step):
        kept = []
        for m in metas:
            try:
                step(m)
                kept.append(m)
            except ValueError as e:
                unreadable.append((m, str(e)))
        return kept

    if not metas:
        raise LayoutError("no meta to measure")
    versions = max({m.versions for m in metas}, key=lambda v: sum(m.versions == v for m in metas))

    def same_build(m):
        if m.versions != versions:
            raise ValueError(f"meta versions {m.versions}, the others {versions}")
    metas = keep(same_build)
    # The bitset's offset (after Bindings and RuntimeInfo), then the SRT info size from the
    # metas without fetch data (nothing spare at their end).
    prefix = most_allowed([m.anchors for m in metas], "resource bitset")
    metas = keep(lambda m: m.place(prefix))
    srt_size = most_allowed([{rest for _, _, rest in m.tails} for m in metas if not m.fetch],
                            "SRT info size")
    metas = keep(lambda m: m.end(srt_size))
    if not metas:
        raise LayoutError("no meta fits the common walker tail")
    sizes, ends, info_size = element_sizes(metas)
    for m, end in zip(metas, ends):
        m.spec_end, m.flat = end, end + info_size
    hash_at = most_allowed([{at - m.spec_end for at in range(m.spec_end, m.flat - 7)
                             if u64(m.data, at) == m.program} for m in metas],
                           "program hash in the Info")

    def hash_in_info(m):
        if u64(m.data, m.spec_end + hash_at) != m.program:
            raise ValueError("program hash not where the other metas have it")
    metas = keep(hash_in_info)
    spare = sorted({m.flats[m.flat] for m in metas if m.fetch})
    print(f"meta layout: versions {versions[0]}/{versions[1]}, spec prefix {prefix - HEADER} "
          f"bytes, " + ", ".join(f"{n} {s or '-'}" for n, s in zip(SPEC_VECTORS, sizes)) +
          f" bytes each, Info {info_size} bytes (program hash at {hash_at}), SRT info "
          f"{srt_size} bytes, spare bytes after fetch data {spare or '-'}")
    return metas


def load(store, unreadable, orphans):
    metas = []
    names = sorted(os.listdir(store))
    paired = set()
    for name in names:
        if not name.endswith(".spv"):
            continue
        program_text, _, index_text = name[:-4].rpartition("_")
        meta_name = f"{hash_combine(int(program_text, 16), int(index_text)):#018x}.meta"
        paired.add(meta_name)
        try:
            with open(os.path.join(store, meta_name), "rb") as f:
                data = f.read()
        except OSError:
            unreadable.append((store, name, "no meta"))
            continue
        try:
            metas.append(Meta(store, name, data))
        except ValueError as e:
            unreadable.append((store, name, str(e)))
    orphans += [os.path.join(store, n) for n in names if n.endswith(".meta") and n not in paired]
    return metas


def programs(metas, store):
    """program hash -> {(spec digest, SPIR-V digest)}"""
    found = {}
    for m in metas:
        if m.store != store:
            continue
        with open(os.path.join(store, m.name), "rb") as f:
            spirv = hashlib.sha256(f.read()).hexdigest()[:16]
        spec = hashlib.sha256(m.data[HEADER:m.spec_end]).hexdigest()[:16]
        found.setdefault(m.program_text, set()).add((spec, spirv))
    return found


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    show_all = "--all" in sys.argv
    unreadable, misfits, orphans = [], [], []
    metas = load(args[0], unreadable, orphans) + load(args[1], unreadable, orphans)
    try:
        metas = measure(metas, misfits)
    except LayoutError as e:
        print(f"cannot read the metas: {e}")
        return 2
    unreadable += [(m.store, m.name, why) for m, why in misfits]
    race, boot = programs(metas, args[0]), programs(metas, args[1])
    both = set(race) & set(boot)
    counts = {"same": 0, "boot within race": 0, "race within boot": 0, "differ": 0}
    differences = 0
    lines = []
    for program in sorted(both):
        r, b = race[program], boot[program]
        if r == b:
            kind = "same"
        elif b <= r:
            kind = "boot within race"
        elif r <= b:
            kind = "race within boot"
        else:
            kind = "differ"
        counts[kind] += 1
        differences += len(b - r)
        if show_all or b - r:
            lines.append(f"{program}: {kind}, race {len(r)} permutations, boot {len(b)}, "
                         f"boot not in race {len(b - r)}")
    boot_only = sorted(set(boot) - set(race))
    print(f"programs: race {len(race)}, boot {len(boot)}, both {len(both)}, "
          f"race only {len(set(race) - set(boot))}, boot only {len(boot_only)}")
    print("programs in both: " + ", ".join(f"{n} {k}" for k, n in counts.items()))
    print(f"boot-built permutations (spec, SPIR-V) not in the race-built store: {differences} "
          f"(the rest of each meta, the Info, is not compared)")
    print(f"SPIR-V files without a readable meta: {len(unreadable)}")
    for store, name, why in unreadable[:8]:
        print(f"  {os.path.join(store, name)}: {why}")
    print(f"metas without a SPIR-V: {len(orphans)}")
    for path in orphans[:8]:
        print(f"  {path}")
    for program in boot_only:
        lines.append(f"{program}: boot only, {len(boot[program])} permutations")
    for line in lines:
        print(line)
    failed = differences or boot_only or unreadable or orphans or not boot or not both
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
