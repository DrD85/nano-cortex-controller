#!/usr/bin/env python3
"""Drill and milling templates for a top plate with the Waveshare ESP32-S3-Touch-LCD-4.3 (touch version, no case)
and momentary footswitches.

Writes for every variant a DXF (CNC, mm) and an SVG (mm), plus one PDF with all variants at 1:1 on A4 landscape
and PNG previews. Dimensions of the board: Waveshare dimension drawing (touch version).

Run:  python3 hardware/schablone/make_template.py
"""
import math
import pathlib

OUT = pathlib.Path(__file__).resolve().parent

# ---- Waveshare ESP32-S3-Touch-LCD-4.3, touch version (mm) ----
GLASS_W, GLASS_H = 106.10, 67.80           # cover glass = outline seen from the front
ACTIVE_W, ACTIVE_H = 95.54, 54.36          # visible area
ACTIVE_LEFT, ACTIVE_TOP = 106.10 - 95.54 - 5.33, 67.80 - 54.36 - 9.09   # 5.23 / 4.35 (bezel bottom 9.09, right 5.33)
PCB_W, PCB_H = 106.00, 68.00
PCB_HOLES = [(4, 4), (102, 4), (4, 64), (102, 64)]   # 98 x 60 mm, 4 mm from the PCB edges
POCKET_CLEARANCE = 0.3                     # around the glass
WINDOW_MARGIN = 0.5                        # around the visible area
SWITCH_HOLE = 12.0                         # typical soft-touch footswitch (thread M12)
SWITCH_KEEPOUT = 25.0                      # switch body / nut below the plate

# Styles: (layer, colour) - the layers are the DXF layers
CUT = ('DURCHFRAESEN', (0, 0, 0))
POCKET = ('TASCHE_UNTEN', (0, 90, 230))
DRILL = ('BOHRUNG', (210, 30, 30))
INFO = ('INFO', (150, 150, 150))
OUTLINE = ('KONTUR', (0, 0, 0))


class Drawing:
    def __init__(self, title, w, h):
        self.title, self.w, self.h = title, w, h
        self.items = []
        self.notes = []
        self.has_display = False
        self.rect(0, 0, w, h, OUTLINE)

    def rect(self, x, y, w, h, style, dashed=False):
        self.items.append(('rect', style, dashed, (x, y, w, h)))

    def circle(self, x, y, d, style, dashed=False):
        self.items.append(('circle', style, dashed, (x, y, d / 2)))

    def line(self, x1, y1, x2, y2, style, dashed=False):
        self.items.append(('line', style, dashed, (x1, y1, x2, y2)))

    def text(self, x, y, s, size=2.6, style=INFO):
        self.items.append(('text', style, False, (x, y, s, size)))

    def cross(self, x, y, size, style):
        self.line(x - size, y, x + size, y, style)
        self.line(x, y - size, x, y + size, style)

    def switch(self, x, y, label):
        self.circle(x, y, SWITCH_HOLE, DRILL)
        self.circle(x, y, SWITCH_KEEPOUT, INFO, dashed=True)
        self.cross(x, y, 3, DRILL)
        self.text(x + 7.5, y - 7.5, label, 2.6, DRILL)

    def display(self, gx, gy, label=True):
        """Display with the top-left corner of the glass at (gx, gy); USB-C ports on the right side."""
        self.has_display = True
        self.rect(gx - POCKET_CLEARANCE, gy - POCKET_CLEARANCE, GLASS_W + 2 * POCKET_CLEARANCE,
                  GLASS_H + 2 * POCKET_CLEARANCE, POCKET, dashed=True)
        self.rect(gx + ACTIVE_LEFT - WINDOW_MARGIN, gy + ACTIVE_TOP - WINDOW_MARGIN,
                  ACTIVE_W + 2 * WINDOW_MARGIN, ACTIVE_H + 2 * WINDOW_MARGIN, CUT)
        px, py = gx + (GLASS_W - PCB_W) / 2, gy + (GLASS_H - PCB_H) / 2
        for hx, hy in PCB_HOLES:
            self.cross(px + hx, py + hy, 1.6, INFO)
        if label:
            cx = gx + GLASS_W / 2
            self.text(cx - 22, gy + ACTIVE_TOP + 8, 'Fenster %.1f x %.1f (durchfräsen)'
                      % (ACTIVE_W + 2 * WINDOW_MARGIN, ACTIVE_H + 2 * WINDOW_MARGIN), 2.6, CUT)
            self.text(cx - 22, gy + ACTIVE_TOP + 13, 'Tasche von unten %.1f x %.1f'
                      % (GLASS_W + 2 * POCKET_CLEARANCE, GLASS_H + 2 * POCKET_CLEARANCE), 2.6, POCKET)
            self.text(cx - 22, gy + ACTIVE_TOP + 18, 'Kreuze: Platinenlöcher 98 x 60 (Rückseite)', 2.2, INFO)
            self.text(cx - 9, gy - 1.6, 'OBEN (Glas)', 2.2, INFO)
        plug_zones(self, gx, gy)


