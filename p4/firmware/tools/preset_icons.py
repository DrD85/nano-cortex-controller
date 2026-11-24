#!/usr/bin/env python3
"""Preset symbols for the own banks (own drawings, 24x24 line icons in the style of the tile icons).

Writes main/preset_icons.c (LVGL A8 masks). Run from the project folder:  python3 tools/preset_icons.py
With --preview FILE it also writes a PNG contact sheet.
"""
import math
import re
import pathlib
import sys

from svg_render import path_points, render

ROOT = pathlib.Path(__file__).resolve().parent.parent
ICON_SIZE = 36
STROKE = 0.9   # about the line weight of the effect icons


def star_path(cx, cy, outer, inner, points=5, rotation=-90):
    pts = []
    for i in range(points * 2):
        r = outer if i % 2 == 0 else inner
        a = math.radians(rotation + i * 180 / points)
        pts.append('%.2f %.2f' % (cx + r * math.cos(a), cy + r * math.sin(a)))
    return 'M' + 'L'.join(pts) + 'Z'


def polyline(f, steps=80):
    return 'M' + 'L'.join('%.2f %.2f' % f(i / steps) for i in range(steps + 1))


def soft_clipped_wave(k=2.5, amp=6.5):
    """One period of tanh(k sin x): a sine with flattened peaks (softer than the overdrive trapezoid)."""
    return polyline(lambda t: (2 + 20 * t, 12 - amp * math.tanh(k * math.sin(2 * math.pi * t)) / math.tanh(k)))


def rotated(d, degrees, cx=12, cy=12, scale=1.0, dx=0, dy=0):
    """A path with absolute M L C Q Z commands, scaled and rotated around (cx, cy), then moved by (dx, dy)."""
    a = math.radians(degrees)
    out = []
    for cmd, args in re.findall(r'([MLCQZ])([^MLCQZ]*)', d):
        nums = [float(n) for n in re.findall(r'[-+]?\d*\.?\d+', args)]
        pts = []
        for i in range(0, len(nums), 2):
            x, y = (nums[i] - cx) * scale, (nums[i + 1] - cy) * scale
            pts.append('%.2f %.2f' % (cx + dx + x * math.cos(a) - y * math.sin(a), cy + dy + x * math.sin(a) + y * math.cos(a)))
        out.append(cmd + 'L'.join(pts) if cmd in 'ML' else cmd + ' '.join(pts))
    return ''.join(out)


def flattened(d):
    """A path as straight segments (so it can be rotated even with arcs in it)."""
    points, closed = path_points(d)[0]
    return 'M' + 'L'.join('%.2f %.2f' % p for p in points) + ('Z' if closed else '')


def fuzzy_square_wave():
    """A hard-clipped square wave whose flat tops are fuzzy (zigzag)."""
    def zigzag(x0, x1, y, amp=0.85, period=2.5):
        n = int(round((x1 - x0) / (period / 2)))
        return 'L'.join('%.2f %.2f' % (x0 + i * (x1 - x0) / n, y + (0 if i in (0, n) else amp if i % 2 == 0 else -amp))
                        for i in range(n + 1))
    return ('<path d="M2 12V6.6L%s"/><path d="M12 6.6V17.4L%s"/><path d="M22 17.4V12"/>'
            % (zigzag(2, 12, 6.6), zigzag(12, 22, 17.4)))


