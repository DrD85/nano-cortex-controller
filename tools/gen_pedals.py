#!/usr/bin/env python3
"""Generates main/fx_pedals.c: small pedal pictures (in colour) for the header of the FX editor and line drawings
of the pedals for the FX tiles.

The pictures are the private ones of the Nano Cortex Editor (../nano-cortex-editor/img, matched to the models
with the editor's MODEL_IMAGES table). They show third-party pedals, so they go into main/private/fx_pedals_private.c
(own build only, ignored by git). main/fx_pedals.c, the published version, has no pictures.

The own build can also show the names of the modelled devices instead of the model names, and take further pictures:
main/private/fx_private.json (ignored by git as well) = {"names": {model name: shown name}, "images": {model name:
picture file}}. The published fx_pedals.c knows no such names.

Run from the project folder:  python3 tools/gen_pedals.py
"""
import json
import pathlib
import re

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps

ROOT = pathlib.Path(__file__).resolve().parent.parent
EDITOR = None   # set below
MODELS = json.loads((ROOT / 'tools' / 'editor_models.json').read_text())
BOX = (320, 330)   # working size (px)
THUMB = (84, 54)   # picture in the FX editor's header (in colour)
TILE_H, TILE_MAX_W, TILE_RADIUS, TILE_MARGIN = 118, 104, 12, 8   # tile drawing: height, widest, corner of the tile, gap to its edge


def find_editor():
    """The Nano Cortex Editor folder next to this project or further up (its img/ holds the private artwork)."""
    for folder in ROOT.parents:
        if (folder / 'nano-cortex-editor').is_dir():
            return folder / 'nano-cortex-editor'
    return ROOT.parent / 'nano-cortex-editor'


EDITOR = find_editor()


def model_images():
    html = (EDITOR / 'index.html').read_text()
    table = re.search(r'const MODEL_IMAGES = \{(.*?)\};', html, re.S)
    return dict(re.findall(r"'([^']+)':\s*'img/([^']+)'", table.group(1))) if table else {}


def picture(path):
    """The device in colour for the FX editor's header, small (RGBA)."""
    im = Image.open(path).convert('RGBA')
    box = im.getchannel('A').getbbox()
    if box:
        im = im.crop(box)
    im.thumbnail(THUMB, Image.LANCZOS)
    return im


