"""Compare the SPIR-V of two shader stores per program hash (precompile gate 1): a store built
by racing and one built at start from a precompile list (GOW3_PRECOMPILE_BOOT).

Permutation numbers differ between the two, so each program is compared as the set of its
permutations, each one the pair (its spec, its SPIR-V): <hash>_<index>.spv with the spec bytes
of its meta, <HashCombine(hash, index)>.meta. A meta whose spec sits with the wrong SPIR-V
therefore counts as a difference. The rest of a meta (the program's Info, with flattened user
data and walker bytes from whichever draw came first) is left out.
A boot-built pair that the race-built store lacks for the same program is a difference;
race-only ones are permutations the list did not cover.
Exits non-zero on a difference, an unreadable meta, an empty boot store or no common program.
Prints hashes and counts only.

usage: python3 -I spirv-compare.py <race-built store dir> <boot-built store dir> [--all]"""
import hashlib, os, struct, sys

MASK = (1 << 64) - 1
# Meta layout (vk_pipeline_serialization.cpp RegisterShaderMeta, Info::Serialize): meta version
# u32, binary version u32, perm hash u64, perm index u64, the spec, then the Info:
# InfoPersistent raw, the flattened user data (u64 count, u32 words), PersistentSrtInfo raw
# (walker pointer, u64 walker size, u32 flat size, padding) and the walker bytes.
META_HEADER = 24
INFO_PERSISTENT = 6368  # sizeof(Shader::InfoPersistent) in this build
INFO_PGM_HASH = 6336  # offsetof(Shader::InfoPersistent, pgm_hash)
SRT_INFO = 24  # sizeof(Shader::PersistentSrtInfo)


def hash_combine(seed, value):
    return seed ^ ((value + 0x9E3779B9 + ((seed << 12) & MASK) + (seed >> 4)) & MASK)


def u64(data, at):
    return struct.unpack_from("<Q", data, at)[0]


def spec_bytes(meta, program, index):
    """The spec part of a meta, or None when the layout does not match."""
    if len(meta) < META_HEADER or u64(meta, 8) != hash_combine(program, index) or \
            u64(meta, 16) != index:
        return None
    for walker in range(len(meta) - SRT_INFO - META_HEADER + 1):
        srt = len(meta) - walker - SRT_INFO
        if u64(meta, srt + 8) != walker:
            continue
        words = struct.unpack_from("<I", meta, srt + 16)[0]
        flat = srt - 8 - 4 * words
        info = flat - INFO_PERSISTENT
        if info >= META_HEADER and u64(meta, flat) == words and \
                u64(meta, info + INFO_PGM_HASH) == program:
            return meta[META_HEADER:info]
    return None


def programs(store, unreadable):
    """program hash -> {(spec digest, SPIR-V digest)}"""
    found = {}
    for name in os.listdir(store):
        if not name.endswith(".spv"):
            continue
        program_text, _, index_text = name[:-4].rpartition("_")
        program, index = int(program_text, 16), int(index_text)
        try:
            with open(os.path.join(store, f"{hash_combine(program, index):#018x}.meta"), "rb") as f:
                spec = spec_bytes(f.read(), program, index)
        except OSError:
            spec = None
        if spec is None:
            unreadable.append(name)
            continue
        with open(os.path.join(store, name), "rb") as f:
            spirv = hashlib.sha256(f.read()).hexdigest()[:16]
        found.setdefault(program_text, set()).add((hashlib.sha256(spec).hexdigest()[:16], spirv))
    return found


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    show_all = "--all" in sys.argv
    unreadable = []
    race, boot = programs(args[0], unreadable), programs(args[1], unreadable)
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
    print(f"programs: race {len(race)}, boot {len(boot)}, both {len(both)}, "
          f"race only {len(set(race) - set(boot))}, boot only {len(set(boot) - set(race))}")
    print("programs in both: " + ", ".join(f"{n} {k}" for k, n in counts.items()))
    print(f"boot-built permutations (spec, SPIR-V) not in the race-built store: {differences}")
    print(f"SPIR-V files without a readable meta: {len(unreadable)}")
    for name in unreadable[:8]:
        print(f"  {name}")
    for line in lines:
        print(line)
    return 1 if differences or unreadable or not boot or not both else 0


if __name__ == "__main__":
    sys.exit(main())
