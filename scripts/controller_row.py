# SPDX-License-Identifier: GPL-2.0-or-later
"""A Controller type row on WipEout's Controls screens, one per player, in the game's style.

Both screens of plugins/frontend/gui/controls_definition_ps4.xml (Controls and ControlsP2, the
second with InputPad="1") keep a switched-off Thrust Sensitivity slider (<aSlider>) below Airbrake
Sensitivity. Its place becomes a list row the game saves as a setting of its own, "Pad Style P1" or
"Pad Style P2" (scripts/pad_style_save.py gives the save those settings; src/runtime_glyphs.c
follows them). The row takes the slider's bytes, padded with spaces. Labels the game has no string
for are shown as written, so CONTROLLER TYPE and XBOX are upper case like the other rows; FE_DEFAULT
reads DEFAULT. `default` is an entry's value: the game matches it against the values. The archives
are not written: the layout is packed into the bytes the original takes (scripts/psarc.py)."""
import argparse
import contextlib
from pathlib import Path
import re

import psarc

VERSION = 1  # of the row: a change rebuilds the cached patches
LAYOUT = 'data/plugins/frontend/gui/controls_definition_ps4.xml'
NAMES = ('Pad Style P1', 'Pad Style P2')
ENTRIES = (('FE_DEFAULT', 'playstation'), ('XBOX', 'xbox'))  # (label, saved value)
SLIDER = re.compile(rb'<aSlider name="Thrust Sensitivity".*?</aSlider>', re.S)


def row(player):
    """The row of player 0 or 1, as the layout writes its other rows' tags."""
    name = NAMES[player]
    pad = ' InputPad="1"' if player else ''
    entries = ''.join(f'<Entry idstring="{label}" value="{value}"></Entry>\r\n' for label, value in ENTRIES)
    return (f'<List name="{name}" global="{name}" focus="true" save="true" default="{ENTRIES[0][1]}" '
            f'delay="0.5"{pad}>\r\n'
            f'<Values idstring="CONTROLLER TYPE" x="0" y="780" color="FEGlobals->TextColor" '
            f'arrowcolor="FEGlobals->TextColor" gap="0" sideWidth="790" rightmin="700" rightmax="805"></Values>\r\n'
            f'{entries}</List>').encode('latin1')


def add_row(xml):
    """The layout with each screen's slider replaced by its player's row, the same length; the
    layout as it is when it has no such slider. ValueError when the sliders are not one per screen
    or a row does not fit in its slider's bytes."""
    sliders = list(SLIDER.finditer(xml))
    if not sliders:
        return xml
    players = [1 if b'InputPad="1"' in m.group() else 0 for m in sliders]
    if sorted(players) != [0, 1]:
        raise ValueError(f'expected one Thrust Sensitivity slider per Controls screen, found players {players}')
    out, at = bytearray(), 0
    for match, player in zip(sliders, players):
        new = row(player)
        if len(new) > len(match.group()):
            raise ValueError(f'the Controller type row of player {player + 1} does not fit ({len(new)} bytes)')
        out += xml[at:match.start()] + new + b' ' * (len(match.group()) - len(new))
        at = match.end()
    return bytes(out + xml[at:])


def changes(game, given=None):
    """{archive: [(offset, original, new)]} that give every copy of the layout its rows. The game
    reads the newest archive's copy: it must fit, or ValueError; an older copy that does not fit
    is left as it is."""
    with contextlib.ExitStack() as opened:
        copies = []
        for name, path in psarc.archives(game, given):
            archive = opened.enter_context(psarc.Archive(path))
            i = archive.find(LAYOUT)
            if i is not None:
                copies.append((name, archive, i))
        result = {}
        for n, (name, archive, i) in enumerate(copies):
            parts = archive.repack(i, add_row(archive.read(i)))
            if parts is None:
                if n == len(copies) - 1:
                    raise ValueError(f'{name}: the Controls layout with its rows does not fit')
                continue
            result[name] = parts
        return result


def build(game, out, given=None):
    """Lines for out/file_patches.txt, cached in out/controller_row until the archives change."""
    found = psarc.archives(game, given)

    def make(folder):
        return [line for name, parts in changes(game, given).items() for line in psarc.patch_lines(name, parts, folder)]
    key = [VERSION, [name for name, _p in found], psarc.stamp(p for _n, p in found)]
    return psarc.cached_lines(Path(out) / 'controller_row', key, make)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True, help='adds its changes to OUT/file_patches.txt')
    psarc.add_archive_option(parser)
    args = parser.parse_args()
    try:
        lines = build(args.game, args.out, args.archive)
    except Exception as error:  # noqa: BLE001  (a menu row: whatever fails, the game starts)
        print(f'Controller type row: {error}; the Controls screens stay as they are', flush=True)
        return
    with open(args.out / 'file_patches.txt', 'a') as f:
        f.writelines(lines)
    print(f'Controller type row: on the Controls screens ({len(lines)} changes; game files untouched)', flush=True)


if __name__ == '__main__':
    main()