def line_drawing(path):
    """Outline and details (knobs, lettering) of the pedal, cropped at the tile's right edge, as an A8 mask."""
    im = Image.open(path).convert('RGBA')
    im.thumbnail(BOX, Image.LANCZOS)
    alpha = im.getchannel('A')
    grey = ImageOps.grayscale(im.convert('RGB'))
    edges = grey.filter(ImageFilter.GaussianBlur(0.6)).filter(ImageFilter.FIND_EDGES)
    edges = ImageOps.autocontrast(edges, cutoff=2).point(lambda v: 255 if v > 70 else int(v * 255 / 70) if v > 25 else 0)
    outline = alpha.filter(ImageFilter.FIND_EDGES).point(lambda v: 255 if v > 60 else 0).filter(ImageFilter.MaxFilter(3))
    drawing = ImageChops.lighter(ImageChops.multiply(edges, alpha), outline)
    box = alpha.getbbox()
    if box:
        drawing = drawing.crop(box)
    drawing = drawing.resize((max(1, round(drawing.width * TILE_H / drawing.height)), TILE_H), Image.LANCZOS)
    if drawing.width <= TILE_MAX_W:
        # A pedal: all of it, a little away from the tile's edge
        whole = Image.new('L', (drawing.width + TILE_MARGIN, TILE_H), 0)
        whole.paste(drawing, (0, 0))
        return whole
    # A wide device (a rack unit): its left part, cut off by the tile's right edge with the tile's rounded corners
    # (the drawing's right edge is the tile's; 10 px of the tile above and below).
    drawing = drawing.crop((0, 0, TILE_MAX_W, TILE_H))
    mask = Image.new('L', drawing.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([-TILE_RADIUS - 2, -10, drawing.width - 1, TILE_H + 9], radius=TILE_RADIUS, fill=255)
    return ImageChops.multiply(drawing, mask)


def a8(out, ident, img):
    data = img.tobytes()
    out.append('static const uint8_t %s_map[] = {' % ident)
    for i in range(0, len(data), 32):
        out.append('    ' + ','.join('0x%02X' % b for b in data[i:i + 32]) + ',')
    out += ['};',
            'static const lv_image_dsc_t %s = {' % ident,
            '    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = %d, .h = %d, .stride = %d },'
            % (img.width, img.height, img.width),
            '    .data_size = sizeof(%s_map),' % ident,
            '    .data = %s_map,' % ident,
            '};', '']


def rgb565a8(out, ident, img):
    """A picture in colour with transparency (LV_COLOR_FORMAT_RGB565A8: the pixels, then their alpha values)."""
    rgb = img.convert('RGB').tobytes()
    data = bytearray()
    for i in range(0, len(rgb), 3):
        v = (rgb[i] >> 3) << 11 | (rgb[i + 1] >> 2) << 5 | rgb[i + 2] >> 3
        data += bytes((v & 0xFF, v >> 8))
    data += img.getchannel('A').tobytes()
    out.append('static const uint8_t %s_map[] = {' % ident)
    for i in range(0, len(data), 32):
        out.append('    ' + ','.join('0x%02X' % b for b in data[i:i + 32]) + ',')
    out += ['};',
            'static const lv_image_dsc_t %s = {' % ident,
            '    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .w = %d, .h = %d, .stride = %d },'
            % (img.width, img.height, img.width * 2),
            '    .data_size = sizeof(%s_map),' % ident,
            '    .data = %s_map,' % ident,
            '};', '']


def c_ident(name):
    return re.sub(r'[^0-9a-zA-Z]+', '_', name).strip('_').lower()


def private_extras():
    """Names and further pictures of the own build (main/private/fx_private.json), {} if there is none."""
    path = ROOT / 'main' / 'private' / 'fx_private.json'
    return json.loads(path.read_text()) if path.exists() else {}


def name_function(out, names):
    """nano_fx_real_name(): the shown name of a model in the own build, NULL = its model name."""
    out.append('static const struct { uint32_t type; const char *name; } REAL_NAMES[] = {')
    # A hyphen inside a name ("CE-2W") is written as U+2011, the non-breaking hyphen: LVGL wraps lines at '-'.
    out += ['    { %d, "%s" },   // %s' % (t, shown.replace('"', '').replace('-', '\\xE2\\x80\\x91" "'), name) for t, name, shown in names]
    out += ['    { 0, NULL },', '};', '',
            'const char *nano_fx_real_name(uint32_t type)',
            '{',
            '    for (size_t i = 0; type && i < sizeof(REAL_NAMES) / sizeof(REAL_NAMES[0]); i++) {',
            '        if (REAL_NAMES[i].type == type) return REAL_NAMES[i].name;',
            '    }',
            '    return NULL;',
            '}', '']


def lookup_functions(out):
    for func, field in (('nano_fx_pedal', 'image'), ('nano_fx_pedal_tile', 'tile')):
        out += ['const lv_image_dsc_t *%s(uint32_t type)' % func,
                '{',
                '    for (size_t i = 0; type && i < sizeof(PEDALS) / sizeof(PEDALS[0]); i++) {',
                '        if (PEDALS[i].type == type) return PEDALS[i].%s;' % field,
                '    }',
                '    return NULL;',
                '}', '']


def write_public():
    out = ['// Generated by tools/gen_pedals.py - do not edit.',
           '// Published version: no pedal pictures (the own build uses main/private/fx_pedals_private.c).',
           '#include "fx_pedals.h"', '',
           'static const struct { uint32_t type; const lv_image_dsc_t *image, *tile; } PEDALS[] = {',
           '    { 0, NULL, NULL },', '};', '']
    lookup_functions(out)
    name_function(out, [])
    (ROOT / 'main' / 'fx_pedals.c').write_text('\n'.join(out))


def main():
    write_public()
    images = model_images() if (EDITOR / 'img').is_dir() else {}
    extras = private_extras()
    images = {name: EDITOR / 'img' / file for name, file in images.items()}
    images.update({name: pathlib.Path(file) for name, file in extras.get('images', {}).items()})
    if not images:
        print('fx_pedals.c: no pictures (published version only)')
        return
    out = ['// Generated by tools/gen_pedals.py - do not edit.',
           '// PRIVATE - do not publish: third-party pedal pictures.',
           '#include "fx_pedals.h"', '']
    used = {}   # file -> identifier
    entries = []
    for model in MODELS['models']:
        type_, name = model[0], model[1]
        # Stereo / mono variants share the picture of the other one.
        file = images.get(name) or images.get(name.replace('(ST)', '(M)')) or images.get(name.replace('(M)', '(ST)'))
        if not file or not file.exists():
            continue
        if file not in used:
            ident = 'pedal_' + c_ident(file.stem)
            used[file] = ident
            rgb565a8(out, ident, picture(file))
            a8(out, ident + '_tile', line_drawing(file))
        entries.append((type_, name, used[file]))

    out.append('static const struct { uint32_t type; const lv_image_dsc_t *image, *tile; } PEDALS[] = {')
    out += ['    { %d, &%s, &%s_tile },   // %s' % (t, ident, ident, name) for t, name, ident in entries]
    out += ['};', '']
    lookup_functions(out)
    shown = extras.get('names', {})
    name_function(out, [(m[0], m[1], shown[m[1]]) for m in MODELS['models'] if m[1] in shown])
    (ROOT / 'main' / 'private').mkdir(exist_ok=True)
    (ROOT / 'main' / 'private' / 'fx_pedals_private.c').write_text('\n'.join(out))
    print('fx_pedals_private.c: %d models, %d pictures (+ published fx_pedals.c without pictures)' % (len(entries), len(used)))


if __name__ == '__main__':
    main()
