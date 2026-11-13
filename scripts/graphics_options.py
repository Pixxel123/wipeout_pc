#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Startup graphics options of gow3.ini (gpu/shim/gow3_graphics.h) for run.py.

The in-game menu and the launcher write them; run.py turns them into game patches and
environment variables at every start, so "Apply and restart" uses the new values. After a
restart the game confirms them (restart_unconfirmed=0) once it shows frames; when a launch
never did, the next start goes back to the last confirmed values.
"""
import json
import math
import os
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

RESOLUTIONS = ('native', '480p', '720p', '900p', '1440p', '1800p', '4K',
               '2560x1080', '3440x1440', '3840x1600', '5120x1440', '1920x1200', '2560x1600', '1280x800',
               '1440x900', '1360x768',
               '1280x960', '1440x1080', '1920x1440', '2048x1536', '2560x1920',
               '5120x2160', '5120x2880', '7680x4320')
# The ultrawide, 16:10 and 4:3 sizes of the Ultrawide mod (#202), built in: beta until more players confirm
# them, except the ones tested in game.
TESTED_RESOLUTIONS = ('1280x800', '1440x900', '1360x768', '1280x960')
BETA_RESOLUTIONS = tuple(r for r in RESOLUTIONS[7:] if r not in TESTED_RESOLUTIONS)
SIZES = {'native': (1920, 1080), '480p': (854, 480), '720p': (1280, 720), '900p': (1600, 900),
         '1440p': (2560, 1440), '1800p': (3200, 1800), '4K': (3840, 2160)}
ASPECTS = (('16:9', 16 / 9), ('16:10', 16 / 10), ('21:9', 2.37), ('32:9', 32 / 9), ('4:3', 4 / 3))
ENGINE_FPS = (60, 100, 120, 240)
DEFAULTS = {'render_resolution': 'native', 'render_scale': '100', 'engine_fps': '120', 'deferred_readback': '1',
            'stale_readback': '1', 'async_shaders': '1'}
RESOLUTION_PREFIX = 'Resolution Patch'
# The games the generated resolution patches apply to (WipEout Omega Collection).
GENERATED_TITLES = ('CUSA07671', 'CUSA05670')
MIN_GENERATED = (1920, 1080)  # measured: below the game's own size the buffer stays 1920x1080 while the camera changes
MAX_GENERATED = (7680, 4320)  # 8K, the largest hand-made size; generated sizes above 3840x2160 are untested
FPS_PREFIX = 'Frame Rate Patch'
TEXTURE_FIX = 'Bug Fix - Texture Corruption Fix'


def read_ini(path):
    values = {}
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError:
        return values
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values


def write_keys(path, updates):
    """Rewrites `updates` in place and appends missing keys, keeping every other line."""
    path = Path(path)
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = ['# gow3 settings (in-game menu: Insert / R3+L2)']
    done, out = set(), []
    for line in lines:
        key = line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
        if key in updates:
            out.append(f'{key}={updates[key]}')
            done.add(key)
        else:
            out.append(line)
    out += [f'{k}={v}' for k, v in updates.items() if k not in done]
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')


def resolutions(extra=()):
    """The render resolutions: the built-in ones and those the enabled mods' patch files add."""
    return RESOLUTIONS + tuple(e for e in extra if e not in RESOLUTIONS)


def aspect_name(width, height):
    """The marketing name of the aspect ratio ("21:9" covers 2560x1080 to 3840x1600), else the reduced ratio."""
    ratio = width / height
    for name, value in ASPECTS:
        if abs(ratio - value) < 0.05:
            return name
    divisor = math.gcd(width, height)
    return f'{width // divisor}:{height // divisor}'



_SIZE_NAME = re.compile(r'([1-9][0-9]{0,4})x([1-9][0-9]{0,4})')


def resolution_size(name):
    """(width, height) of a built-in name or a mod's "<width>x<height>" (plain digits, no sign, space or
    leading zero); None for anything else (a hand-edited state file's number or list too)."""
    if not isinstance(name, str):
        return None
    if name in SIZES:
        return tuple(SIZES[name])
    match = _SIZE_NAME.fullmatch(name)
    return (int(match[1]), int(match[2])) if match else None


def aspect_of(name):
    """The aspect ratio a render resolution belongs to ('16:9', '21:9', a mod's '5:4'), None if unknown."""
    size = resolution_size(name)
    return aspect_name(*size) if size else None


def aspects(names):
    """The aspect ratios of these resolutions: the named ones in ASPECTS order, then the others."""
    present = {aspect_of(n) for n in names} - {None}
    order = [a for a, _r in ASPECTS]
    return [a for a in order if a in present] + sorted(present - set(order))


