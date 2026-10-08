#!/usr/bin/env python3
"""Board fonts from IBM Plex Sans (the typeface of Neural DSP's Cortex Control and of the desktop editor).

Converts main/fonts/IBMPlexSans-Medium.ttf and -SemiBold.ttf (unchanged files from IBM, SIL Open Font License 1.1,
main/fonts/OFL.txt) into LVGL bitmap fonts (4 bpp, Latin-1 plus a few punctuation marks, with the font's kerning) and
writes main/ui_fonts.c / main/ui_fonts.h. The converted fonts are a "Modified Version" in the sense of the OFL, so they
do not carry the reserved name: they are called ui_font_<size> and ui_font_title_<size>. LV_SYMBOL_* icons come from
the built-in Montserrat fonts (fallback). Run from the project folder:  python3 tools/gen_fonts.py
"""
import pathlib
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONTS = ROOT / 'main' / 'fonts'

# name, file, size (px), fallback for the LV_SYMBOL icons
SPECS = [
    ('ui_font_12', 'IBMPlexSans-Medium.ttf', 12, 'lv_font_montserrat_14'),
    ('ui_font_14', 'IBMPlexSans-Medium.ttf', 14, 'lv_font_montserrat_14'),
    ('ui_font_16', 'IBMPlexSans-Medium.ttf', 16, 'lv_font_montserrat_14'),
    ('ui_font_20', 'IBMPlexSans-Medium.ttf', 20, 'lv_font_montserrat_20'),
    ('ui_font_24', 'IBMPlexSans-Medium.ttf', 24, 'lv_font_montserrat_20'),
    ('ui_font_28', 'IBMPlexSans-Medium.ttf', 28, 'lv_font_montserrat_28'),
    ('ui_font_title_22', 'IBMPlexSans-SemiBold.ttf', 22, 'lv_font_montserrat_20'),
    ('ui_font_title_26', 'IBMPlexSans-SemiBold.ttf', 26, 'lv_font_montserrat_28'),
    ('ui_font_title_30', 'IBMPlexSans-SemiBold.ttf', 30, 'lv_font_montserrat_28'),
    ('ui_font_title_34', 'IBMPlexSans-SemiBold.ttf', 34, 'lv_font_montserrat_28'),
    ('ui_font_title_40', 'IBMPlexSans-SemiBold.ttf', 40, 'lv_font_montserrat_48'),
    ('ui_font_title_46', 'IBMPlexSans-SemiBold.ttf', 46, 'lv_font_montserrat_48'),
]
RANGES = [(0x20, 0x7E), (0xA0, 0xFF)]
EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2212]


