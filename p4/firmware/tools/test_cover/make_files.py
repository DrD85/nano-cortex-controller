# Test pictures and WAVs for tools/test_cover (PIL needed: claude-werkzeuge/bild-venv or any Python with Pillow).
import io
import struct
import sys

from PIL import Image

out = sys.argv[1]


def quarters(w, h, side=None):
    side = side or min(w, h)
    im = Image.new('RGB', (w, h), (128, 128, 128))
    x0, y0, s = (w - side) // 2, (h - side) // 2, side // 2
    im.paste((255, 0, 0), (x0, y0, x0 + s, y0 + s))
    im.paste((0, 255, 0), (x0 + s, y0, x0 + side, y0 + s))
    im.paste((0, 0, 255), (x0, y0 + s, x0 + s, y0 + side))
    im.paste((255, 255, 255), (x0 + s, y0 + s, x0 + side, y0 + side))
    return im


def jpeg(im, **kw):
    b = io.BytesIO()
    im.save(b, 'JPEG', quality=90, **kw)
    return b.getvalue()


quarters(1000, 1000).save(out + '/square.jpg', quality=90)
quarters(1200, 600).save(out + '/wide.jpg', quality=90)
quarters(50, 50).save(out + '/small.jpg', quality=95)
quarters(333, 333).save(out + '/odd.jpg', quality=95, subsampling=0)
quarters(600, 600).save(out + '/progressive.jpg', quality=90, progressive=True)
quarters(300, 300).save(out + '/cover.png')


def syncsafe(n):
    return bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])


def apic(ver, mime, ptype, desc, enc, data):
    body = bytes([enc]) + mime.encode() + b'\0' + bytes([ptype])
    body += (desc.encode('utf-16') + b'\0\0') if enc == 1 else (desc.encode('latin-1') + b'\0')
    body += data
    size = syncsafe(len(body)) if ver == 4 else struct.pack('>I', len(body))
    return b'APIC' + size + b'\0\0' + body


def tit2(ver, text):
    body = b'\3' + text.encode() + b'\0'
    size = syncsafe(len(body)) if ver == 4 else struct.pack('>I', len(body))
    return b'TIT2' + size + b'\0\0' + body


def id3(ver, frames):
    body = b''.join(frames) + b'\0' * 64   # padding
    return b'ID3' + bytes([ver, 0, 0]) + syncsafe(len(body)) + body


def wav(tag=None, tag_id=b'id3 '):
    fmt = struct.pack('<HHIIHH', 1, 2, 44100, 44100 * 4, 4, 16)
    data = b'\0' * (44100 * 4 // 10)   # 0.1 s
    chunks = b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    if tag is not None:
        chunks += tag_id + struct.pack('<I', len(tag)) + tag + (b'\0' if len(tag) & 1 else b'')
    return b'RIFF' + struct.pack('<I', 4 + len(chunks)) + b'WAVE' + chunks


other = jpeg(Image.new('RGB', (200, 200), (255, 255, 0)))   # a back cover (type 4), comes first
front = jpeg(quarters(800, 800))
open(out + '/tagged-v3.wav', 'wb').write(wav(id3(3, [tit2(3, 'Slow Blues'), apic(3, 'image/jpeg', 4, 'back', 0, other),
                                                    apic(3, 'image/jpeg', 3, 'Cover (front)', 1, front)])))
open(out + '/tagged-v4.wav', 'wb').write(wav(id3(4, [apic(4, 'image/jpg', 3, 'front', 0, front), tit2(4, 'x')]), b'ID3 '))
open(out + '/plain.wav', 'wb').write(wav())
png = io.BytesIO()
quarters(300, 300).save(png, 'PNG')
open(out + '/png-tag.wav', 'wb').write(wav(id3(3, [apic(3, 'image/png', 3, '', 0, png.getvalue())])))

# A jam folder with albums (p4_cover_load's order): beside the track, its tags, the album folder's picture, the jam
# folder's picture. One colour per place, so the test sees where a cover came from.
import os
import shutil

jam = out + '/jam'
shutil.rmtree(jam, ignore_errors=True)
for d in ('Blues', 'Bare', 'Prog', 'Ä' * 30):
    os.makedirs(jam + '/' + d)


def solid(path, rgb, **kw):
    Image.new('RGB', (200, 200), rgb).save(path, 'JPEG', quality=90, **kw)


solid(jam + '/cover.jpg', (0, 0, 255))              # the jam folder's: blue
solid(jam + '/Blues/folder.jpg', (0, 255, 0))       # the album's: green ...
solid(jam + '/Blues/artwork.jpeg', (255, 255, 0))   # ... before its artwork (yellow)
solid(jam + '/Blues/Own.jpg', (255, 0, 0))          # beside a track: red
solid(jam + '/Prog/cover.jpg', (255, 255, 0), progressive=True)   # not readable: front.jpg after it
solid(jam + '/Prog/front.jpg', (255, 0, 255))       # magenta
title = 'Ü' * 39                                    # the longest name: 60-byte album, 78-byte title
solid(jam + '/' + 'Ä' * 30 + '/' + title + '.jpg', (255, 0, 0))
for name in ('Loose', 'Blues/Slow', 'Blues/Own', 'Bare/Song', 'Prog/Song', 'Ä' * 30 + '/' + title):
    open(jam + '/' + name + '.wav', 'wb').write(wav())
open(jam + '/Blues/Tagged.wav', 'wb').write(wav(id3(3, [apic(3, 'image/jpeg', 3, '', 0, front)])))
print('files made')