def plug_zones(d, gx, gy):
    """Plugs under the plate: USB-C (power) on the right with a 90-degree plug, the I2C cable on the left."""
    pcb_top = gy + (GLASS_H - PCB_H) / 2
    usb_y, i2c_y = pcb_top + 32.90, pcb_top + 29.27
    d.rect(gx + GLASS_W, usb_y - 6, 12, 12, INFO, dashed=True)
    d.text(gx + GLASS_W + 0.8, usb_y + 9, 'USB-C 90°', 2.0, INFO)
    d.rect(gx - 10, i2c_y - 4, 10, 8, INFO, dashed=True)
    d.text(gx - 10, i2c_y + 7, 'I2C', 2.0, INFO)


def variant_a():
    d = Drawing('Variante A: Display + 6 Fußschalter (210 x 115 mm)', 210, 115)
    gx, gy = (210 - GLASS_W) / 2, 8.0
    d.display(gx, gy)
    side_y = gy + GLASS_H / 2
    d.switch(26, side_y, 'FS 1')
    d.switch(184, side_y, 'FS 2')
    for i, x in enumerate((30, 80, 130, 180)):
        d.switch(x, 97, 'FS %d' % (i + 3))
    d.notes = ['Passt auf 21 x 11,5 cm.', 'Je 1 Schalter links/rechts', 'neben dem Display, 4 unten',
               '(Abstand 50 mm).', 'Vorschlag: FS 1 Mode,', 'FS 2 Tuner, FS 3-6 Kacheln;', 'per Learn frei änderbar.',
               'Rechts: USB-C-Kabel mit', '90°-Stecker, sonst stößt', 'er an FS 2.']
    return d


def variant_b():
    d = Drawing('Variante B: 8 Fußschalter (210 x 115 mm), Display separat', 210, 115)
    for row, y in enumerate((30, 85)):
        for col, x in enumerate((27, 79, 131, 183)):
            d.switch(x, y, 'FS %d' % (row * 4 + col + 1))
    d.notes = ['8 Schalter in 2 Reihen,', 'Abstand 52 x 55 mm.', 'Das Display sitzt woanders',
               '(z. B. hinten am Board,', 'Seite 4), verbunden mit', '4 Adern I2C zum SX1509.']
    return d


def variant_c():
    d = Drawing('Variante C: Display + 8 Fußschalter (210 x 170 mm)', 210, 170)
    gx, gy = (210 - GLASS_W) / 2, 8.0
    d.display(gx, gy)
    for row, y in enumerate((100, 150)):
        for col, x in enumerate((30, 80, 130, 180)):
            d.switch(x, y, 'FS %d' % (row * 4 + col + 1))
    d.notes = ['Alles auf einer Platte:', 'braucht 21 x 17 cm.', 'Schalter wie die Kacheln:',
               'obere Reihe 1-4,', 'untere Reihe 5-8.']
    return d


def variant_display():
    d = Drawing('Display einzeln (130 x 90 mm), z. B. für Variante B', 130, 90)
    d.display((130 - GLASS_W) / 2, (90 - GLASS_H) / 2)
    d.notes = ['Fenster durchfräsen,', 'Tasche von unten,', 'damit das Glas bündig', 'anliegt.']
    return d


