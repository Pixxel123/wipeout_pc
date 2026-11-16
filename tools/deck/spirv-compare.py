"""Compare the SPIR-V of two shader stores per program hash (precompile gate 1): a store built
by racing and one built at start from a precompile list (GOW3_PRECOMPILE_BOOT).

Permutation numbers differ between the two, so each program is compared as the set of its
SPIR-V contents (<program hash>_<index>.spv), not file by file. The .meta files are left out:
they hold flattened user data and walker bytes of whichever draw came first.
A boot-built SPIR-V that the race-built store lacks for the same program is a translation
difference; race-only ones are permutations the list did not cover.
Prints hashes and counts only.

usage: python3 -I spirv-compare.py <race-built store dir> <boot-built store dir> [--all]"""
import hashlib, os, sys


def programs(store):
    """program hash -> {sha256 of the SPIR-V: [file index, ...]}"""
    found = {}
    for name in os.listdir(store):
        if not name.endswith(".spv"):
            continue
        program, _, index = name[:-4].rpartition("_")
        with open(os.path.join(store, name), "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()[:16]
        found.setdefault(program, {}).setdefault(digest, []).append(index)
    return found


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    show_all = "--all" in sys.argv
    race, boot = programs(args[0]), programs(args[1])
    counts = {"same": 0, "boot within race": 0, "race within boot": 0, "differ": 0}
    boot_only_spirv = 0
    lines = []
    for program in sorted(set(race) & set(boot)):
        r, b = set(race[program]), set(boot[program])
        if r == b:
            kind = "same"
        elif b <= r:
            kind = "boot within race"
        elif r <= b:
            kind = "race within boot"
        else:
            kind = "differ"
        counts[kind] += 1
        boot_only_spirv += len(b - r)
        if show_all or b - r:
            lines.append(f"{program}: {kind}, race {len(r)} SPIR-V, boot {len(b)}, "
                         f"boot not in race {len(b - r)}")
    print(f"programs: race {len(race)}, boot {len(boot)}, both {len(set(race) & set(boot))}, "
          f"race only {len(set(race) - set(boot))}, boot only {len(set(boot) - set(race))}")
    print("programs in both: " + ", ".join(f"{n} {k}" for k, n in counts.items()))
    print(f"boot-built SPIR-V not in the race-built store (translation differences): "
          f"{boot_only_spirv}")
    for line in lines:
        print(line)
    return 1 if boot_only_spirv else 0


if __name__ == "__main__":
    sys.exit(main())