# ---- just enough of the TrueType/OpenType tables: cmap (format 4), unitsPerEm and GPOS pair kerning ----
class Ttf:
    def __init__(self, path):
        self.d = path.read_bytes()
        count = self.u16(4)
        self.tables = {self.d[12 + 16 * i:16 + 16 * i].decode('latin1'): self.u32(20 + 16 * i) for i in range(count)}
        self.units = self.u16(self.tables['head'] + 18)
        self.cmap = self.read_cmap()

    def u16(self, o): return struct.unpack_from('>H', self.d, o)[0]
    def s16(self, o): return struct.unpack_from('>h', self.d, o)[0]
    def u32(self, o): return struct.unpack_from('>I', self.d, o)[0]

    def read_cmap(self):
        base = self.tables['cmap']
        for i in range(self.u16(base + 2)):
            platform, encoding, offset = self.u16(base + 4 + 8 * i), self.u16(base + 6 + 8 * i), self.u32(base + 8 + 8 * i)
            sub = base + offset
            if platform == 3 and encoding == 1 and self.u16(sub) == 4:
                segs = self.u16(sub + 6) // 2
                ends, starts = sub + 14, sub + 16 + 2 * segs
                deltas, ranges = starts + 2 * segs, starts + 4 * segs
                out = {}
                for s in range(segs):
                    end, start = self.u16(ends + 2 * s), self.u16(starts + 2 * s)
                    delta, rofs = self.s16(deltas + 2 * s), self.u16(ranges + 2 * s)
                    for c in range(start, min(end, 0xFFFE) + 1):
                        if rofs == 0:
                            g = (c + delta) & 0xFFFF
                        else:
                            g = self.u16(ranges + 2 * s + rofs + 2 * (c - start))
                            if g: g = (g + delta) & 0xFFFF
                        if g: out[c] = g
                return out
        raise ValueError('no format 4 cmap')

    def coverage(self, o):
        if self.u16(o) == 1:
            return {self.u16(o + 4 + 2 * i): i for i in range(self.u16(o + 2))}
        out = {}
        for i in range(self.u16(o + 2)):
            start, end, index = self.u16(o + 4 + 6 * i), self.u16(o + 6 + 6 * i), self.u16(o + 8 + 6 * i)
            for g in range(start, end + 1): out[g] = index + g - start
        return out

    def class_def(self, o):
        out = {}
        if self.u16(o) == 1:
            start = self.u16(o + 2)
            for i in range(self.u16(o + 4)): out[start + i] = self.u16(o + 6 + 2 * i)
        else:
            for i in range(self.u16(o + 2)):
                start, end, cls = self.u16(o + 4 + 6 * i), self.u16(o + 6 + 6 * i), self.u16(o + 8 + 6 * i)
                for g in range(start, end + 1): out[g] = cls
        return out

    @staticmethod
    def value_size(fmt): return 2 * bin(fmt & 0xFF).count('1')

    def x_advance(self, o, fmt):
        # ValueRecord fields in order XPlacement, YPlacement, XAdvance, ...: XAdvance is bit 0x0004
        if not fmt & 4: return 0
        return self.s16(o + 2 * bin(fmt & 3).count('1'))

    def pair_subtables(self):
        """PairPos subtables of the lookups of the 'kern' feature, in lookup order."""
        gpos = self.tables['GPOS']
        features, lookups = gpos + self.u16(gpos + 6), gpos + self.u16(gpos + 8)
        indices = []
        for i in range(self.u16(features)):
            if self.d[features + 2 + 6 * i:features + 6 + 6 * i] == b'kern':
                f = features + self.u16(features + 6 + 6 * i)
                indices += [self.u16(f + 4 + 2 * k) for k in range(self.u16(f + 2))]
        result = []
        for li in sorted(set(indices)):
            lookup = lookups + self.u16(lookups + 2 + 2 * li)
            ltype, subs = self.u16(lookup), self.u16(lookup + 4)
            for k in range(subs):
                sub = lookup + self.u16(lookup + 6 + 2 * k)
                if ltype == 9:   # extension
                    if self.u16(sub + 2) != 2: continue
                    sub = sub + self.u32(sub + 4)
                elif ltype != 2:
                    continue
                result.append((li, sub))
        return result

    def kerning(self, glyphs):
        """{(left glyph, right glyph): value in font units} for the given glyph ids."""
        wanted = set(glyphs)
        kern = {}
        done = set()   # (lookup, left, right) that a subtable already decided
        for li, sub in self.pair_subtables():
            fmt, cov = self.u16(sub), self.coverage(sub + self.u16(sub + 2))
            vf1, vf2 = self.u16(sub + 4), self.u16(sub + 6)
            rec2 = self.value_size(vf1) + self.value_size(vf2)
            if fmt == 1:
                for g1 in wanted & set(cov):
                    ps = sub + self.u16(sub + 10 + 2 * cov[g1])
                    for i in range(self.u16(ps)):
                        r = ps + 2 + i * (2 + rec2)
                        g2 = self.u16(r)
                        if g2 in wanted and (li, g1, g2) not in done:
                            done.add((li, g1, g2))
                            kern[(g1, g2)] = kern.get((g1, g2), 0) + self.x_advance(r + 2, vf1)
            elif fmt == 2:
                cd1, cd2 = self.class_def(sub + self.u16(sub + 8)), self.class_def(sub + self.u16(sub + 10))
                c1n, c2n = self.u16(sub + 12), self.u16(sub + 14)
                for g1 in wanted & set(cov):
                    c1 = cd1.get(g1, 0)
                    for g2 in wanted:
                        if (li, g1, g2) in done: continue
                        done.add((li, g1, g2))
                        c2 = cd2.get(g2, 0)
                        if c1 >= c1n or c2 >= c2n: continue
                        v = self.x_advance(sub + 16 + (c1 * c2n + c2) * rec2, vf1)
                        if v: kern[(g1, g2)] = kern.get((g1, g2), 0) + v
        return kern


def chars():
    out = [c for a, b in RANGES for c in range(a, b + 1)] + EXTRA
    return out