def variant_f():
    d = Drawing('Variante F: 8 Fu\u00dfschalter (210 x 100 mm)', 210, 100)
    for row, y in enumerate((25, 75)):
        for col, x in enumerate((27, 79, 131, 183)):
            d.switch(x, y, 'FS %d' % (row * 4 + col + 1))
    d.notes = ['2 Reihen wie die Kacheln:', 'hinten FS 1-4, vorne FS 5-8.', 'Abstand 52 x 50 mm,',
               'Rand 27 mm / 25 mm.', 'Display separat (Seite D).']
    return d


def variant_e():
    d = Drawing('Variante E: 8 Fußschalter (290 x 100 mm)', 290, 100)
    for row, y in enumerate((25, 75)):
        for col, x in enumerate((37, 109, 181, 253)):
            d.switch(x, y, 'FS %d' % (row * 4 + col + 1))
    d.notes = ['2 Reihen wie die Kacheln:', 'hinten FS 1-4, vorne FS 5-8.', 'Abstand 72 x 50 mm,',
               'Rand 37 mm / 25 mm.', 'Display separat (Seite D', 'der A4-Schablonen).']
    return d


VARIANTS = [('A_display_6_schalter', variant_a), ('B_8_schalter', variant_b),
            ('C_display_8_schalter', variant_c), ('D_display_einzeln', variant_display),
            ('E_8_schalter_290x100', variant_e), ('F_8_schalter_210x100', variant_f)]

DISPLAY_NOTES = [
    'Legende:',
    '  schwarz: durchfräsen',
    '  blau gestrichelt: Tasche',
    '  von unten (Glas)',
    '  rot: Bohrung Ø 12 mm',
    '  grau gestrichelt: Ø 25 frei',
    '  (Schalterkörper unten)',
    '',
    'Vor dem Fräsen prüfen:',
    '- Board auf den Ausdruck',
    '  legen: USB-C rechts,',
    '  Anzeige aufrecht.',
    '- Ø deiner Taster messen',
    '  (12 / 12,2 / 12,5 mm).',
    '- Taschenecken: Fräserradius',
    '  -> Dogbones oder',
    '  Fräser Ø 2 mm.',
    '- Tasche nur bei Platten',
    '  ab 4 mm; ~1,5 mm Material',
    '  über dem Glas lassen.',
    '- Bautiefe Board 8,8 mm,',
    '  Taster ~30 mm.',
]
SWITCH_NOTES = [
    'Legende:',
    '  schwarz: Plattenkontur',
    '  rot: Bohrung Ø 12 mm',
    '  grau gestrichelt: Ø 25 frei',
    '  (Schalterkörper unten)',
    '',
    'Vor dem Bohren prüfen:',
    '- Ø deiner Taster messen',
    '  (12 / 12,2 / 12,5 mm).',
    '- Mitten ankörnen, dann',
    '  vorbohren (z. B. 4 mm).',
    '- Bautiefe Taster ~30 mm.',
]


# ---- DXF (R12, mm) ----

def dxf(d):
    out = ['0', 'SECTION', '2', 'HEADER', '9', '$INSUNITS', '70', '4', '0', 'ENDSEC',
           '0', 'SECTION', '2', 'ENTITIES']

    def y(v):
        return d.h - v   # DXF y up, origin at the bottom-left corner of the plate

    for kind, (layer, _), dashed, a in d.items:
        if kind == 'rect':
            x, yy, w, h = a
            pts = [(x, y(yy)), (x + w, y(yy)), (x + w, y(yy + h)), (x, y(yy + h))]
            out += ['0', 'POLYLINE', '8', layer, '66', '1', '70', '1', '10', '0', '20', '0', '30', '0']
            for px, py in pts:
                out += ['0', 'VERTEX', '8', layer, '10', '%.3f' % px, '20', '%.3f' % py, '30', '0']
            out += ['0', 'SEQEND', '8', layer]
        elif kind == 'circle':
            x, yy, r = a
            out += ['0', 'CIRCLE', '8', layer, '10', '%.3f' % x, '20', '%.3f' % y(yy), '30', '0', '40', '%.3f' % r]
        elif kind == 'line':
            x1, y1, x2, y2 = a
            out += ['0', 'LINE', '8', layer, '10', '%.3f' % x1, '20', '%.3f' % y(y1), '30', '0',
                    '11', '%.3f' % x2, '21', '%.3f' % y(y2), '31', '0']
        elif kind == 'text':
            x, yy, s, size = a
            s = s.replace('→', '->').replace('←', '<-')
            out += ['0', 'TEXT', '8', layer, '10', '%.3f' % x, '20', '%.3f' % y(yy), '30', '0', '40', '%.2f' % size, '1', s]
    out += ['0', 'ENDSEC', '0', 'EOF']
    return '\n'.join(out) + '\n'


