#!/usr/bin/env python3
"""Canonical glyph outlines from fontTools (the outside reference for cfw-text's FontFace).

    python3 outlines.py font.ttf [face-index]   ->  one line per glyph:  gid advance lsb canonical-outline

The canonical form (shared with FontFaceTest.cpp): each contour as its segments, L x y / Q cx cy x y /
C c1x c1y c2x c2y x y, coordinates in 1/64 font units, starting after the segment that ends at the
smallest point; a closing line only if the contour does not end where it started.
"""
import sys
from fontTools.pens.basePen import BasePen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont


def q(v):
    return int(round(v * 64))


class Canonical(BasePen):
    def __init__(self, glyphset):
        super().__init__(glyphset)
        self.contours = []
        self.cur = None

    def _moveTo(self, p):
        self.cur = [q(p[0]), q(p[1])]
        self.contours.append({'start': tuple(self.cur), 'segs': []})

    def _lineTo(self, p):
        self.cur = [q(p[0]), q(p[1])]
        self.contours[-1]['segs'].append(('L', q(p[0]), q(p[1])))

    def _qCurveToOne(self, p1, p2):
        self.contours[-1]['segs'].append(('Q', q(p1[0]), q(p1[1]), q(p2[0]), q(p2[1])))

    def _curveToOne(self, p1, p2, p3):
        self.contours[-1]['segs'].append(('C', q(p1[0]), q(p1[1]), q(p2[0]), q(p2[1]), q(p3[0]), q(p3[1])))

    def _closePath(self):
        c = self.contours[-1]
        segs = c['segs']
        end = segs[-1][-2:] if segs else c['start']
        if tuple(end) != c['start']:
            segs.append(('L',) + c['start'])

    _endPath = _closePath


def canonical(contours):
    out = []
    for c in contours:
        segs = c['segs']
        if not segs:
            continue
        ends = [s[-2:] for s in segs]
        k = min(range(len(segs)), key=lambda i: (ends[i][0], ends[i][1], i))
        rot = segs[k + 1:] + segs[:k + 1]
        out.append(' '.join(' '.join(str(x) for x in s) for s in rot))
    return ' | '.join(out)


def freetype_contours(path, index, gid):
    """The same canonical contours from FreeType's points (unscaled, unhinted): for glyphs fontTools
    cannot interpret (Type 2 arithmetic, point-matched components). Implied on-curve points are
    computed exactly here (FreeType's own decomposition rounds them to whole units)."""
    import freetype
    face = freetype.Face(path, index)
    face.load_glyph(gid, freetype.FT_LOAD_NO_SCALE | freetype.FT_LOAD_NO_HINTING)
    outline = face.glyph.outline
    pen = Canonical(None)
    start = 0
    for end in outline.contours:
        pts = outline.points[start:end + 1]
        tags = outline.tags[start:end + 1]
        start = end + 1
        m = len(pts)
        on = [t & 1 for t in tags]
        cubic = [bool(t & 2) for t in tags]
        k0 = next((k for k in range(m) if on[k]), None)
        if k0 is None:  # all off-curve (quadratic): start at a midpoint
            first = ((pts[0][0] + pts[-1][0]) / 2, (pts[0][1] + pts[-1][1]) / 2)
            order = list(range(m))
        else:
            first = pts[k0]
            order = [(k0 + 1 + j) % m for j in range(m - 1)]
        pen._moveTo(first)
        pending = []
        for k in order:
            if on[k]:
                if not pending:
                    pen._lineTo(pts[k])
                elif cubic[pending[0]]:
                    pen._curveToOne(pts[pending[0]], pts[pending[1]], pts[k])
                else:
                    pen._qCurveToOne(pts[pending[0]], pts[k])
                pending = []
            elif cubic[k]:
                pending.append(k)
            else:
                if pending:
                    a = pts[pending[0]]
                    mid = ((a[0] + pts[k][0]) / 2, (a[1] + pts[k][1]) / 2)
                    pen._qCurveToOne(a, mid)
                pending = [k]
        if pending:
            if cubic[pending[0]]:
                pen._curveToOne(pts[pending[0]], pts[pending[1]], first)
            else:
                pen._qCurveToOne(pts[pending[0]], first)
        pen._closePath()
    return pen.contours


def main():
    path = sys.argv[1]
    index = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    font = TTFont(path, fontNumber=index) if path.lower().endswith(('.ttc', '.otc')) else TTFont(path)
    gs = font.getGlyphSet()
    order = font.getGlyphOrder()
    hmtx = font['hmtx']
    glyf = font['glyf'] if 'glyf' in font else None
    for gid, name in enumerate(order):
        pen = Canonical(gs)
        adv, lsb = hmtx[name]
        target = pen
        # FreeType places every TrueType glyph so its left edge is at the hmtx left side bearing (the
        # phantom points); fontTools does that for simple glyphs only, so composites are shifted here.
        if glyf is not None and glyf[name].isComposite() and hasattr(glyf[name], 'xMin'):
            target = TransformPen(pen, (1, 0, 0, 1, lsb - glyf[name].xMin, 0))
        try:
            gs[name].draw(target)
            contours = pen.contours
        except (NotImplementedError, AttributeError):
            # fontTools cannot run Type 2 arithmetic or draw point-matched components.
            pen = Canonical(gs)
            contours = freetype_contours(path, index, gid)
        print(gid, adv, lsb, canonical(contours))


if __name__ == '__main__':
    main()