def resolutions_for(aspect, names):
    """The resolutions of one aspect ratio, smallest first, with native on top of its list."""
    chosen = [n for n in names if aspect_of(n) == aspect]
    pixels = lambda n: (n != 'native', resolution_size(n)[0] * resolution_size(n)[1])
    return sorted(chosen, key=pixels)


def closest_resolution(aspect, current, names):
    """The resolution of `aspect` nearest to `current` in pixels (`current` itself when it is of it)."""
    chosen = resolutions_for(aspect, names)
    if current in chosen or not chosen:
        return current if current in chosen else None
    size = resolution_size(current) or SIZES['native']
    return min(chosen, key=lambda n: abs(resolution_size(n)[0] * resolution_size(n)[1] - size[0] * size[1]))


def resolution_label(name, aspect=True):
    """What the lists show for a render resolution: '1440p · 2560 × 1440 · 16:9', '3440 × 1440 · 21:9 (beta)'.
    `name` is a built-in name or a mod's "<width>x<height>"; anything else is shown as it is. Without
    `aspect` the ratio is left out (the launcher's list beside the Aspect ratio filter)."""
    if name == DISPLAY:
        return "Display · this monitor's size"
    size = resolution_size(name)
    if not size:
        return name
    width, height = size
    title = 'Native' if name == 'native' else name if name in SIZES else None
    label = ' · '.join(p for p in (title, f'{width} × {height}', aspect and aspect_name(width, height)) if p)
    return label + (' (beta)' if name in BETA_RESOLUTIONS else '')


def startup_options(ini, extra=()):
    """The startup options, normalized; unknown values fall back to the defaults. `extra`: the
    resolutions of the enabled mods (a mod that is off leaves its resolution out: native)."""
    options = {key: ini.get(key, value) for key, value in DEFAULTS.items()}
    if options['render_resolution'] not in resolutions(extra):
        options['render_resolution'] = 'native'
    if options['render_scale'] not in tuple(str(p) for p in RENDER_SCALES):
        options['render_scale'] = '100'
    if options['engine_fps'] not in {str(f) for f in ENGINE_FPS}:
        options['engine_fps'] = '120'
    for key in ('deferred_readback', 'stale_readback', 'async_shaders'):
        options[key] = '0' if options[key] == '0' else '1'
    return options


