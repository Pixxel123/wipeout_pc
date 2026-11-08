# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes the Xbox controllers of photo mode's help (xbox_photo_*.svg) from xbox_controller.svg:
the Controls screen's drawing as white outlines, the stick an action uses filled red, in place of
the game's DualShock pictograms.

WipEout HD/2048 draws them from data/fe/images/photo_mode.2x.gnf (512x512: right stick red at
U 8..138 V 427..508, left stick red at U 238..368; photo_mode.gnf is the same at half size), Fury
from photo_mode_fury.gnf (256x256, 1x only: U 4..68 and 119..183, V 214..253, bolder lines, the
sticks filled, a darker red). Rotate and Pan use the left stick, Zoom the right one.

The drawing keeps its paths and transforms; the labels and the sticks' grip dots go, and every
path gets an inline style (what xbox_controls.load_svg reads). Run: python3 scripts/art/photo_pads.py
"""
import copy
from pathlib import Path
import xml.etree.ElementTree as ET

ART = Path(__file__).resolve().parent
SOURCE = ART / 'xbox_controller.svg'
SVG_NS = 'http://www.w3.org/2000/svg'
WHITE, RED, FURY_RED = '#ffffff', '#f01515', '#b21d2c'  # the sheets' white and reds
VIEW = (30, 35, 680, 425)  # the drawing (x 36..704, y 41..454) and room for the line

# The stick caps: the outer one is filled, the rings inside it are left out.
CAP = {'left': 'path5108', 'right': 'path5110'}
CAP_RINGS = {'left': {'path4997', 'path4999', 'path5086'}, 'right': {'path4995', 'path5001', 'path5088'}}
GRIP_DOTS = {'path5090', 'path5092', 'path5094', 'path5096', 'path5098', 'path5100', 'path5102', 'path5104'}
# What the sprite's size (131 x 82 texels, as the DualShock pictograms) cannot show: an outer and
# an inner line two texels apart read as one grey smear, so only the outer one stays. Left out: the
# guide button's X and inner ring, the trigger backs, Back/Start and their arrows, the d-pad's rings
# and inner rims (its cross stays), the stick wells (the caps stay, the idle one filled white so it
# reads as a stick), the face buttons' inner rings, the shoulder ridge and the bumpers' inner
# outlines.
SMALL = {'path4993', 'path4979', 'rect5193', 'rect5196', 'path4987', 'path4989', 'path5189',
         'path4973', 'path4975', 'path5218', 'path4983', 'path4985', 'path5191', 'path5106',
         'path5185', 'path5187', 'path4959', 'path4961', 'path4967', 'path4971', 'path5180',
         'path5176', 'path5178'}
# Fury's sprite is half the size again: the body, the triggers, the caps and the d-pad only.
FURY_DROP = SMALL | {'path4977', 'path4943', 'path4953', 'path4957', 'path4963', 'path4965', 'path4969'}

ET.register_namespace('', SVG_NS)


def styled(active, line, red, idle_fill, drop):
    tree = copy.deepcopy(ET.parse(SOURCE))
    root = tree.getroot()
    for element in root.iter():
        for name in [n for n in element.attrib if n.startswith('{') or ':' in n]:
            del element.attrib[name]  # Inkscape's own attributes
    idle = 'right' if active == 'left' else 'left'

    def keep(element):
        tag = element.tag.split('}')[-1]
        if tag in ('metadata', 'defs', 'namedview'):
            return False
        if tag != 'path':
            return True
        style = element.get('style') or ''
        ident = element.get('id')
        if 'stroke:none' in style or 'stroke:' not in style:
            return False  # letters and labels
        return ident not in GRIP_DOTS | CAP_RINGS[active] | CAP_RINGS[idle] | drop

    def prune(parent):
        for child in list(parent):
            if not keep(child):
                parent.remove(child)
            else:
                prune(child)
    prune(root)

    outline = f'fill:none;stroke:{WHITE};stroke-width:{line};stroke-linejoin:round'
    for element in root.iter(f'{{{SVG_NS}}}path'):
        ident = element.get('id')
        if ident == CAP[active]:
            element.set('style', f'fill:{red};stroke:{red};stroke-width:{line}')
        elif ident == CAP[idle] and idle_fill:
            element.set('style', f'fill:{WHITE};stroke:{WHITE};stroke-width:{line}')
        else:
            element.set('style', outline)
    for name in ('width', 'height', 'version', 'id'):
        root.attrib.pop(name, None)
    return root


def write(name, root, width, height, comment):
    root.set('width', str(width))
    root.set('height', str(height))
    root.set('viewBox', ' '.join(str(v) for v in VIEW))
    root.insert(0, ET.Comment(f' {comment} '))
    ET.indent(root, '  ')
    (ART / name).write_text(ET.tostring(root, encoding='unicode') + '\n')


def main():
    for side in ('left', 'right'):
        # 2 texels of the 131 x 82 sprite, as the DualShock's lines; Fury's 1.8 texels of 65 x 40.
        write(f'xbox_photo_{side}_stick.svg', styled(side, 10.4, RED, True, SMALL), 131, 82,
              f'Photo mode help (photo_mode.2x.gnf, 2x), from xbox_controller.svg: the {side} stick in red.')
        write(f'xbox_photo_fury_{side}_stick.svg', styled(side, 18, FURY_RED, True, FURY_DROP), 130, 80,
              f'Fury photo mode help (photo_mode_fury.gnf, 2x of 1x), from xbox_controller.svg: the {side} stick in red.')
        # photo_mode.gnf (1x) has the same 65 x 41 sprite as Fury's: Fury's shapes and lines, HD's red.
        write(f'xbox_photo_small_{side}_stick.svg', styled(side, 18, RED, True, FURY_DROP), 130, 82,
              f'Photo mode help at 1x (photo_mode.gnf), from xbox_controller.svg: the {side} stick in red.')


if __name__ == '__main__':
    main()
