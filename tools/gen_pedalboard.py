#!/usr/bin/env python3
"""Pictures for the board's pedal view: one file with a picture per FX model, in colour, at one scale.

The pedal view shows the five FX slots as pedals. Its pictures do not go into the firmware: they are written to a part
of the flash memory nothing else uses (PEDALBOARD_OFFSET, see main/pedal_assets.c) and drawn from there as they are -
no decoding, no RAM, no longer start. Without the file every pedal is drawn (colour, symbol, name); the published
firmware has no file.

The pictures are private ones (third-party pedals): they are looked for
  0. in the Nano Cortex Editor's img/ (its MODEL_IMAGES table) first, so board and editor show the same pedal; a picture
     from Pedal Playground's set (img/pp/<file>) gets the pedal's real size from that set's pedals.json
  1. in main/private/fx_private.json, "board": {model name: {"pp": file in Pedal Playground's set | "file": path}}
  2. in Pedal Playground's set of the QC editor project (cortex-control-ipad/private/pedal-images + pedals.json, with
     the pedals' real sizes), by the name of the modelled device in that project's device list - the same search as
     the QC editor's (mac-app/app/js/board.js, matchPhoto); "photos": {model id: file} for those it does not find
Writes build/pedalboard.bin and build/pedalboard.png (all pictures, to look through).

Run from the project folder:  python3 tools/gen_pedalboard.py
Flash:  esptool --port <port> write-flash 0x700000 build/pedalboard.bin
"""
import json
import pathlib
import re
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
MODELS = json.loads((ROOT / 'tools' / 'editor_models.json').read_text())
PX_PER_INCH = 50            # a 2.9 x 5.1 inch pedal is 145 x 255 px
MAX_W, MAX_H = 150, 318     # a slot of the pedal view (wider or taller pictures are scaled down on their own)
NOMINAL_H = 4.9             # inches, for pictures without a known size
MANUAL_PHOTOS = {}          # model id -> picture of that set, from fx_private.json ("photos"), as the QC editor's table
SIZES = {}                  # Pedal Playground picture file -> (width, height) in inches


def sibling(name):
    for folder in ROOT.parents:
        if (folder / name).is_dir():
            return folder / name
    return ROOT.parent / name


EDITOR, QC = sibling('nano-cortex-editor'), sibling('cortex-control-ipad') / 'private'
compact = lambda t: re.sub(r'[^a-z0-9]+', '', re.sub(r'[®™]', '', str(t or '').lower()))
words = lambda t: [w for w in re.split(r'[^a-z0-9]+', re.sub(r"[®™’']", '', str(t or '').lower())) if w]


def load_catalog():
    path = QC / 'pedals.json'
    if not path.exists():
        return []
    have = {f.name for f in (QC / 'pedal-images').iterdir()} if (QC / 'pedal-images').is_dir() else set()
    return [dict(p, b=compact(p['Brand']), n=compact(p['Name'])) for p in json.loads(path.read_text()) if p['Image'] in have]


def match_photo(catalog, text):
    """The QC editor's search: the catalog's brand, then the longest part of the rest that a pedal name contains."""
    if not text:
        return None
    brands = {p['b'] for p in catalog}
    by_brand = re.match(r'^(?:based on )?(?:the )?(.+?) by (.+?)(?: \(.*\))?$', str(text).strip(), re.I)
    every = words(by_brand.group(2) + ' ' + by_brand.group(1) if by_brand else re.sub(r'^based on (the )?', '', str(text), flags=re.I))
    for start in range(len(every) - 1):
        w = every[start:]
        for n in range(min(4, len(w) - 1), 0, -1):
            brand = ''.join(w[:n])
            if brand not in brands:
                continue
            rest, pool = w[n:], [p for p in catalog if p['b'] == brand]
            for k in range(len(rest), 0, -1):
                key = ''.join(rest[:k])
                if len(key) < 3:
                    continue
                hits = [p for p in pool if key in p['n'] or (len(p['n']) >= 4 and p['n'] in key)]
                if hits:
                    return sorted(hits, key=lambda p: len(p['n']))[0]
    return None


def pp_sizes():
    """Pedal Playground's real sizes (inches) by picture file - for the whole set, also pictures only the editor has"""
    path = QC / 'pedals.json'
    return {p['Image']: (p['Width'], p['Height']) for p in json.loads(path.read_text())} if path.exists() else {}


def editor_images():
    html = EDITOR / 'index.html'
    if not html.exists():
        return {}
    table = re.search(r'const MODEL_IMAGES = \{(.*?)\};', html.read_text(), re.S)
    return dict(re.findall(r"'([^']+)':\s*'img/([^']+)'", table.group(1))) if table else {}


