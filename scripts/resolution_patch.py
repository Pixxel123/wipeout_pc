#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""WipEout's render resolution patch for any even size.

The hand-made "Resolution Patch" entries of patches/WipEout_Omega_Collection.xml all write the same sites with
values that follow from width x height (the XML header explains each site). resolution_patch_lines makes those
lines for any size; tests/test_resolution_patch.py keeps it equal to the hand-made entries byte for byte.
run.py writes a patch file with it when gow3.ini asks for a size nobody provides.
"""
import math
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

from graphics_options import GENERATED_TITLES, RESOLUTION_PREFIX, aspect_name

# WipEout's render resolution patch for any even size: the hand-made entries of
# patches/WipEout_Omega_Collection.xml all write these sites, with values that follow from the size
# (the XML header explains each site). Address is the reference build's; Pattern/At find the site.
_SIZE_SITES = (  # (Address, Pattern, At, field, bytes): W/H the output size, HW/HH half of it
    ('0x00bfa2e8', '7a 00 00 8a 45 a0 b9 80 07 00 00 66 bf 80 07 bb', '7', 'W', 4),
    ('0x00bfa2ee', 'a0 b9 80 07 00 00 66 bf 80 07 bb ff ff ff ff ba', '8', 'W', 2),
    ('0x00bfa31d', 'a1 00 48 8b 45 90 3d 80 07 00 00 89 c6 41 89 c0', '7', 'W', 4),
    ('0x00bfa2f6', '07 bb ff ff ff ff ba 38 04 00 00 66 41 b9 ff ff', '7', 'H', 4),
    ('0x00bfa302', '00 66 41 b9 ff ff 66 41 ba 38 04 4c 8b 25 c5 8a', '9', 'H', 2),
    ('0x00bfa367', '8b 3d e2 87 9e 00 3d 38 04 00 00 44 0f b7 f1 b9', '7', 'H', 4),
    ('0x00a44654', 'a0 00 8b 44 24 14 ba c0 03 00 00 b9 1c 02 00 00', '7', 'HW', 4),
    ('0x00a44659', '14 ba c0 03 00 00 b9 1c 02 00 00 45 31 c0 45 31', '7', 'HH', 4),
    ('0x00a44708', 'b7 db 31 ff 31 f6 41 b9 c0 03 00 00 c7 04 24 1c', '8', 'HW', 4),
    ('0x00a4470f', '41 b9 c0 03 00 00 c7 04 24 1c 02 00 00 45 31 e4', '9', 'HH', 4),
    ('0x00a4473d', 'ff be 01 00 00 00 41 b9 c0 03 00 00 44 89 f1 41', '8', 'HW', 4),
    ('0x00a4474a', '44 89 f1 41 89 d8 c7 04 24 1c 02 00 00 e8 bd 3a', '9', 'HH', 4),
    ('0x00a4b51b', 'ca b9 01 0f 00 00 66 41 ba c0 03 66 41 bd 1c 02', '9', 'HW', 2),
    ('0x00a4b520', '00 66 41 ba c0 03 66 41 bd 1c 02 c4 c2 70 f7 f0', '9', 'HH', 2),
    ('0x00a4b52c', 'f7 f0 d1 ea 84 c0 b9 c0 03 00 00 0f 45 ce 66 44', '7', 'HW', 4),
    ('0x00a4b539', 'ce 66 44 0f 45 d6 be 1c 02 00 00 66 44 0f 45 ea', '7', 'HH', 4),
    ('0x00a4b5b9', '9c 00 01 0f 45 ce 69 d9 c0 03 00 00 20 c2 0f 44', '8', 'HW', 4),
    ('0x00a4b5c4', '00 20 c2 0f 44 de 69 c1 1c 02 00 00 84 d2 89 9d', '8', 'HH', 4),
)
# Camera aspect ratio (both camera sets): a/2, 2a, a as float32, where a = W/H. The 16:9 entries
# leave the game's own 16/9 values alone.
_CAMERA_PATTERNS = ('00 00 00 bf f3 8e 63 3f f3 8e 63 40 39 8e e3 3f 35 fa 0e 3c 00 00 80 bf 9a 99',
                    '00 00 00 bf f3 8e 63 3f f3 8e 63 40 39 8e e3 3f 35 fa 0e 3c 00 00 80 bf 00 00')
# Narrower than 16:9 (Vert+, #125): the degrees-to-half-angle constant pi/360 next to them, which the
# projection and the culling planes share, scaled so the view stays as wide as at 16:9.
_CAMERA_SITES = (('0x00c60d08', 0, '12', 'a'), ('0x00c60d00', 0, '4', 'a/2'), ('0x00c60d04', 0, '8', '2a'),
                 ('0x00c60d0c', 0, '16', 'fov'),
                 ('0x00c60d48', 1, '12', 'a'), ('0x00c60d40', 1, '4', 'a/2'), ('0x00c60d44', 1, '8', '2a'),
                 ('0x00c60d4c', 1, '16', 'fov'))
_FOV_UNIT = struct.unpack('<f', bytes.fromhex('35fa0e3c'))[0]  # the game's float32 pi/360
GAME_FOV_DEGREES = 71  # the usual vertical field of view: the factor is exact there
# PS4 Pro mode's 2D/HDR layer at the output size (cmove -> mov), with the camera lines.
_PRO_LAYER = ('0x00bfbe0c', '4189c6904189cf90',
              '41 be 00 0f 00 00 41 bf 70 08 00 00 bf 30 00 00 00 44 0f 44 f0 44 0f 44 f9', '17')
# The game's video memory pool (4224 MB) grown for big pictures (the note's "DMEM to N MB" is read by
# patches.py). The value is filled in from memory_mb.
_MEMORY = ('0x00c6bb60', None,
           '00 00 00 08 01 00 00 00 00 00 00 10 00 00 00 00 00 80 00 00 00 00 00 00 00 70 05 00 00 00 00 00 '
           '00 00 00 28 01 00 00 00 00 00 00 10 00 00 00 00 00 80 00 00 00 00 00 00 00 70 05 00 00 00 00 00',
           '0')
MEMORY_BASE_MB = 4224
# (from megapixels, MB more): 3440x1440 (4.95 MP) gets 1 GB, 2560x1600 (4.10 MP) none; 5120x2160
# (11.1 MP) 1 GB, 5K (14.7 MP) 2 GB, 8K (33.2 MP) 4 GB, as the hand-made entries (#188).
MEMORY_STEPS = ((24_000_000, 4096), (12_000_000, 2048), (4_500_000, 1024))
NATIVE_ASPECT = (16, 9)
GENERATED_AUTHOR = 'generated (sites by d4g)'


def is_native_aspect(width, height):
    return width * NATIVE_ASPECT[1] == height * NATIVE_ASPECT[0]


def memory_mb(width, height):
    """How many MB the game's video memory pool grows by for this render size (0: none)."""
    return next((mb for pixels, mb in MEMORY_STEPS if width * height >= pixels), 0)


