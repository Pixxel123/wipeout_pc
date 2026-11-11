# SPDX-License-Identifier: GPL-2.0-or-later
"""A Controller type row on WipEout's Controls screens, one per player, in the game's style.

Both screens of plugins/frontend/gui/controls_definition_ps4.xml (Controls and ControlsP2, the
second with InputPad="1") keep a switched-off Thrust Sensitivity slider (<aSlider>) below Airbrake
Sensitivity. It becomes a list row the game saves as a setting of its own, "Pad Style P1" or
"Pad Style P2" (scripts/pad_style_save.py gives the save those settings; src/runtime_glyphs.c
follows them), and the row is the first of the screen's Adjust settings: the rows above the slider
move down one. AUTO shows the buttons of the player's connected gamepad, DEFAULT the PlayStation
ones, XBOX the Xbox ones. The rows keep their bytes, padded with spaces. Labels the game has no
string for are shown as written, so CONTROLLER TYPE, AUTO and XBOX are upper case like the other
rows; FE_DEFAULT reads DEFAULT. `default` is an entry's value: the game matches it against the values. The archives
are not written: the layout is packed into the bytes the original takes (scripts/psarc.py)."""
import argparse
import contextlib
from pathlib import Path
import re

import psarc

VERSION = 2  # of the row: a change rebuilds the cached patches
LAYOUT = 'data/plugins/frontend/gui/controls_definition_ps4.xml'
NAMES = ('Pad Style P1', 'Pad Style P2')
# (label, saved value): AUTO follows the player's connected gamepad (src/runtime_glyphs.c, run.py).
ENTRIES = (('AUTO', 'auto'), ('FE_DEFAULT', 'playstation'), ('XBOX', 'xbox'))
DEFAULT = 'auto'  # a new profile's (one the save has no setting for yet gets the launcher's, pad_style_save.py)
# A screen's Adjust settings rows, top to bottom: Motion Sensor, Motion Sensitivity, Airbrake
# Sensitivity and the switched-off Thrust Sensitivity slider.
BLOCK = re.compile(rb'<MotionList name="Motion Sensor".*?</MotionList>\s*<Slider name="Motion Sensitivity".*?</Slider>'
                   rb'\s*<Slider name="Airbrake Sensitivity".*?</Slider>\s*<aSlider name="Thrust Sensitivity".*?</aSlider>',
                   re.S)
SETTING = re.compile(rb'<(MotionList|Slider|aSlider) .*?</\1>', re.S)
SLIDER = re.compile(rb'<aSlider name="Thrust Sensitivity"')
Y = re.compile(rb'(\sy=")([^"]*)(")')
DELAY = re.compile(rb'(\sdelay=")([^"]*)(")')
INDENT = re.compile(rb'\r\n[ \t]+')


def row(player, y=b'780', delay=b'0.5'):
    """The row of player 0 or 1, as the layout writes its other rows' tags."""
    name = NAMES[player]
    pad = ' InputPad="1"' if player else ''
    entries = ''.join(f'<Entry idstring="{label}" value="{value}"></Entry>\r\n' for label, value in ENTRIES)
    return (f'<List name="{name}" global="{name}" focus="true" save="true" default="{DEFAULT}" '
            f'delay="{delay.decode()}"{pad}>\r\n'
            f'<Values idstring="CONTROLLER TYPE" x="0" y="{y.decode()}" color="FEGlobals->TextColor" '
            f'arrowcolor="FEGlobals->TextColor" gap="0" sideWidth="790" rightmin="700" rightmax="805"></Values>\r\n'
            f'{entries}</List>').encode('latin1')


def _first(pattern, element, value):
    """element with the first match of pattern given value."""
    return pattern.sub(lambda m: m.group(1) + value + m.group(3), element, count=1)


def _rows_first(block, player, indent):
    """A screen's Adjust settings rows with the Controller type row first: it takes the Motion
    Sensor row's place (its height and its turn in the rows' entrance) and each row below moves
    down one, the last into the switched-off slider's; the same length, padded with spaces."""
    settings = [m.group() for m in SETTING.finditer(block)]
    ys = [Y.search(s).group(2) for s in settings]
    delays = [DELAY.search(s).group(2) for s in settings]
    # Their inner lines lose their indentation (whitespace between tags) to make room for the row.
    moved = [INDENT.sub(b'\r\n', _first(DELAY, _first(Y, s, ys[i + 1]), delays[i + 1]))
             for i, s in enumerate(settings[:-1])]
    new = (b'\r\n' + indent).join([row(player, ys[0], delays[0]), *moved])
    if len(new) > len(block):
        raise ValueError(f'the Controller type row of player {player + 1} does not fit ({len(new)} > {len(block)} bytes)')
    return new + b' ' * (len(block) - len(new))


def add_row(xml):
    """The layout with each Controls screen's Controller type row first in its Adjust settings, in
    place of the switched-off Thrust Sensitivity slider (the rows below it move down one); the same
    length. The layout as it is when it has no such slider. ValueError when the sliders are not
    one per screen, a slider is not below the other three rows, or a row does not fit."""
    if not SLIDER.search(xml):
        return xml
    blocks = list(BLOCK.finditer(xml))
    players = [1 if b'InputPad="1"' in m.group() else 0 for m in blocks]
    if sorted(players) != [0, 1] or len(SLIDER.findall(xml)) != 2:
        raise ValueError(f'expected one Adjust settings block with a Thrust Sensitivity slider per Controls screen, '
                         f'found players {players}')
    out, at = bytearray(), 0
    for match, player in zip(blocks, players):
        line = xml[:match.start()].rsplit(b'\n', 1)[-1]
        indent = line if not line.strip() else b''
        if len(SETTING.findall(match.group())) != 4:
            raise ValueError(f'the Adjust settings of player {player + 1} are not the four rows expected')
        out += xml[at:match.start()] + _rows_first(match.group(), player, indent)
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
