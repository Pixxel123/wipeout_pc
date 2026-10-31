"""Menu changes the game reads in its own archives, written for the runtime (GOW3_FILE_PATCHES,
src/runtime_file.c); the game files are not changed.

WipEout's Eliminator target slider (Racebox) stops at 600 points (#89), a limit of the menu file
only: data09.psarc's racebox_definition.xml, stored uncompressed, says maxSlide="600". It becomes
--limit (2000 by default; the launcher's Game page) in the same number of bytes: each extra digit
takes one space of the line's indentation.
Output: out/file_patches.txt, "guest-path offset original new" per line (hex); empty when no
archive matches.
"""
import argparse
from pathlib import Path

import psarc

ARCHIVE = 'data09.psarc'
MENU = 'data/plugins/frontend/gui/racebox_definition.xml'
SLIDER = b'<Slider name="Eliminations"'
LIMIT = b'maxSlide="600"'


def eliminator_patches(game, limit=2000, given=None):
    """(guest path, offset, original, new) of each slider's limit in the stored menu file of
    data09.psarc; given: the archives as psarc.archives takes them."""
    name, path = next(((n, f) for n, f in psarc.archives(game, given) if n.lower() == ARCHIVE), (None, None))
    if not path:
        return []
    with psarc.Archive(path) as archive:
        i = archive.find(MENU)
        if not i or archive.compressed(i):
            return []
        offset, data = archive.entries[i]['offset'], archive.read(i)
    new_limit = f'maxSlide="{limit}"'.encode()
    extra = len(new_limit) - len(LIMIT)
    if limit == 600 or extra < 0:
        return []
    patches = []
    at = data.find(SLIDER)
    while at >= 0:
        indent = data.rfind(b'\n', 0, at) + 1
        end = data.find(LIMIT, at)
        if indent + extra <= at and not data[indent:indent + extra].strip(b' \t') and 0 < end - at < 400:
            end += len(LIMIT)
            original = data[indent:end]
            new = original[extra:].replace(LIMIT, new_limit)
            patches.append((f'/app0/{name}', offset + indent, original, new))
        at = data.find(SLIDER, at + 1)
    return patches


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--limit', type=int, default=2000, help='Eliminator target slider maximum (600 and up)')
    psarc.add_archive_option(p)
    a = p.parse_args()
    patches = eliminator_patches(a.game, a.limit, a.archive)
    (a.out / 'file_patches.txt').write_text(''.join(f'{g} {o:x} {old.hex()} {new.hex()}\n' for g, o, old, new in patches))
    if patches:
        print(f'Menu: Eliminator target up to {a.limit} ({len(patches)} slider{"s" if len(patches) > 1 else ""})', flush=True)


if __name__ == '__main__':
    main()