# ---- SVG (mm) ----

def svg(d):
    def esc(s):
        return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%gmm" height="%gmm" viewBox="0 0 %g %g">' % (d.w, d.h, d.w, d.h),
           '<rect width="100%" height="100%" fill="white"/>']
    for kind, (layer, rgb), dashed, a in d.items:
        st = 'fill="none" stroke="rgb(%d,%d,%d)" stroke-width="0.25"%s' % (rgb + (' stroke-dasharray="1.5 1"' if dashed else '',))
        if kind == 'rect':
            out.append('<rect x="%.3f" y="%.3f" width="%.3f" height="%.3f" %s/>' % (a + (st,)))
        elif kind == 'circle':
            out.append('<circle cx="%.3f" cy="%.3f" r="%.3f" %s/>' % (a + (st,)))
        elif kind == 'line':
            out.append('<line x1="%.3f" y1="%.3f" x2="%.3f" y2="%.3f" %s/>' % (a + (st,)))
        elif kind == 'text':
            x, y, s, size = a
            out.append('<text x="%.3f" y="%.3f" font-family="Helvetica, Arial" font-size="%.2f" fill="rgb(%d,%d,%d)">%s</text>'
                       % ((x, y, size) + rgb + (esc(s),)))
    out.append('</svg>')
    return '\n'.join(out) + '\n'


# ---- PDF (A4 landscape, 1:1, vector) ----

PT = 72 / 25.4
PLATE_X, PLATE_Y = 12, 24          # top-left corner of the plate on the page (mm)


class Page:
    """One PDF page: the drawing, the page size (mm) and the part of the plate shown (x0..x1, for tiles)."""
    def __init__(self, d, size=(297, 210), x0=0.0, x1=None, part=None, overlap=None):
        self.d, self.w, self.h = d, size[0], size[1]
        self.x0, self.x1 = x0, d.w if x1 is None else x1
        self.part, self.overlap = part, overlap   # part: 'Blatt 1/2 (links)'; overlap: (from, to) in plate mm


def pdf_text(s):
    table = {'ä': '\\344', 'ö': '\\366', 'ü': '\\374', 'Ä': '\\304', 'Ö': '\\326',
             'Ü': '\\334', 'ß': '\\337', 'Ø': '\\330', '±': '\\261', '×': '\\327',
             '→': '->', '←': '<-', '–': '\\226'}
    s = s.replace('\\', '\\\\').replace('(', '\\(').replace(')', '\\)')
    return ''.join(table.get(ch, ch) for ch in s)


