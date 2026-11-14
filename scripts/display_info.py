#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The primary monitor's desktop size and refresh rate (Windows, or xrandr on Linux), for the "display"
render resolution and the launcher's lists. Each can be said instead, with GOW3_DISPLAY_SIZE and
GOW3_DISPLAY_HZ. Stands alone: it imports nothing of the port."""
import functools
import os
import re
import subprocess
import sys

_SIZE = re.compile(r'([1-9][0-9]{0,4})x([1-9][0-9]{0,4})')

DISPLAY_SIZE_ENV = 'GOW3_DISPLAY_SIZE'  # "<width>x<height>": says what the monitor's size is, instead of asking


def display_size(env=None):
    """(width, height) in pixels of the primary monitor's desktop, the one the game window opens on, or
    None when it cannot be told. GOW3_DISPLAY_SIZE says it instead. A DSR or DLDSR desktop is its larger size."""
    env = os.environ if env is None else env
    said = _SIZE.fullmatch(env.get(DISPLAY_SIZE_ENV, ''))
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


@functools.lru_cache(maxsize=1)
def _this_display():
    return display_size()


def xrandr_primary_size(text):
    """The primary monitor's size in `xrandr --current` output (the first connected one when none is
    primary), or None."""
    found = (re.search(r' connected primary (\d+)x(\d+)\+', text)
             or re.search(r' connected (\d+)x(\d+)\+', text))
    return (int(found.group(1)), int(found.group(2))) if found else None