def vert_plus(width, height):
    """The field of view factor that keeps a size narrower than 16:9 as wide as 16:9 (1 otherwise)."""
    aspect, native = width / height, NATIVE_ASPECT[0] / NATIVE_ASPECT[1]
    if aspect >= native:
        return 1.0
    half = math.radians(GAME_FOV_DEGREES / 2)
    return 2 * math.degrees(math.atan(math.tan(half) * native / aspect)) / GAME_FOV_DEGREES


def resolution_patch_lines(width, height):
    """[(Address, Value, Pattern, At)] of WipEout's "Resolution Patch" for an even width x height:
    the same sites and values as the hand-made entries of patches/WipEout_Omega_Collection.xml."""
    if width % 2 or height % 2 or width < 2 or height < 2:
        raise ValueError(f'the render size must be even, not {width}x{height}')
    values = {'W': width, 'H': height, 'HW': width // 2, 'HH': height // 2}
    lines = []
    extra = memory_mb(width, height)
    if extra:
        lines.append((_MEMORY[0], struct.pack('<Q', (MEMORY_BASE_MB + extra) << 20).hex(), *_MEMORY[2:]))
    if not is_native_aspect(width, height):
        aspect = width / height
        floats = {'a': aspect, 'a/2': aspect / 2, '2a': 2 * aspect, 'fov': _FOV_UNIT * vert_plus(width, height)}
        lines += [(address, struct.pack('<f', floats[field]).hex(), _CAMERA_PATTERNS[pattern], at)
                  for address, pattern, at, field in _CAMERA_SITES
                  if field != 'fov' or width * NATIVE_ASPECT[1] < height * NATIVE_ASPECT[0]]
    lines += [(address, struct.pack('<I' if size == 4 else '<H', values[field]).hex(), pattern, at)
              for address, pattern, at, field, size in _SIZE_SITES]
    if not is_native_aspect(width, height):
        lines.append(_PRO_LAYER)
    return lines


def resolution_patch_note(width, height):
    aspect = aspect_name(width, height)
    if is_native_aspect(width, height):
        note = (f'Renders at {width}x{height} ({aspect}): output and half-resolution buffers at that size. '
                'Same 16:9 view as the game, sharper.')
    else:
        wider = width / height > NATIVE_ASPECT[0] / NATIVE_ASPECT[1]
        view = ('The camera keeps its vertical field of view, so the wider picture shows more at the sides (Hor+).'
                if wider else
                'The view is as wide as at 16:9 and taller: the field of view grows with the height (Vert+).')
        note = (f'Renders at {width}x{height} ({aspect}): output and half-resolution buffers at that size and the '
                f'camera aspect ratio set to match. {view} HUD and menus stay 16:9 in the middle of the screen.')
    extra = memory_mb(width, height)
    if extra:
        note += (f' The game gets {extra // 1024} GB more of its own video memory for the bigger picture '
                 f'(DMEM to {extra} MB), so big races load.')
    return note + ' Generated for this size: not tested in game.'


def resolution_patch_xml(width, height, out, name=None):
    """Writes a patch file with the generated "Resolution Patch - <name>" (default "<width>x<height>") and
    returns its path."""
    root = ET.Element('Patch')
    ids = ET.SubElement(root, 'TitleID')
    for title in GENERATED_TITLES:
        ET.SubElement(ids, 'ID').text = title
    meta = ET.SubElement(root, 'Metadata', Title='WipEout Omega Collection',
                         Name=f'{RESOLUTION_PREFIX} - {name or f"{width}x{height}"}', Note=resolution_patch_note(width, height),
                         Author=GENERATED_AUTHOR, PatchVer='1.0', AppVer='01.00', AppElf='eboot.bin',
                         isEnabled='false')
    patch_list = ET.SubElement(meta, 'PatchList')
    for address, value, pattern, at in resolution_patch_lines(width, height):
        ET.SubElement(patch_list, 'Line', Type='bytes', Address=address, Value=value, Pattern=pattern, At=at)
    tree = ET.ElementTree(root)
    ET.indent(tree)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tree.write(out, encoding='utf-8', xml_declaration=True)
    return out