def pdf_page(page):
    d = page.d
    PAGE_H = page.h
    ops = []
    notes_x = PLATE_X + (page.x1 - page.x0) + 8

    def P(x, y):   # plate mm -> page pt
        return (PLATE_X + x - page.x0) * PT, (PAGE_H - PLATE_Y - y) * PT

    # only the shown part of the plate (tiles)
    ops.append('q %.3f %.3f %.3f %.3f re W n' % ((PLATE_X - 1) * PT, 0, (page.x1 - page.x0 + 2) * PT, PAGE_H * PT))

    def stroke(rgb, dashed, width=0.6):
        ops.append('%.3f %.3f %.3f RG %.2f w %s d' % (tuple(c / 255 for c in rgb) + (width, '[2.5 1.8] 0' if dashed else '[] 0')))

    for kind, (layer, rgb), dashed, a in d.items:
        if kind == 'text':
            continue
        stroke(rgb, dashed, 0.8 if layer in ('DURCHFRAESEN', 'KONTUR') else 0.6)
        if kind == 'rect':
            x, y, w, h = a
            px, py = P(x, y + h)
            ops.append('%.3f %.3f %.3f %.3f re S' % (px, py, w * PT, h * PT))
        elif kind == 'line':
            x1, y1, x2, y2 = a
            ops.append('%.3f %.3f m %.3f %.3f l S' % (P(x1, y1) + P(x2, y2)))
        elif kind == 'circle':
            x, y, r = a
            cx, cy = P(x, y)
            r *= PT
            k = 0.5523 * r
            ops.append('%.3f %.3f m %.3f %.3f %.3f %.3f %.3f %.3f c %.3f %.3f %.3f %.3f %.3f %.3f c '
                       '%.3f %.3f %.3f %.3f %.3f %.3f c %.3f %.3f %.3f %.3f %.3f %.3f c S' % (
                           cx + r, cy, cx + r, cy + k, cx + k, cy + r, cx, cy + r,
                           cx - k, cy + r, cx - r, cy + k, cx - r, cy,
                           cx - r, cy - k, cx - k, cy - r, cx, cy - r,
                           cx + k, cy - r, cx + r, cy - k, cx + r, cy))

    def text(x_pt, y_pt, s, size, rgb=(0, 0, 0), bold=False):
        ops.append('BT %.3f %.3f %.3f rg /%s %.2f Tf %.3f %.3f Td (%s) Tj ET'
                   % (tuple(c / 255 for c in rgb) + ('F2' if bold else 'F1', size, x_pt, y_pt, pdf_text(s))))

    for kind, (layer, rgb), dashed, a in d.items:
        if kind == 'text':
            x, y, s, size = a
            px, py = P(x, y)
            text(px, py, s, size * PT * 0.95, rgb)
    if page.overlap:   # registration crosses in the overlap and the edges to cut / glue
        a, b = page.overlap
        m = (a + b) / 2
        stroke((0, 0, 0), False, 0.6)
        for yy in (d.h + 8, d.h + 22):
            cx, cy = P(m, yy)
            ops.append('%.3f %.3f m %.3f %.3f l S %.3f %.3f m %.3f %.3f l S'
                       % (cx - 4 * PT, cy, cx + 4 * PT, cy, cx, cy - 4 * PT, cx, cy + 4 * PT))
        if page.x0 > 0:   # right sheet: cut along its left edge
            stroke((0, 0, 0), False, 0.6)
            ops.append('%.3f %.3f m %.3f %.3f l S' % (P(a, -2) + P(a, d.h + 28)))
            text(P(a, d.h + 33)[0] + 2, P(a, d.h + 33)[1], 'Hier abschneiden, auf Blatt 1 legen (Kreuze decken sich)', 7)
        else:             # left sheet: glue area
            stroke((120, 120, 120), True, 0.6)
            ops.append('%.3f %.3f m %.3f %.3f l S' % (P(a, -2) + P(a, d.h + 28)))
            text(P(a, d.h + 33)[0] - 30 * PT, P(a, d.h + 33)[1], 'Klebefläche (10 mm) ->', 7)
    ops.append('Q')

    text(PLATE_X * PT, (PAGE_H - 12) * PT, d.title + (' - ' + page.part if page.part else ''), 13, bold=True)
    text(PLATE_X * PT, (PAGE_H - 18) * PT, 'Maßstab 1:1 - beim Drucken „Tatsächliche Größe“ / 100 % wählen. '
         'Maße in mm, Ursprung oben links.', 8.5)
    # 100 mm scale bar under the notes
    bx, by = notes_x * PT, 18 * PT
    stroke((0, 0, 0), False, 0.8)
    ops.append('%.3f %.3f m %.3f %.3f l S' % (bx, by, bx + 50 * PT, by))
    for i in range(6):
        ops.append('%.3f %.3f m %.3f %.3f l S' % (bx + i * 10 * PT, by, bx + i * 10 * PT, by + (2.5 if i % 5 == 0 else 1.5) * PT))
    text(bx, by - 4 * PT, 'Kontrolle: 50 mm', 7)
    y = (PAGE_H - PLATE_Y - 2) * PT
    for line in d.notes + [''] + (DISPLAY_NOTES if d.has_display else SWITCH_NOTES):
        text(notes_x * PT, y, line, 7.4, bold=line.endswith(':') and not line.startswith(' '))
        y -= 3.6 * PT
    return '\n'.join(ops).replace('„', '"').replace('“', '"')


