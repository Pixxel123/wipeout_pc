# SPDX-License-Identifier: GPL-2.0-or-later
"""Controller button icons for the launcher's Controls page, drawn on a Tk canvas (no images).

Buttons are named as SDL gamepad buttons ('a', 'leftshoulder', 'dpup', ...); the style chooses
PlayStation, Xbox or Nintendo symbols, colors and labels."""

from pad_styles import DPAD, STYLES  # noqa: F401  (scripts/, on the launcher's path)

FACE_COLORS = {
    'playstation': {'a': '#6b9ae8', 'b': '#e5555e', 'x': '#d683d6', 'y': '#45c4a2'},
    'xbox': {'a': '#3fae49', 'b': '#d9372f', 'x': '#2f6fd9', 'y': '#e5b521'},
    'nintendo': dict.fromkeys('abxy', '#cfcfcf'),
}

INK, DIM, BODY = '#e6e0d8', '#5a5250', '#262020'


def spec(button, style):
    """(kind, text, color) of an SDL button in a style, or None for an unknown button.
    'label:<text>' is a small pill with that text (actions such as L3+R3)."""
    if button.startswith('label:'):
        return 'pill', button[len('label:'):], INK
    text = STYLES.get(style, {}).get(button, (None,))[0]
    if not text:
        return None
    if button in FACE_COLORS[style]:
        return ('symbol' if style == 'playstation' else 'letter'), text, FACE_COLORS[style][button]
    kind = ('dpad' if button in DPAD else 'bumper' if button.endswith('shoulder')
            else 'trigger' if button.endswith('trigger') else 'stick' if button.endswith('stick') else 'pill')
    return kind, text, INK


def draw(canvas, button, style, size, font='Segoe UI'):
    """Draws the icon of `button` filling a size × size canvas (cleared first), its text in the
    `font` family."""
    canvas.delete('all')
    found = spec(button, style)
    if not found:
        return
    kind, text, color = found
    s, m = size, size / 2
    if kind in ('symbol', 'letter'):
        canvas.create_oval(1, 1, s - 1, s - 1, fill=BODY if kind == 'symbol' else color, outline='')
        if kind == 'letter':
            canvas.create_text(m, m, text=text, fill='#101010', font=(font, max(7, int(s * 0.42)), 'bold'))
            return
        w, r = max(2, s // 11), s * 0.24
        if text == 'cross':
            canvas.create_line(m - r, m - r, m + r, m + r, fill=color, width=w)
            canvas.create_line(m - r, m + r, m + r, m - r, fill=color, width=w)
        elif text == 'circle':
            canvas.create_oval(m - r, m - r, m + r, m + r, outline=color, width=w)
        elif text == 'square':
            canvas.create_rectangle(m - r, m - r, m + r, m + r, outline=color, width=w)
        else:
            canvas.create_polygon(m, m - r * 1.15, m + r * 1.1, m + r * 0.8, m - r * 1.1, m + r * 0.8,
                                  outline=color, fill='', width=w)
    elif kind == 'dpad':
        t, a = s * 0.3, s * 0.08
        arms = {'up': (m - t / 2, a, m + t / 2, m), 'down': (m - t / 2, m, m + t / 2, s - a),
                'left': (a, m - t / 2, m, m + t / 2), 'right': (m, m - t / 2, s - a, m + t / 2)}
        for name, box in arms.items():
            canvas.create_rectangle(*box, fill=INK if name == text else DIM, outline='')
    elif kind == 'stick':
        canvas.create_oval(1, 1, s - 1, s - 1, fill=BODY, outline=DIM, width=max(1, s // 14))
        canvas.create_text(m, m, text=text, fill=color, font=(font, max(6, int(s * 0.28)), 'bold'))
    else:
        # Bumpers: a flat bar; triggers: taller with a rounded top; the rest: a small pill.
        top, bottom = {'bumper': (s * 0.28, s * 0.72), 'trigger': (s * 0.12, s * 0.88)}.get(kind, (s * 0.3, s * 0.7))
        r = (bottom - top) / 2 if kind != 'trigger' else s * 0.2
        x0, x1 = 1, s - 1
        canvas.create_oval(x0, top, x0 + 2 * r, top + 2 * r, fill=BODY, outline='')
        canvas.create_oval(x1 - 2 * r, top, x1, top + 2 * r, fill=BODY, outline='')
        canvas.create_rectangle(x0 + r, top, x1 - r, bottom, fill=BODY, outline='')
        canvas.create_rectangle(x0, top + r, x1, bottom, fill=BODY, outline='')
        small = (font, max(5, int(s * (0.2 if len(text) > 3 else 0.3))), 'bold')
        canvas.create_text(m, (top + bottom) / 2, text=text, fill=color, font=small)
