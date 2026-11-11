# SPDX-License-Identifier: GPL-2.0-or-later
"""WipEout's saved settings: the Controller type rows' values in the profile save's DATA.BIN.

The game keeps its menu settings in a list of named values. Its profile loader (image 0x681440)
frees the list and rebuilds it only from DATA.BIN's records, so a setting the save does not have
yet, such as the rows scripts/controller_row.py adds, is lost at every start and never saved.
add_settings() gives the save "Pad Style P1" and "Pad Style P2" once, seeded with the launcher's
controller type; from then on the game reads, changes and writes them itself.

DATA.BIN: u32 0x2000, 0x3c0 bytes, then records back to back to the end of the file, with no
count or checksum (the loader needs them to fill it exactly): a 0x68-byte header (+0 u32 key, the
game's hash of the name, +4 the name, NUL-terminated, in 64 bytes, +0x44 u32 0, +0x48 u32 the
value's length with its NUL, +0x4c u32 capacity) and `capacity` bytes of value. A file that does
not read this way is never written."""
import os
from pathlib import Path
import struct
import zlib

MAGIC, HEAD, RECORD, CAPACITY = 0x2000, 0x3c4, 0x68, 0x40
PLAYERS = ('Pad Style P1', 'Pad Style P2')
STYLES = ('playstation', 'xbox', 'nintendo')
AUTO = 'auto'  # the row's AUTO: the style of the player's connected gamepad (pad_style())


def pad_style(output):
    """The style of the gamepad on the line `gow3-probe --gamepad` prints, None without one. By
    SDL's type: PlayStation pads theirs, Nintendo ones (Switch, Joy-Con, GameCube) theirs, any
    other the Xbox buttons (src/glyph_swap.h glyph_style_for_pad(), the same rule)."""
    for line in (output or '').splitlines():
        if line.startswith('Gamepad: type='):
            kind = line[len('Gamepad: type='):].split(' ', 1)[0]
            return ('playstation' if kind.startswith('ps') else
                    'nintendo' if kind.startswith(('switch', 'joycon', 'gamecube')) else 'xbox')
    return None


def key(name):
    """The game's hash of a setting's name (image 0x401a08)."""
    return zlib.crc32(name.encode('latin1'), 0xffffffff)


def records(data):
    """[(offset, name, value)] of a DATA.BIN, or None when it is not one."""
    if len(data) < HEAD or struct.unpack_from('<I', data, 0)[0] != MAGIC:
        return None
    found, at = [], HEAD
    while at < len(data):
        if at + RECORD > len(data):
            return None
        length, capacity = struct.unpack_from('<II', data, at + 0x48)
        if length > capacity or at + RECORD + capacity > len(data):
            return None
        name = data[at + 4:at + 0x44].split(b'\0')[0].decode('latin1')
        value = data[at + RECORD:at + RECORD + length].split(b'\0')[0].decode('latin1')
        found.append((at, name, value))
        at += RECORD + capacity
    return found


def record(name, value):
    """A record as the game writes them."""
    raw = name.encode('latin1')
    data = value.encode('latin1') + b'\0'
    if len(raw) >= 64 or len(data) > CAPACITY:
        raise ValueError(f'setting {name!r}={value!r} does not fit a record')
    header = bytearray(RECORD)
    struct.pack_into('<I', header, 0, key(name))
    header[4:4 + len(raw)] = raw
    struct.pack_into('<III', header, 0x44, 0, len(data), CAPACITY)
    return bytes(header) + data.ljust(CAPACITY, b'\0')


def with_settings(data, values):
    """data with a record for each setting of `values` it lacks (data itself when it lacks none),
    or None when data is not a DATA.BIN."""
    found = records(data)
    if found is None:
        return None
    have = {name for _o, name, _v in found}
    extra = b''.join(record(name, value) for name, value in values.items() if name not in have)
    return data + extra if extra else data


def saves(savedata, title):
    """Player 1's profile saves of the game (user 1)."""
    return sorted(Path(savedata).glob(f'1/{title}/*/DATA.BIN'))


def saved_style(savedata, title, name):
    """The first save's value of setting `name`, or None."""
    for path in saves(savedata, title):
        try:
            found = records(path.read_bytes())
        except OSError:
            continue
        if found:
            return next((value for _o, n, value in found if n == name), None)
    return None


def add_settings(savedata, title, style):
    """Gives every save that lacks them both players' settings, valued `style` (the launcher's
    controller type, or playstation); returns the files changed. Written through a temporary file
    on the disk before it replaces the save, so an interrupted write leaves the old save. A save
    that cannot be read or written is left as it is; the others still get theirs."""
    style = style if style in STYLES else 'playstation'
    changed = []
    for path in saves(savedata, title):
        try:
            data = path.read_bytes()
        except OSError:
            continue
        new = with_settings(data, dict.fromkeys(PLAYERS, style))
        if new is None or new is data:
            continue
        temp = path.with_name(path.name + '.tmp')
        try:
            with open(temp, 'wb') as f:
                f.write(new)
                f.flush()
                os.fsync(f.fileno())
            os.replace(temp, path)
        except OSError:
            try:
                temp.unlink(missing_ok=True)
            except OSError:
                pass
            continue
        changed.append(path)
    return changed


def settings_ready(savedata, title):
    """Whether the Controller type rows can be shown: no save yet (the game's settings then come
    from the layouts, the rows' included), or every save reads as a DATA.BIN holding both
    players' settings. The game saves a row's value without checking that the setting exists, so
    a row must not be on screen while a save lacks it."""
    for path in saves(savedata, title):
        try:
            found = records(path.read_bytes())
        except OSError:
            return False
        if found is None or not set(PLAYERS) <= {name for _o, name, _v in found}:
            return False
    return True
