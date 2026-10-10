#!/usr/bin/env python3
"""Numbered screenshots for the guide in the README: docs/images/guide-*.png from the rendered screenshots.
Run by make_screenshots.sh (needs Pillow)."""
import pathlib

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
IMAGES = ROOT / 'docs' / 'images'
FONT = ImageFont.truetype(str(ROOT / 'main' / 'fonts' / 'IBMPlexSans-SemiBold.ttf'), 19)
RADIUS = 15

# picture -> (source, [(number, x, y), ...]): where each number of the README's list sits
GUIDES = {
    'guide-main': ('01-preset-mode', [(1, 270, 26), (2, 511, 26), (3, 592, 26), (4, 94, 85), (5, 606, 88), (6, 400, 152),
                                      (7, 400, 328)]),
    'guide-fx-editor': ('04-fx-editor', [(1, 56, 56), (2, 330, 38), (3, 680, 38), (4, 430, 104), (5, 702, 104), (6, 396, 208)]),
}


def badge(draw, number, x, y):
    draw.ellipse([x - RADIUS - 3, y - RADIUS - 3, x + RADIUS + 3, y + RADIUS + 3], fill=(0, 0, 0))
    draw.ellipse([x - RADIUS, y - RADIUS, x + RADIUS, y + RADIUS], fill=(255, 255, 255))
    draw.text((x, y - 1), str(number), font=FONT, fill=(13, 15, 17), anchor='mm')


def main():
    for name, (source, marks) in GUIDES.items():
        img = Image.open(IMAGES / (source + '.png')).convert('RGB')
        draw = ImageDraw.Draw(img)
        for number, x, y in marks:
            badge(draw, number, x, y)
        img.save(IMAGES / (name + '.png'))
        print('docs/images/%s.png' % name)


if __name__ == '__main__':
    main()