def build(name, file, size, fallback, ttf_cache):
    path = FONTS / file
    ttf = ttf_cache.setdefault(file, Ttf(path))
    font = ImageFont.truetype(str(path), size)
    codes = [c for c in chars() if c in ttf.cmap or c == 0x20]
    bitmap, glyph_dsc = [], ['    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,']
    for c in codes:
        ch = chr(c)
        adv = round(font.getlength(ch) * 16)
        x0, y0, x1, y1 = font.getbbox(ch, anchor='ls')
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        index = len(bitmap)
        if w and h and not ch.isspace():
            img = Image.new('L', (w, h), 0)
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor='ls')
            px = [round(v * 15 / 255) for v in img.tobytes()]
            if len(px) % 2: px.append(0)
            bitmap += [px[i] << 4 | px[i + 1] for i in range(0, len(px), 2)]
        else:
            w = h = 0; x0 = y1 = 0
        glyph_dsc.append('    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}, /* U+%04X */'
                         % (index, adv, w, h, x0, -y1, c))

    # kerning, rounded to 1/16 px; pairs below half a pixel are left out
    gid_of = {ttf.cmap[c]: i + 1 for i, c in enumerate(codes) if c in ttf.cmap}
    pairs = []
    for (g1, g2), v in ttf.kerning(list(gid_of)).items():
        px16 = round(v * size / ttf.units * 16)
        if abs(px16) >= 8:
            pairs.append((gid_of[g1], gid_of[g2], max(-128, min(127, px16))))
    pairs.sort()

    ascent, descent = font.getmetrics()
    cap = -font.getbbox('H', anchor='ls')[1]
    xh = -font.getbbox('x', anchor='ls')[1]

    # cmaps: the two Latin ranges (format 0) and the extras (sparse)
    cmaps, gid = [], 1
    for a, b in RANGES:
        n = len([c for c in codes if a <= c <= b])
        cmaps.append('    { .range_start = %d, .range_length = %d, .glyph_id_start = %d, .unicode_list = NULL, .glyph_id_ofs_list = NULL, '
                     '.list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY },' % (a, n, gid))
        gid += n
    extras = [c for c in codes if c in EXTRA]
    out = ['/* %s: generated by tools/gen_fonts.py from %s at %d px (SIL OFL 1.1, main/fonts/OFL.txt) */' % (name, file.replace('.ttf', '').replace('IBMPlexSans', 'IBM Plex Sans'), size)]
    out.append('static LV_ATTRIBUTE_LARGE_CONST const uint8_t %s_bitmap[] = {' % name)
    for i in range(0, len(bitmap), 24): out.append('    ' + ','.join('0x%02x' % b for b in bitmap[i:i + 24]) + ',')
    out.append('};')
    out.append('static const lv_font_fmt_txt_glyph_dsc_t %s_glyphs[] = {' % name)
    out += glyph_dsc
    out.append('};')
    out.append('static const uint16_t %s_extras[] = { %s };' % (name, ', '.join(str(c - extras[0]) for c in extras)))
    cmaps.append('    { .range_start = %d, .range_length = %d, .glyph_id_start = %d, .unicode_list = %s_extras, .glyph_id_ofs_list = NULL, '
                 '.list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY },' % (extras[0], extras[-1] - extras[0] + 1, gid, name, len(extras)))
    out.append('static const lv_font_fmt_txt_cmap_t %s_cmaps[] = {' % name)
    out += cmaps
    out.append('};')
    out.append('static const uint8_t %s_kern_ids[] = {' % name)
    for i in range(0, len(pairs), 12): out.append('    ' + ', '.join('%d, %d' % (a, b) for a, b, _ in pairs[i:i + 12]) + ',')
    out.append('};')
    out.append('static const int8_t %s_kern_values[] = {' % name)
    for i in range(0, len(pairs), 24): out.append('    ' + ', '.join(str(v) for _, _, v in pairs[i:i + 24]) + ',')
    out.append('};')
    out.append('static const lv_font_fmt_txt_kern_pair_t %s_kern = { .glyph_ids = %s_kern_ids, .values = %s_kern_values, '
               '.pair_cnt = %d, .glyph_ids_size = 0 };' % (name, name, name, len(pairs)))
    out.append('static const lv_font_fmt_txt_dsc_t %s_dsc = { .glyph_bitmap = %s_bitmap, .glyph_dsc = %s_glyphs, .cmaps = %s_cmaps, '
               '.kern_dsc = &%s_kern, .kern_scale = 16, .cmap_num = %d, .bpp = 4, .kern_classes = 0, .bitmap_format = 0 };'
               % (name, name, name, name, name, len(cmaps)))
    out.append('const lv_font_t %s = { .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt, .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt, '
               '.line_height = %d, .base_line = %d, .cap_height = %d, .x_height = %d, .subpx = LV_FONT_SUBPX_NONE, '
               '.underline_position = -2, .underline_thickness = 1, .static_bitmap = 1, .dsc = &%s_dsc, .fallback = &%s, .user_data = NULL };'
               % (name, ascent + descent, descent, cap, xh, name, fallback))
    assert len(codes) + 1 < 256, 'kern pairs use 8-bit glyph ids'
    return '\n'.join(out) + '\n', len(bitmap), len(pairs)


def main():
    cache = {}
    parts = ['// Generated by tools/gen_fonts.py - do not edit. Fonts converted from IBM Plex Sans (Copyright 2017 IBM Corp.,',
             '// SIL Open Font License 1.1, see main/fonts/OFL.txt). As a Modified Version they do not carry the reserved name.',
             '#include "ui_fonts.h"', '']
    header = ['// Board fonts converted from IBM Plex Sans by tools/gen_fonts.py (SIL OFL 1.1, main/fonts/OFL.txt).',
              '#pragma once', '', '#include "lvgl.h"', '']
    total = 0
    for name, file, size, fallback in SPECS:
        code, nbytes, npairs = build(name, file, size, fallback, cache)
        parts.append(code)
        header.append('extern const lv_font_t %s;' % name)
        total += nbytes
        print('%-18s %2d px  %6d bytes  %4d kern pairs' % (name, size, nbytes, npairs))
    (ROOT / 'main' / 'ui_fonts.c').write_text('\n'.join(parts))
    (ROOT / 'main' / 'ui_fonts.h').write_text('\n'.join(header) + '\n')
    print('bitmaps total %d KB' % (total // 1024))


if __name__ == '__main__':
    main()
