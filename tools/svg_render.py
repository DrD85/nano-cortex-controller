"""Renders small line icons (24x24 SVG subset) into 8-bit alpha masks with Pillow.

Supported: <path d> with M L H V C S Q A Z (absolute and relative), <circle>, <rect> (with rx), <line>.
Elements are stroked; fill="..." (anything but "none") fills them as well. No other SVG features.
"""
import math
import re

from PIL import Image, ImageDraw

_TOKEN = re.compile(r'[MmLlHhVvCcSsQqAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')
_ELEMENT = re.compile(r'<(path|circle|rect|line)\b([^>]*)/?>')
_ATTR = re.compile(r'([\w-]+)="([^"]*)"')
_ARGS = {'M': 2, 'L': 2, 'H': 1, 'V': 1, 'C': 6, 'S': 4, 'Q': 4, 'A': 7, 'Z': 0}
_CURVE_STEPS = 24


def _cubic(p0, p1, p2, p3):
    out = []
    for i in range(1, _CURVE_STEPS + 1):
        t = i / _CURVE_STEPS
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
    return out


def _arc(p1, rx, ry, phi, large, sweep, p2):
    """Endpoint arc (SVG A) as points, via the centre parameterisation of the SVG spec."""
    if rx == 0 or ry == 0 or p1 == p2:
        return [p2]
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (p1[0] - p2[0]) / 2, (p1[1] - p2[1]) / 2
    x1, y1 = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    den = rx * rx * y1 * y1 + ry * ry * x1 * x1
    coef = math.sqrt(max(0.0, num / den)) * (-1 if large == sweep else 1)
    cx1, cy1 = coef * rx * y1 / ry, -coef * ry * x1 / rx
    cx = cp * cx1 - sp * cy1 + (p1[0] + p2[0]) / 2
    cy = sp * cx1 + cp * cy1 + (p1[1] + p2[1]) / 2

    def angle(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)

    t1 = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    dt = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    steps = max(4, int(abs(dt) / (math.pi / 32)))
    out = []
    for i in range(1, steps + 1):
        t = t1 + dt * i / steps
        x, y = rx * math.cos(t), ry * math.sin(t)
        out.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return out


def path_points(d):
    """Returns the subpaths of a path as (points, closed)."""
    tokens = _TOKEN.findall(d)
    subpaths, points = [], []
    cur = start = (0.0, 0.0)
    last_ctrl = None
    cmd = None
    i = 0
    while i < len(tokens):
        if tokens[i].isalpha():
            cmd = tokens[i]
            i += 1
            if cmd in 'Zz':
                if points:
                    subpaths.append((points, True))
                points = []
                cur = start
                last_ctrl = None
                continue
        n = _ARGS[cmd.upper()]
        args = [float(t) for t in tokens[i:i + n]]
        i += n
        rel = cmd.islower()
        up = cmd.upper()
        ox, oy = cur if rel else (0.0, 0.0)
        if up == 'M':
            if points:
                subpaths.append((points, False))
            cur = start = (ox + args[0], oy + args[1])
            points = [cur]
            cmd = 'l' if rel else 'L'   # further pairs are line-tos
            last_ctrl = None
            continue
        if up == 'L':
            new = [(ox + args[0], oy + args[1])]
        elif up == 'H':
            new = [((cur[0] if rel else 0) + args[0], cur[1])]
        elif up == 'V':
            new = [(cur[0], (cur[1] if rel else 0) + args[0])]
        elif up == 'C':
            c1, c2, end = (ox + args[0], oy + args[1]), (ox + args[2], oy + args[3]), (ox + args[4], oy + args[5])
            new = _cubic(cur, c1, c2, end)
            last_ctrl = c2
        elif up == 'S':
            c1 = (2 * cur[0] - last_ctrl[0], 2 * cur[1] - last_ctrl[1]) if last_ctrl else cur
            c2, end = (ox + args[0], oy + args[1]), (ox + args[2], oy + args[3])
            new = _cubic(cur, c1, c2, end)
            last_ctrl = c2
        elif up == 'Q':
            q, end = (ox + args[0], oy + args[1]), (ox + args[2], oy + args[3])
            c1 = (cur[0] + 2 / 3 * (q[0] - cur[0]), cur[1] + 2 / 3 * (q[1] - cur[1]))
            c2 = (end[0] + 2 / 3 * (q[0] - end[0]), end[1] + 2 / 3 * (q[1] - end[1]))
            new = _cubic(cur, c1, c2, end)
        elif up == 'A':
            new = _arc(cur, args[0], args[1], args[2], int(args[3]), int(args[4]), (ox + args[5], oy + args[6]))
        if up not in 'CS':
            last_ctrl = None
        if not points:
            points = [cur]
        points += new
        cur = new[-1]
    if points:
        subpaths.append((points, False))
    return subpaths


def _circle(cx, cy, r):
    steps = 64
    return [(cx + r * math.cos(2 * math.pi * i / steps), cy + r * math.sin(2 * math.pi * i / steps)) for i in range(steps)]


def _element_subpaths(tag, a):
    f = lambda k, default=0.0: float(a.get(k, default))
    if tag == 'path':
        return path_points(a['d'])
    if tag == 'circle':
        return [(_circle(f('cx'), f('cy'), f('r')), True)]
    if tag == 'line':
        return [([(f('x1'), f('y1')), (f('x2'), f('y2'))], False)]
    x, y, w, h, r = f('x'), f('y'), f('width'), f('height'), f('rx')
    if r <= 0:
        return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], True)]
    d = 'M%g %gH%gA%g %g 0 0 1 %g %gV%gA%g %g 0 0 1 %g %gH%gA%g %g 0 0 1 %g %gV%gA%g %g 0 0 1 %g %gZ' % (
        x + r, y, x + w - r, r, r, x + w, y + r, y + h - r, r, r, x + w - r, y + h, x + r, r, r, x, y + h - r,
        y + r, r, r, x + r, y)
    return path_points(d)


def render(svg_body, size, stroke=1.5, view=24, supersample=8):
    """Renders the elements of an icon (inner SVG markup) into a size x size alpha mask (bytes)."""
    k = size * supersample / view
    big = size * supersample
    img = Image.new('L', (big, big), 0)
    d = ImageDraw.Draw(img)
    for tag, attrs in _ELEMENT.findall(svg_body):
        a = dict(_ATTR.findall(attrs))
        width = float(a.get('stroke-width', stroke))
        for points, closed in _element_subpaths(tag, a):
            pts = [(x * k, y * k) for x, y in points]
            if a.get('fill', 'none') != 'none' and len(pts) > 2:
                d.polygon(pts, fill=255)
            if a.get('stroke', '') == 'none':
                continue
            if closed:
                pts = pts + [pts[0]]
            w = width * k
            for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
                length = math.hypot(x1 - x0, y1 - y0)
                if length == 0:
                    continue
                nx, ny = -(y1 - y0) / length * w / 2, (x1 - x0) / length * w / 2
                d.polygon([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)], fill=255)
            for x, y in pts:   # round joins and caps
                d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=255)
    return img.resize((size, size), Image.LANCZOS)