def guitarist():
    """Silhouette: head bowed, long straight hair hanging over the face, legs apart, a Strat-like guitar."""
    body = ('<path fill="#000" d="M6.2 11.6C6.2 10.2 7.2 9.8 9.0 9.8H13.8C15.6 9.8 16.6 10.2 16.6 11.6L13.8 15.6H9.2Z"/>'
            '<path fill="#000" d="M9.2 15.0H11.4L8.2 23.0H6.2Z"/><path fill="#000" d="M11.8 15.0H13.8L17.2 23.0H15.2Z"/>'
            '<path stroke-width="1.6" d="M6.8 11.6L6.0 14.0L9.8 15.6"/>'      # strumming arm
            '<path stroke-width="1.4" d="M16.0 11.6L17.4 13.4L19.8 10.6"/>')  # fretting arm
    # Round skull, hair straight down from the temples with ragged ends; the head bows 18 degrees.
    cx, w, top, bottom = 11.6, 2.0, 2.6, 9.4
    hair = ('M{l} {mid}A{w} {w} 0 0 1 {r} {mid}C{r2} {m2} {r2} {b1} {r3} {bottom}L{s1} {sb}L{s2} {bottom}L{s3} {sb}'
            'L{s4} {bottom}L{s5} {sb}L{l3} {bottom}C{l2} {b1} {l2} {m2} {l} {mid}Z').format(
        l=cx - w, r=cx + w, w=w, mid=top + w, r2=cx + w + 0.15, l2=cx - w - 0.15, m2=top + w + 2.5, b1=bottom - 2,
        r3=cx + w + 0.35, l3=cx - w - 0.35, bottom=bottom, sb=bottom - 1.2,
        s1=cx + w * 0.55, s2=cx + w * 0.15, s3=cx - w * 0.2, s4=cx - w * 0.55, s5=cx - w * 0.85)
    hair = rotated(flattened(hair), 18, cx, 9.5)
    # Guitar across the body, neck up to the right; a cut (gap) separates it from the player.
    angle, size, at = -34, 1.25, (0.6, 0.8)
    guitar = rotated('M8.2 13.2C8.6 12.0 10.0 11.8 10.8 12.6C11.6 12.0 13.2 12.0 13.6 13.2C14.2 14.8 13.4 16.6 11.4 17.0'
                     'C9.4 17.4 7.6 16.6 7.6 15.2C7.6 14.4 8.0 13.8 8.2 13.2Z', angle, 10.6, 14.6, size, *at)
    neck = rotated('M%.2f 14.4L22.4 14.4' % (10.6 + 3.0 * size), angle, 10.6, 14.6, 1.0, *at)
    head = rotated('M22.4 14.4L24.0 14.4', angle, 10.6, 14.6, 1.0, *at)
    return (body + '<path cut="1" stroke-width="1.1" fill="#000" d="%s"/><path fill="#000" d="%s"/>' % (hair, hair) +
            '<path cut="1" stroke-width="2.2" fill="#000" d="%s"/><path cut="1" stroke-width="2.4" d="%s"/>'
            '<path fill="#000" d="%s"/><path stroke-width="0.95" d="%s"/><path stroke-width="1.6" d="%s"/>'
            % (guitar, neck, guitar, neck, head))


def rocket():
    """Rocket flying up to the right: body with a window, two fins, exhaust flame."""
    parts = ['M12 2.2C14.9 4.4 15.9 8.2 15.6 14.6L8.4 14.6C8.1 8.2 9.1 4.4 12 2.2Z',
             'M8.6 10.8L5.6 14.6L5.6 17.6L8.4 15.6', 'M15.4 10.8L18.4 14.6L18.4 17.6L15.6 15.6',
             'M10.4 16.8Q10.8 19.6 12 22Q13.2 19.6 13.6 16.8']
    window = rotated('M12 8.6', 45, scale=1.08)[1:].split(' ')
    return (''.join('<path d="%s"/>' % rotated(p, 45, scale=1.08) for p in parts) +
            '<circle cx="%s" cy="%s" r="1.7"/>' % tuple(window))


def planet():
    """Planet with a tilted ring (its back half hidden behind the planet) and three stars."""
    cx, cy, r, rx, ry, tilt, gap = 12, 12.5, 6.0, 10.6, 3.1, math.radians(-18), 1.0

    def ring_local(x, y):   # point in the ring's own (untilted) coordinates
        dx, dy = x - cx, y - cy
        return dx * math.cos(-tilt) - dy * math.sin(-tilt), dx * math.sin(-tilt) + dy * math.cos(-tilt)

    def visible_runs(points, hidden):
        runs, cur = [], []
        for p in points:
            if hidden(p):
                if len(cur) > 1:
                    runs.append(cur)
                cur = []
            else:
                cur.append(p)
        if len(cur) > 1:
            runs.append(cur)
        return ''.join('<path d="M%s"/>' % 'L'.join('%.2f %.2f' % p for p in run) for run in runs)

    angles = [math.radians(i) for i in range(0, 361, 3)]
    ring = [(cx + rx * math.cos(t) * math.cos(tilt) - ry * math.sin(t) * math.sin(tilt),
             cy + rx * math.cos(t) * math.sin(tilt) + ry * math.sin(t) * math.cos(tilt)) for t in angles]
    disc = [(cx + r * math.cos(t), cy + r * math.sin(t)) for t in angles]
    behind = lambda p: ring_local(*p)[1] < 0 and math.hypot(p[0] - cx, p[1] - cy) < r + gap
    in_front = lambda p: ring_local(*p)[1] > 0 and abs((ring_local(*p)[0] / rx) ** 2 + (ring_local(*p)[1] / ry) ** 2 - 1) < 0.35
    return (visible_runs(disc, in_front) + visible_runs(ring, behind) +
            '<circle cx="20" cy="4" r="0.9" fill="#000"/><circle cx="4.5" cy="20" r="0.7" fill="#000"/>'
            '<circle cx="5" cy="5.5" r="0.6" fill="#000"/>')