def launch_ini(config, state_path):
    """The options this launch will start with, read without writing anything: the ini's, or the last
    confirmed ones when prepare() is about to restore them (the previous restart never confirmed its own)."""
    ini = read_ini(config)
    if ini.get('restart_unconfirmed') != '1':
        return ini
    try:
        state = json.loads(Path(state_path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return ini
    if not isinstance(state, dict):
        return ini
    last_good = state.get('last_good')
    return last_good if state.get('attempt') and isinstance(last_good, dict) and last_good else ini


def prepare(config, state_path, extra=()):
    """(options, reverted): the startup options of this launch, restoring the last confirmed
    ones when the previous launch after a restart never confirmed its own."""
    ini = read_ini(config)
    state = {}
    try:
        state = json.loads(Path(state_path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        pass
    options = startup_options(ini, extra)
    reverted = False
    if ini.get('restart_unconfirmed') == '1':
        if state.get('attempt') and state.get('last_good'):
            options = startup_options(state['last_good'], extra)
            write_keys(config, {**options, 'restart_unconfirmed': '0'})
            state['attempt'] = False
            reverted = True
        else:
            state['attempt'] = True
    else:
        state = {'last_good': options, 'attempt': False}
    Path(state_path).write_text(json.dumps(state, indent=2), encoding='utf-8')
    return options, reverted


def patch_names(names, options, texture_fix_available=True):
    """The launcher's patch list with the resolution and frame rate patches of `options`."""
    names = [n for n in names if not n.startswith(RESOLUTION_PREFIX) and not n.startswith(FPS_PREFIX)]
    resolution = options['render_resolution']
    if resolution != 'native':
        names = [n for n in names if n != TEXTURE_FIX] + [f'{RESOLUTION_PREFIX} - {resolution}']
    elif texture_fix_available and TEXTURE_FIX not in names:
        names.insert(0, TEXTURE_FIX)
    if options['engine_fps'] != '60':
        names.append(f'{FPS_PREFIX} - {options["engine_fps"]} FPS')
    return names


def restrict_to_available(names, available):
    """names without the patches this XML lacks. A missing frame rate patch is replaced by the
    highest one it has below the requested rate (the 240 FPS patch of the first XML has no
    counterpart in the second)."""
    kept = [n for n in names if n in available]
    for name in names:
        if name in available:
            continue
        print(f'Patches: {name} is not available for this game build: skipped', flush=True)
        if name.startswith(f'{FPS_PREFIX} - '):
            wanted = int(name.split(' - ')[1].split()[0])
            lower = [int(n.split(' - ')[1].split()[0]) for n in available if n.startswith(f'{FPS_PREFIX} - ')
                     and int(n.split(' - ')[1].split()[0]) <= wanted]
            if lower:
                kept.append(f'{FPS_PREFIX} - {max(lower)} FPS')
                print(f'Patches: using {kept[-1]} instead', flush=True)
    return kept


def applied_engine_fps(names):
    """The engine rate the applied patch list gives: 60 without a frame rate patch."""
    return max((int(n.split(' - ')[1].split()[0]) for n in names if n.startswith(f'{FPS_PREFIX} - ')), default=60)


def environment(options):
    return {'GOW3_DEFERRED_READBACK': options['deferred_readback'],
            'GOW3_STALE_READBACK': options['stale_readback'],
            'GOW3_ASYNC_SHADERS': options['async_shaders']}


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


def generated_resolution(value, known):
    """(width, height) when `value` is an even size ("<width>x<height>" or a built-in name), at least
    MIN_GENERATED and at most MAX_GENERATED, that no resolution in `known` provides, else None (nothing to
    generate)."""
    if not value or value == 'native' or value in known:
        return None
    size = resolution_size(value)
    if (not size or size[0] % 2 or size[1] % 2 or size[0] < MIN_GENERATED[0] or size[1] < MIN_GENERATED[1]
            or size[0] > MAX_GENERATED[0] or size[1] > MAX_GENERATED[1]):
        return None
    return size


DISPLAY = 'display'  # render_resolution: the monitor's size, read again at every start
RENDER_SCALES = (100, 150, 200)  # render_scale, percent of the render resolution: above 100 % is supersampling
DISPLAY_SIZE_ENV = 'GOW3_DISPLAY_SIZE'  # "<width>x<height>": says what the monitor's size is, instead of asking


def size_name(width, height):
    """The render resolution name of a size: a built-in name with exactly that size ('1440p', 'native'),
    else '<width>x<height>' (generated at start for WipEout)."""
    return next((n for n in RESOLUTIONS if resolution_size(n) == (width, height)), f'{width}x{height}')


def scaled_size(size, percent):
    """`size` at `percent`, each side rounded to the nearest even pixel (the game halves its buffers)."""
    return tuple(int(side * percent / 100 / 2 + 0.5) * 2 for side in size)


def display_size(env=None):
    """(width, height) in pixels of the primary monitor's desktop, the one the game window opens on, or
    None when it cannot be told. GOW3_DISPLAY_SIZE says it instead. A DSR or DLDSR desktop is its larger size."""
    env = os.environ if env is None else env
    said = _SIZE_NAME.fullmatch(env.get(DISPLAY_SIZE_ENV, ''))
    if said:
        return int(said.group(1)), int(said.group(2))
    try:
        if sys.platform == 'win32':
            import ctypes
            ctypes.windll.user32.SetProcessDPIAware()  # pixels, not the scaled size
            size = ctypes.windll.user32.GetSystemMetrics(0), ctypes.windll.user32.GetSystemMetrics(1)
            return size if all(size) else None
        host = {k: v for k, v in env.items() if k not in ('LD_LIBRARY_PATH', 'PYTHONHOME', 'PYTHONPATH')}
        text = subprocess.run(['xrandr', '--current'], capture_output=True, text=True, timeout=3, env=host).stdout
    except (OSError, AttributeError, subprocess.SubprocessError):
        return None
    return xrandr_primary_size(text)


def xrandr_primary_size(text):
    """The primary monitor's size in `xrandr --current` output (the first connected one when none is
    primary), or None."""
    found = (re.search(r' connected primary (\d+)x(\d+)\+', text)
             or re.search(r' connected (\d+)x(\d+)\+', text))
    return (int(found.group(1)), int(found.group(2))) if found else None


def render_size(options, display):
    """(width, height) the game renders at when the options ask for the monitor's size or a render scale,
    else None (the render resolution as it is named). An unknown monitor, or one smaller than the game's
    own size, renders at the game's own size; a scale that passes MAX_GENERATED is left out."""
    name, scale = options['render_resolution'], int(options['render_scale'])
    if name != DISPLAY and scale == 100:
        return None
    base = (display if name == DISPLAY else resolution_size(name)) or SIZES['native']
    fits = lambda size: size[0] <= MAX_GENERATED[0] and size[1] <= MAX_GENERATED[1]
    size = next((s for s in (scaled_size(base, scale), scaled_size(base, 100)) if fits(s)), SIZES['native'])
    return SIZES['native'] if size[0] < MIN_GENERATED[0] or size[1] < MIN_GENERATED[1] else size


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
