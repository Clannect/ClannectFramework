#!/usr/bin/env python3
"""Unhinted anti-aliased glyph masks from FreeType (the outside reference for cfw-text's GlyphCache).

    python3 glyph_masks.py out.json.z

For each test font, size and quarter-pixel offset: every glyph of a character sample, as FreeType renders it
without hinting: [glyph, size, subpixel, left, top (y down), width, height, coverage bytes as hex].
"""
import json
import os
import sys
import zlib

import freetype

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
FONTS = os.path.join(ROOT, 'modules', 'cfw-text', 'testdata', 'fonts')
SAMPLE = 'AaBgQR@&%éßЖыΩλبמ fijW,.|'
SIZES = [7, 11, 13.5, 16, 24, 41]


def masks(path):
    face = freetype.Face(path)
    out = []
    for size in SIZES:
        face.set_char_size(int(round(size * 64)))
        for sub in range(4):
            face.set_transform(freetype.Matrix(0x10000, 0, 0, 0x10000), freetype.Vector(sub * 16, 0))
            for ch in SAMPLE:
                gid = face.get_char_index(ord(ch))
                if not gid:
                    continue
                face.load_glyph(gid, freetype.FT_LOAD_NO_HINTING | freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_BITMAP)
                bm = face.glyph.bitmap
                rows = [bytes(bm.buffer[r * bm.pitch:r * bm.pitch + bm.width]) for r in range(bm.rows)]
                out.append([gid, size, sub / 4, face.glyph.bitmap_left, -face.glyph.bitmap_top, bm.width, bm.rows,
                            b''.join(rows).hex()])
    return out


def main():
    result = {name: masks(os.path.join(FONTS, name)) for name in ('DejaVuSans.ttf', 'CfwTestPlain.otf')}
    with open(sys.argv[1], 'wb') as f:
        f.write(zlib.compress(json.dumps(result, separators=(',', ':')).encode(), 9))


if __name__ == '__main__':
    main()