def swell():
    """Three waves, growing from top to bottom (pads, swells)."""
    return ''.join('<path d="%s"/>' % polyline(lambda t, y=y, amp=amp: (2.5 + 19 * t, y - amp * math.sin(4 * math.pi * t)), 90)
                   for y, amp in ((6.5, 1.2), (12, 1.8), (17.5, 2.4)))


# Name shown in the bank editor, then the drawing.
PRESET_ICONS = [
    ('Clean',        # sparkle: a clear, glassy sound
     '<path d="M11 3Q12 11 20 12Q12 13 11 21Q10 13 2 12Q10 11 11 3Z"/>'
     '<path d="M19 2.5Q19.4 4.6 21.5 5Q19.4 5.4 19 7.5Q18.6 5.4 16.5 5Q18.6 4.6 19 2.5Z"/>'),
    ('Edge',         # a sine whose peaks just start to flatten: edge of breakup
     '<path d="%s"/>' % soft_clipped_wave()),
    ('Drive',        # flame
     '<path d="M12 22C7.8 22 5.5 19 5.5 15.6C5.5 12.2 8 10.3 9 7.8C9.8 9.6 10.6 10.5 11.6 11C11.4 7.6 13 4.6 15.4 2.5'
     'C15.2 5.6 16.6 7.6 17.6 9.4C18.3 10.8 18.5 12.2 18.5 13.8C18.5 18.8 15.8 22 12 22Z"/>'
     '<path d="M12 22C10.2 22 9 20.7 9 19C9 17.2 10.4 16.1 11.2 14.4C11.8 15.6 12.5 16.2 13.4 16.6'
     'C13.9 15.8 14.2 15 14.3 14C15.2 15.2 15.8 16.5 15.8 17.9C15.8 20.3 14.2 22 12 22Z"/>'),
    ('Solo',         # star
     '<path d="%s"/>' % star_path(12, 12.8, 10, 4.1)),
    ('Fuzz',         # spiky fuzz ball
     '<path d="%s"/>' % star_path(12, 12, 10, 6.8, 16)),
    ('Atmospheric',  # moon and stars
     '<path d="M19.5 15.5A8.5 8.5 0 1 1 9 4.2A7 7 0 0 0 19.5 15.5Z"/>'
     '<circle cx="16.5" cy="4.5" r="1" fill="#000"/><circle cx="21" cy="9.5" r="0.8" fill="#000"/>'
     '<circle cx="13" cy="9" r="0.6" fill="#000"/>'),
    # Added later: keep the order, the banks store the position.
    ('Metal',        # lightning bolt
     '<path d="M13.5 2L5 13.5H11.5L10.5 22L19 10.5H12.5Z"/>'),
    ('Boost',        # double chevron up
     '<path d="M5 12.5L12 5.5L19 12.5"/><path d="M5 19L12 12L19 19"/>'),
    ('Rhythm',       # metronome
     '<path d="M9.8 3H14.2L17.5 21H6.5Z"/><path d="M12 17.5L17.5 6"/><path d="M5 21H19"/>'
     '<circle cx="15.6" cy="10" r="1.3" fill="#000"/>'),
    ('Bass',         # bass clef
     '<path d="M5.2 8.6C5.2 5.6 7.6 3.5 10.6 3.5C14.2 3.5 16.2 6 16.2 9.6C16.2 15 11.4 19.4 5 21.5"/>'
     '<circle cx="6.6" cy="8.6" r="1.7" fill="#000"/>'
     '<circle cx="19.4" cy="7" r="1" fill="#000"/><circle cx="19.4" cy="12" r="1" fill="#000"/>'),
    ('Acoustic',     # acoustic guitar
     '<path d="M12 8.2C9.3 8.2 8.2 9.6 8.6 11.2C8.9 12.3 8.1 12.8 7.3 13.7C6 15.1 5.8 17.1 6.6 18.9'
     'C7.6 21 9.8 22 12 22C14.2 22 16.4 21 17.4 18.9C18.2 17.1 18 15.1 16.7 13.7C15.9 12.8 15.1 12.3 15.4 11.2'
     'C15.8 9.6 14.7 8.2 12 8.2Z"/>'
     '<circle cx="12" cy="15.2" r="1.9"/><path d="M11 8.2V2H13V8.2"/><path d="M10.2 19.2H13.8"/>'),
    ('Blues',        # eighth note
     '<circle cx="9.5" cy="17.5" r="3"/><path d="M12.5 17.5V3C12.5 6 18 6.5 18 11"/>'),
    ('Live',         # microphone
     '<rect x="9" y="2.5" width="6" height="11" rx="3"/><path d="M6 10.5A6 6 0 0 0 18 10.5"/>'
     '<path d="M12 16.5V21"/><path d="M8.5 21H15.5"/>'),
    ('Favorite',     # heart
     '<path d="M12 20.5C6.2 16.6 3 13.1 3 9.3C3 6.6 5 4.5 7.6 4.5C9.5 4.5 11 5.5 12 7.1'
     'C13 5.5 14.5 4.5 16.4 4.5C19 4.5 21 6.6 21 9.3C21 13.1 17.8 16.6 12 20.5Z"/>'),
    ('Fuzz Wave', fuzzy_square_wave()),
    ('Guitarist', guitarist()),
    ('Rocket', rocket()),
    ('Space', planet()),
    ('Swell', swell()),
]
# The bank editor shows "none" plus the symbols in two rows of ten, and 'B' carries the position in 5 bits.
assert len(PRESET_ICONS) <= 19


