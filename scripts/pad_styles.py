# SPDX-License-Identifier: GPL-2.0-or-later
"""How each controller names its buttons: one table for the launcher (its Controls page's icons and
gamepad boxes, launcher/button_icons.py) and the game's prompts (scripts/wipeout_glyphs.py).

Buttons are SDL gamepad buttons ('a' is the south face button, 'leftshoulder', 'dpup', ...)."""

# SDL button -> (its short label, its name in the launcher's gamepad boxes), per style.
# The names are translation keys (launcher/gow3_lang.py). None: the button has no box (L2/R2 stay
# on the triggers). The order is the boxes' order.
DPAD = {'dpup': ('up', 'D-pad up'), 'dpdown': ('down', 'D-pad down'),
        'dpleft': ('left', 'D-pad left'), 'dpright': ('right', 'D-pad right')}
STYLES = {
    'playstation': {
        'a': ('cross', 'Cross'), 'b': ('circle', 'Circle'), 'x': ('square', 'Square'), 'y': ('triangle', 'Triangle'),
        'leftshoulder': ('L1', 'L1'), 'rightshoulder': ('R1', 'R1'), 'leftstick': ('L3', 'L3'), 'rightstick': ('R3', 'R3'),
        'start': ('OPTIONS', 'Options'), 'back': ('SHARE', 'Share / Create'), 'touchpad': ('TOUCH', 'Touchpad click'),
        'guide': ('PS', 'PS button'), **DPAD, 'misc1': ('MUTE', 'Mute button'),
        'lefttrigger': ('L2', None), 'righttrigger': ('R2', None)},
    'xbox': {
        'a': ('A', 'A'), 'b': ('B', 'B'), 'x': ('X', 'X'), 'y': ('Y', 'Y'),
        'leftshoulder': ('LB', 'LB'), 'rightshoulder': ('RB', 'RB'),
        'leftstick': ('LS', 'LS (left stick click)'), 'rightstick': ('RS', 'RS (right stick click)'),
        'start': ('MENU', 'Menu'), 'back': ('VIEW', 'View'), 'touchpad': ('TOUCH', 'Touchpad click'),
        'guide': ('XBOX', 'Xbox button'), **DPAD, 'misc1': ('SHARE', 'Share'),
        'lefttrigger': ('LT', None), 'righttrigger': ('RT', None)},
    # By position, as printed on a Nintendo controller: the south button is B, the east one A.
    'nintendo': {
        'a': ('B', 'B'), 'b': ('A', 'A'), 'x': ('Y', 'Y'), 'y': ('X', 'X'),
        'leftshoulder': ('L', 'L'), 'rightshoulder': ('R', 'R'),
        'leftstick': ('LS', 'Left stick click'), 'rightstick': ('RS', 'Right stick click'),
        'start': ('+', 'Plus (+)'), 'back': ('−', 'Minus (−)'), 'touchpad': ('TOUCH', 'Touchpad click'),
        'guide': ('HOME', 'Home button'), **DPAD, 'misc1': ('CAPT', 'Capture'),
        'lefttrigger': ('ZL', None), 'righttrigger': ('ZR', None)},
}