def choose(catalog, devices, private, images):
    """[(type, model name, picture path, (width, height) in inches or None, where it came from)]"""
    by_image = {p['Image']: p for p in catalog}
    by_qc = {compact(v['qc']): v for v in devices.values()}
    board = private.get('board', {})
    out = []
    for model in MODELS['models']:
        type_, name = model[0], model[1]
        other = name.replace('(ST)', '(M)') if '(ST)' in name else name.replace('(M)', '(ST)')
        pick = board.get(name) or board.get(other)
        entry, path, source = None, None, None
        file = images.get(name) or images.get(other)
        if file and (EDITOR / 'img' / file).exists():
            size = SIZES.get(pathlib.Path(file).name) if file.startswith('pp/') else None
            out.append((type_, name, EDITOR / 'img' / file, size, 'Nano editor: ' + pathlib.Path(file).name))
            continue
        if pick and pick.get('pp') in by_image:
            entry, source = by_image[pick['pp']], 'chosen'
        elif pick and pick.get('file') and pathlib.Path(pick['file']).exists():
            path, source = pathlib.Path(pick['file']), 'chosen'
        if not entry and not path and MANUAL_PHOTOS.get(type_) in by_image:
            entry, source = by_image[MANUAL_PHOTOS[type_]], 'QC editor'
        if not entry and not path:
            device = by_qc.get(compact(name)) or by_qc.get(compact(re.sub(r'\s*\((M|ST)\)', '', name)))
            entry = match_photo(catalog, device['original']) if device else None
            source = 'device list' if entry else None
        if entry:
            out.append((type_, name, QC / 'pedal-images' / entry['Image'], (entry['Width'], entry['Height']), source + ': ' + entry['Brand'] + ' ' + entry['Name']))
        elif path:
            out.append((type_, name, path, None, source + ': ' + path.name))
        else:
            out.append((type_, name, None, None, 'none - drawn on the board'))
    return out


def picture(path, inches):
    im = Image.open(path).convert('RGBA')
    box = im.getchannel('A').getbbox()
    if box:
        im = im.crop(box)
    if inches:
        w, h = inches[0] * PX_PER_INCH, inches[1] * PX_PER_INCH
    else:   # no size known: as tall as a usual pedal, a wide device (a rack unit) as wide as the slot
        h = NOMINAL_H * PX_PER_INCH
        w = h * im.width / im.height
    scale = min(1, MAX_W / w, MAX_H / h)
    return im.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS)


def rgb565a8(img):
    rgb = img.convert('RGB').tobytes()
    data = bytearray()
    for i in range(0, len(rgb), 3):
        v = (rgb[i] >> 3) << 11 | (rgb[i + 1] >> 2) << 5 | rgb[i + 2] >> 3
        data += bytes((v & 0xFF, v >> 8))
    return bytes(data) + img.getchannel('A').tobytes()


def main():
    private = ROOT / 'main' / 'private' / 'fx_private.json'
    private = json.loads(private.read_text()) if private.exists() else {}
    devices = QC / 'devicelist' / 'devices.json'
    devices = json.loads(devices.read_text()) if devices.exists() else {}
    MANUAL_PHOTOS.update({int(k): v for k, v in private.get('photos', {}).items()})
    SIZES.update(pp_sizes())
    chosen = choose(load_catalog(), devices, private, editor_images())
    pictures = [(t, name, picture(path, inches), source) for t, name, path, inches, source in chosen if path]
    if not pictures:
        print('No pictures found - nothing written (the board draws every pedal)')
        return
    # File: "NPB1", count, version, size of the file; entries {type, offset, width, height}; the pictures (RGB565A8)
    head = 12 + 12 * len(pictures)
    blobs, entries, offset = [], [], head
    for t, name, img, source in pictures:
        blob = rgb565a8(img)
        entries.append(struct.pack('<IIHH', t, offset, img.width, img.height))
        blobs.append(blob)
        offset += len(blob)
    data = b'NPB1' + struct.pack('<HHI', len(pictures), 1, offset) + b''.join(entries) + b''.join(blobs)
    (ROOT / 'build').mkdir(exist_ok=True)
    (ROOT / 'build' / 'pedalboard.bin').write_bytes(data)
    # All of them on one sheet
    cols, cell_w, cell_h = 9, 168, 372
    sheet = Image.new('RGB', (cols * cell_w, -(-len(chosen) // cols) * cell_h), (18, 20, 22))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype(str(ROOT / 'main' / 'fonts' / 'IBMPlexSans-Medium.ttf'), 13)
    except OSError:
        font = ImageFont.load_default()
    done = {t: img for t, name, img, source in pictures}
    for i, (t, name, path, inches, source) in enumerate(chosen):
        x, y = (i % cols) * cell_w, (i // cols) * cell_h
        if t in done:
            img = done[t]
            sheet.paste(img, (x + (cell_w - img.width) // 2, y + 326 - img.height), img)
        draw.text((x + 8, y + 334), name, font=font, fill=(242, 242, 242))
        draw.text((x + 8, y + 351), source.split(': ', 1)[-1][:24], font=font, fill=(150, 154, 160) if t in done else (255, 176, 46))
    sheet.save(ROOT / 'build' / 'pedalboard.png')
    for t, name, path, inches, source in chosen:
        print('%6d  %-24s %s' % (t, name, source))
    print('build/pedalboard.bin: %d pictures of %d models, %.1f MB' % (len(pictures), len(chosen), len(data) / 1e6))


if __name__ == '__main__':
    main()