def c_ident(name):
    return ''.join(ch if ch.isalnum() else '_' for ch in name).lower()


def generate():
    out = ['// Generated by tools/preset_icons.py - do not edit.',
           '// Symbols for the own banks (own drawings) as LVGL A8 masks, drawn in the tile colour.',
           '#include "preset_icons.h"', '']
    names = []
    for name, body in PRESET_ICONS:
        ident = 'preset_icon_' + c_ident(name)
        names.append((name, ident))
        data = render(body, ICON_SIZE, STROKE).tobytes()
        out.append('static const uint8_t %s_map[] = {' % ident)
        for i in range(0, len(data), 24):
            out.append('    ' + ', '.join('0x%02X' % b for b in data[i:i + 24]) + ',')
        out += ['};',
                'static const lv_image_dsc_t %s = {' % ident,
                '    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = %d, .h = %d, .stride = %d },'
                % (ICON_SIZE, ICON_SIZE, ICON_SIZE),
                '    .data_size = sizeof(%s_map),' % ident,
                '    .data = %s_map,' % ident,
                '};', '']
    out.append('const ui_preset_icon_t PRESET_ICONS[PRESET_ICON_COUNT] = {')
    out += ['    { %s, &%s },' % ('"%s"' % name, ident) for name, ident in names]
    out += ['};', '']
    (ROOT / 'main' / 'preset_icons.c').write_text('\n'.join(out))

    header = ['// Generated by tools/preset_icons.py - do not edit.',
              '// Symbols for the own banks; index 0 in the bank data means "no symbol", n means PRESET_ICONS[n - 1].',
              '#pragma once', '', '#include "lvgl.h"', '',
              '#define PRESET_ICON_COUNT %d' % len(PRESET_ICONS), '',
              'typedef struct {', '    const char *name;', '    const lv_image_dsc_t *image;', '} ui_preset_icon_t;', '',
              'extern const ui_preset_icon_t PRESET_ICONS[PRESET_ICON_COUNT];', '']
    (ROOT / 'main' / 'preset_icons.h').write_text('\n'.join(header))
    return len(names)


def preview(path, extra=()):
    """Contact sheet: each symbol large and at tile size, on dark and on a coloured tile."""
    from PIL import Image, ImageDraw
    items = list(PRESET_ICONS)
    cell, per_row = 150, 7
    rows = (len(items) + per_row - 1) // per_row
    sheet = Image.new('RGB', (cell * per_row, 250 * rows), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    colors = [(0xF2, 0xF2, 0xF2), (0xFF, 0xD2, 0x36), (0xFF, 0x70, 0x00), (0x3D, 0x8B, 0xFF), (0xFF, 0x4D, 0x4D), (0x00, 0xFF, 0xDD)]
    for i, (name, body) in enumerate(items):
        x, y = (i % per_row) * cell, (i // per_row) * 250
        sheet.paste((255, 112, 0), (x + 27, y + 10), render(body, 96, STROKE))
        color = colors[i % len(colors)]
        d.rounded_rectangle([x + 20, y + 130, x + 130, y + 216], radius=10, fill=color)
        sq = (0, 0, 0) if sum(color) > 380 else (255, 255, 255)
        d.rounded_rectangle([x + 28, y + 138, x + 74, y + 184], radius=11, fill=tuple(int(c * 0.8) for c in color))
        sheet.paste(sq, (x + 33, y + 143), render(body, ICON_SIZE, STROKE))
        d.text((x + 10, y + 226), name, fill=(220, 220, 220))
    sheet.save(path)


if __name__ == '__main__':
    print('preset_icons.c:', generate(), 'symbols')
    if len(sys.argv) > 2 and sys.argv[1] == '--preview':
        preview(sys.argv[2])