def pdf(pages):
    objs = []

    def add(body):
        objs.append(body)
        return len(objs)

    f1 = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>')
    f2 = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>')
    pages_id = len(objs) + 1 + 2 * len(pages)
    kids = []
    for page in pages:
        content = pdf_page(page).encode('latin-1', 'replace')
        cid = add(b'<< /Length %d >>\nstream\n' % len(content) + content + b'\nendstream')
        kids.append(add('<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R '
                        '/Resources << /Font << /F1 %d 0 R /F2 %d 0 R >> >> >>'
                        % (pages_id, page.w * PT, page.h * PT, cid, f1, f2)))
    assert add('<< /Type /Pages /Kids [%s] /Count %d >>' % (' '.join('%d 0 R' % k for k in kids), len(kids))) == pages_id
    root = add('<< /Type /Catalog /Pages %d 0 R >>' % pages_id)
    data = bytearray(b'%PDF-1.4\n')
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(len(data))
        data += b'%d 0 obj\n' % i + (body if isinstance(body, bytes) else body.encode('latin-1')) + b'\nendobj\n'
    xref = len(data)
    data += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objs) + 1)
    for off in offsets:
        data += b'%010d 00000 n \n' % off
    data += b'trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs) + 1, root, xref)
    return bytes(data)


# ---- PNG preview ----

def png(d, path, scale=4):
    from PIL import Image, ImageDraw
    img = Image.new('RGB', (int(d.w * scale) + 20, int(d.h * scale) + 20), 'white')
    g = ImageDraw.Draw(img)
    o = 10
    for kind, (layer, rgb), dashed, a in d.items:
        if kind == 'rect':
            x, y, w, h = a
            g.rectangle([o + x * scale, o + y * scale, o + (x + w) * scale, o + (y + h) * scale], outline=rgb, width=2)
        elif kind == 'circle':
            x, y, r = a
            g.ellipse([o + (x - r) * scale, o + (y - r) * scale, o + (x + r) * scale, o + (y + r) * scale], outline=rgb, width=2)
        elif kind == 'line':
            x1, y1, x2, y2 = a
            g.line([o + x1 * scale, o + y1 * scale, o + x2 * scale, o + y2 * scale], fill=rgb, width=2)
        elif kind == 'text':
            x, y, s, size = a
            g.text((o + x * scale, o + (y - size) * scale), s, fill=rgb)
    img.save(path)


def main():
    a4 = []
    for name, make in VARIANTS:
        d = make()
        (OUT / ('schablone_%s.dxf' % name)).write_text(dxf(d))
        (OUT / ('schablone_%s.svg' % name)).write_text(svg(d))
        png(d, OUT / ('vorschau_%s.png' % name))
        if d.w <= 210:
            a4.append(Page(d))
            (OUT / ('schablone_%s.pdf' % name)).write_bytes(pdf([Page(d)]))
        else:   # wider than A4: one A3 page, and two A4 sheets to glue together
            (OUT / ('schablone_%s_A3.pdf' % name)).write_bytes(pdf([Page(d, size=(420, 297))]))
            half = d.w / 2
            (OUT / ('schablone_%s_2xA4.pdf' % name)).write_bytes(pdf([
                Page(d, x0=0, x1=half + 5, part='Blatt 1/2 (links)', overlap=(half - 5, half + 5)),
                Page(d, x0=half - 5, x1=d.w, part='Blatt 2/2 (rechts)', overlap=(half - 5, half + 5))]))
    (OUT / 'schablonen_1zu1_A4.pdf').write_bytes(pdf(a4))
    print('\n'.join(sorted(p.name for p in OUT.iterdir() if p.suffix in ('.pdf', '.dxf', '.svg', '.png'))))


if __name__ == '__main__':
    main()
