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
import re
from pathlib import Path

from display_info import this_display

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
DEFAULTS = {'render_resolution': 'display', 'render_scale': '100', 'engine_fps': '120', 'deferred_readback': '1',
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


def resolution_label(name):
    """What the lists show for a render resolution, as a PC game shows it: '2560 × 1440'. "display" is the
    monitor's size; `name` is otherwise a built-in name or a mod's "<width>x<height>", and anything else is
    shown as it is."""
    if name == DISPLAY:
        return display_label(display_render_size(this_display()) if this_display() else None)
    size = resolution_size(name)
    return f'{size[0]} × {size[1]}' if size else name


def resolution_choices(names, monitor, keep=()):
    """[(name, label)] of the Resolution list as a PC game lists it: "display" first (when `names` has it),
    labelled with the size it renders at on this monitor, then the other sizes largest first. A size equal
    to that one or larger is left out (Resolution scale goes above the monitor). The `keep` names (the saved
    one) are always listed, also a size `names` lacks, except a size equal to the monitor's entry: that
    entry stands for it, so no size shows twice. An unknown `monitor` (None) leaves every size in."""
    shown = display_render_size(monitor) if monitor and DISPLAY in names else None
    names = [*names, *(k for k in keep if k and k not in names and resolution_size(k))]

    def listed(name):
        size = resolution_size(name)
        if not size or not shown:
            return True
        if size == shown:
            return False  # the monitor's entry is that size: listed once, also when it is the saved one
        return name in keep or (size[0] <= shown[0] and size[1] <= shown[1])

    def largest_first(name):
        size = resolution_size(name)
        return (0, -size[0] * size[1], -size[0]) if size else (1, 0, 0)
    first = [DISPLAY] if DISPLAY in names else []
    rest = sorted((n for n in dict.fromkeys(names) if n != DISPLAY and listed(n)), key=largest_first)
    return [(n, display_label(shown) if n == DISPLAY else resolution_label(n)) for n in first + rest]


HEAVY_ABOVE = (3840, 2160)  # a render size past this on either side needs more than 8 GB of video memory
HEAVY_MARK = '  ⚠'


def render_scale_choices(base, offered=True):
    """[(percent, label)] of the Resolution scale list: the RENDER_SCALES whose size for `base` (the
    resolution's size, None when unknown) stays within MAX_GENERATED; 100 % alone for a game that has no
    render scale (`offered` False). Above 100 % the label says the size it renders at, '150 % · 3840 × 2160',
    and HEAVY_MARK past HEAVY_ABOVE."""
    def fits(percent):
        if not base:
            return True
        width, height = scaled_size(base, percent)
        return width <= MAX_GENERATED[0] and height <= MAX_GENERATED[1]

    def label(percent):
        if not base or percent == 100:
            return f'{percent} %'
        width, height = scaled_size(base, percent)
        heavy = width > HEAVY_ABOVE[0] or height > HEAVY_ABOVE[1]
        return f'{percent} % · {width} × {height}' + (HEAVY_MARK if heavy else '')
    return [(str(p), label(p)) for p in RENDER_SCALES if p == 100 or (offered and fits(p))]


def startup_options(ini, extra=()):
    """The startup options, normalized; unknown values fall back to the defaults. `extra`: the
    resolutions of the enabled mods (a mod that is off leaves its resolution out: native)."""
    options = {key: ini.get(key, value) for key, value in DEFAULTS.items()}
    # "display" (the default) where the game lists it, else the game's own size.
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


def size_name(width, height):
    """The render resolution name of a size: a built-in name with exactly that size ('1440p', 'native'),
    else '<width>x<height>' (generated at start for WipEout)."""
    return next((n for n in RESOLUTIONS if resolution_size(n) == (width, height)), f'{width}x{height}')


def scaled_size(size, percent):
    """`size` at `percent`, each side rounded to the nearest even pixel (the game halves its buffers)."""
    return tuple(int(side * percent / 100 / 2 + 0.5) * 2 for side in size)


def display_render_size(monitor):
    """What "display" draws the race at, at 100 %, on a monitor of this size (None: unknown): the monitor's
    size; below the game's own size, LOW_OUTPUT (output_size)."""
    options = {'render_resolution': DISPLAY, 'render_scale': '100'}
    return output_size(options, monitor) or render_size(options, monitor)


def display_label(size):
    """The list's name for render_resolution=display: the size it renders at, '3440 × 1440', or 'Display'
    when the monitor is unknown."""
    return f'{size[0]} × {size[1]}' if size else 'Display'


LOW_OUTPUT = (1280, 720)  # the game's output size below its own 1920 x 1080 (gow3_settings.h OutputWidths)


def output_size(options, display):
    """LOW_OUTPUT when the options ask for less than the game's own 1920 x 1080 (720p, a Steam Deck's or a
    small laptop's screen), else None. A resolution patch cannot go below the game's own size (its buffers
    stay 1920 x 1080); its output size can: the race is drawn at 1280 x 720, the HUD and menus stay sharp."""
    name, scale = options['render_resolution'], int(options['render_scale'])
    base = ((display[0] & ~1, display[1] & ~1) if display else None) if name == DISPLAY else resolution_size(name)
    if not base:
        return None
    width, height = scaled_size(base, scale)
    return LOW_OUTPUT if width < MIN_GENERATED[0] or height < MIN_GENERATED[1] else None


def legacy_low_output(ini):
    """A gow3.ini from before the 1280 x 720 Resolution entry drew the race at 1280 x 720 with the output
    setting (the Steam Deck's default, the old Output resolution row) and the game's own render resolution:
    that is "720p" now. Any other ini comes back as it is. WipEout only: God of War III's 720p patch is a
    render resolution of its own."""
    if ini.get('output_res') == f'{LOW_OUTPUT[0]}x{LOW_OUTPUT[1]}' and ini.get('render_resolution', 'native') == 'native':
        return {**ini, 'render_resolution': '720p'}
    return ini


def render_size(options, display):
    """(width, height) the game renders at when the options ask for the monitor's size, a render scale or
    less than the game's own size, else None (the render resolution as it is named). An unknown monitor,
    or a size below the game's own, renders at the game's own size (output_size draws the race smaller); an odd side of the monitor loses its last pixel; a scale that
    passes MAX_GENERATED becomes the largest listed one that fits."""
    name, scale = options['render_resolution'], int(options['render_scale'])
    if output_size(options, display):
        return SIZES['native']  # the game's own buffers; output_size draws the race smaller
    if name != DISPLAY and scale == 100:
        return None
    base = ((display[0] & ~1, display[1] & ~1) if display else None) if name == DISPLAY else resolution_size(name)
    base = base or SIZES['native']
    fits = lambda size: size[0] <= MAX_GENERATED[0] and size[1] <= MAX_GENERATED[1]
    sizes = (scaled_size(base, p) for p in sorted(RENDER_SCALES, reverse=True) if p <= scale)
    size = next((s for s in sizes if fits(s)), SIZES['native'])
    return SIZES['native'] if size[0] < MIN_GENERATED[0] or size[1] < MIN_GENERATED[1] else size


