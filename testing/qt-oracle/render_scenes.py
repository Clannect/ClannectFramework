#!/usr/bin/env python3
"""Renders the cfw-gfx golden scenes with Qt's QPainter (the outside oracle).

The scene format is documented in modules/cfw-gfx/testdata/scenes/README.md; PainterGoldenTest.cpp is
the CFW side of the same interpreter. Output: straight-alpha RGBA PNGs in modules/cfw-gfx/testdata/golden.
"""
import json
import os
import sys

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import (QBrush, QColor, QGradient, QGuiApplication, QImage, QLinearGradient, QPainter,
                           QPainterPath, QPen, QPixmap, QPolygonF, QRadialGradient, QTransform)

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
SCENES = os.path.join(ROOT, 'modules', 'cfw-gfx', 'testdata', 'scenes')
GOLDEN = os.path.join(ROOT, 'modules', 'cfw-gfx', 'testdata', 'golden')


def test_image(name):
    """Straight RGBA test images; PainterGoldenTest.cpp generates the same ones."""
    if name == 'photo':
        w, h = 32, 24
        px = [((x * 8) % 256, (y * 10) % 256, (x * y) % 256, 255) for y in range(h) for x in range(w)]
    elif name == 'alpha':
        w, h = 16, 16
        px = [(255, 128, x * 16, y * 16 + 15) for y in range(h) for x in range(w)]
    elif name == 'checker':
        w, h = 4, 4
        px = [(0, 0, 255, 255) if (x + y) % 2 else (255, 255, 0, 128) for y in range(h) for x in range(w)]
    else:
        raise ValueError(name)
    data = bytes(v for p in px for v in p)
    return QImage(data, w, h, w * 4, QImage.Format_RGBA8888).copy()


def color(c):
    return QColor(c[0], c[1], c[2], c[3])


def path(cmds, rule='nonzero'):
    p = QPainterPath()
    for c in cmds:
        k, a = c[0], c[1:]
        if k == 'M':
            p.moveTo(a[0], a[1])
        elif k == 'L':
            p.lineTo(a[0], a[1])
        elif k == 'Q':
            p.quadTo(a[0], a[1], a[2], a[3])
        elif k == 'C':
            p.cubicTo(a[0], a[1], a[2], a[3], a[4], a[5])
        elif k == 'Z':
            p.closeSubpath()
        elif k == 'rect':
            p.addRect(QRectF(*a))
        elif k == 'rrect':
            p.addRoundedRect(QRectF(a[0], a[1], a[2], a[3]), a[4], a[5])
        elif k == 'ellipse':
            p.addEllipse(QRectF(*a))
        elif k == 'arc':
            p.arcTo(QRectF(a[0], a[1], a[2], a[3]), a[4], a[5])
        elif k == 'poly':
            p.addPolygon(QPolygonF([QPointF(a[i], a[i + 1]) for i in range(0, len(a), 2)]))
            p.closeSubpath()
        else:
            raise ValueError(k)
    p.setFillRule(Qt.OddEvenFill if rule == 'evenodd' else Qt.WindingFill)
    return p


SPREAD = {'pad': QGradient.PadSpread, 'repeat': QGradient.RepeatSpread, 'reflect': QGradient.ReflectSpread}


def brush(b):
    if 'color' in b:
        return QBrush(color(b['color']))
    if 'linear' in b:
        g = QLinearGradient(*b['linear'])
    else:
        cx, cy, r = b['radial']
        f = b.get('focal', [cx, cy])
        g = QRadialGradient(QPointF(cx, cy), r, QPointF(f[0], f[1]))
    for s in b['stops']:
        g.setColorAt(s[0], color(s[1:]))
    g.setSpread(SPREAD[b.get('spread', 'pad')])
    return QBrush(g)


CAP = {'flat': Qt.FlatCap, 'square': Qt.SquareCap, 'round': Qt.RoundCap}
JOIN = {'bevel': Qt.BevelJoin, 'miter': Qt.MiterJoin, 'svgmiter': Qt.SvgMiterJoin, 'round': Qt.RoundJoin}
BLEND = {
    'source-over': QPainter.CompositionMode_SourceOver, 'source': QPainter.CompositionMode_Source,
    'destination-in': QPainter.CompositionMode_DestinationIn,
    'destination-out': QPainter.CompositionMode_DestinationOut,
    'multiply': QPainter.CompositionMode_Multiply, 'screen': QPainter.CompositionMode_Screen,
    'plus': QPainter.CompositionMode_Plus,
}


def pen(p):
    q = QPen(brush(p['brush']) if 'brush' in p else QBrush(color(p.get('color', [0, 0, 0, 255]))), p.get('width', 1))
    q.setCapStyle(CAP[p.get('cap', 'square')])
    q.setJoinStyle(JOIN[p.get('join', 'bevel')])
    q.setMiterLimit(p.get('miter', 2))
    if 'dashes' in p:
        q.setDashPattern(p['dashes'])
        q.setDashOffset(p.get('offset', 0))
    q.setCosmetic(p.get('cosmetic', False))
    return q


def tinted(img, tint):
    """Multiplies every premultiplied channel by the premultiplied tint, as ImageOptions::tint does."""
    pre = img.convertToFormat(QImage.Format_RGBA8888_Premultiplied)
    a = np.frombuffer(pre.constBits(), dtype=np.uint8).reshape(pre.height(), pre.width(), 4).astype(np.uint32)
    t = np.array([tint[0] * tint[3] / 255, tint[1] * tint[3] / 255, tint[2] * tint[3] / 255, tint[3]])
    t = np.round(t).astype(np.uint32)
    out = ((a * t + 128) + ((a * t + 128) >> 8)) >> 8
    out = np.ascontiguousarray(out.astype(np.uint8))
    return QImage(out.tobytes(), pre.width(), pre.height(), pre.width() * 4,
                  QImage.Format_RGBA8888_Premultiplied).copy()


def render(scene):
    w, h = scene['size']
    img = QImage(w, h, QImage.Format_RGBA8888_Premultiplied)
    img.fill(color(scene.get('background', [0, 0, 0, 0])))
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing)
    p.setRenderHint(QPainter.SmoothPixmapTransform)
    for op in scene['ops']:
        k = op['op']
        if k == 'save':
            p.save()
        elif k == 'restore':
            p.restore()
        elif k == 'translate':
            p.translate(*op['d'])
        elif k == 'scale':
            p.scale(*op['s'])
        elif k == 'rotate':
            p.rotate(op['deg'])
        elif k == 'transform':
            m = op['m']
            p.setTransform(QTransform(m[0], m[3], m[1], m[4], m[2], m[5]), True)
        elif k == 'quad':
            q = QTransform()
            ok = QTransform.quadToQuad(QPolygonF([QPointF(*v) for v in op['from']]),
                                       QPolygonF([QPointF(*v) for v in op['to']]), q)
            assert ok
            p.setTransform(q, True)
        elif k == 'opacity':
            p.setOpacity(op['v'])
        elif k == 'blend':
            p.setCompositionMode(BLEND[op['mode']])
        elif k == 'clipRect':
            p.setClipRect(QRectF(*op['r']), Qt.IntersectClip)
        elif k == 'clipPath':
            p.setClipPath(path(op['path'], op.get('rule', 'nonzero')), Qt.IntersectClip)
        elif k == 'fill':
            p.fillPath(path(op['path'], op.get('rule', 'nonzero')), brush(op['brush']))
        elif k == 'fillRect':
            p.fillRect(QRectF(*op['r']), brush(op['brush']))
        elif k == 'stroke':
            p.strokePath(path(op['path']), pen(op['pen']))
        elif k == 'image':
            src = test_image(op['image'])
            if 'tint' in op:
                src = tinted(src, op['tint'])
            p.setRenderHint(QPainter.SmoothPixmapTransform, op.get('smooth', True))
            target = QRectF(*op['target'])
            if op.get('tile'):
                p.drawTiledPixmap(target, QPixmap.fromImage(src))
            else:
                s = op.get('source', [0, 0, src.width(), src.height()])
                p.drawImage(target, src, QRectF(*s))
            p.setRenderHint(QPainter.SmoothPixmapTransform, True)
        else:
            raise ValueError(k)
    p.end()
    # Premultiplied -> straight with rounding, for the PNG.
    a = np.frombuffer(img.constBits(), dtype=np.uint8).reshape(h, w, 4).astype(np.uint32)
    alpha = a[..., 3:4]
    straight = np.where(alpha > 0, (a[..., :3] * 255 + alpha // 2) // np.maximum(alpha, 1), 0)
    out = np.concatenate([np.minimum(straight, 255), alpha], axis=2).astype(np.uint8)
    return QImage(np.ascontiguousarray(out).tobytes(), w, h, w * 4, QImage.Format_RGBA8888).copy()


def main():
    app = QGuiApplication(sys.argv[:1])  # noqa: F841 (QPixmap needs an application)
    names = sys.argv[1:] or sorted(f[:-5] for f in os.listdir(SCENES) if f.endswith('.json'))
    os.makedirs(GOLDEN, exist_ok=True)
    for name in names:
        with open(os.path.join(SCENES, name + '.json')) as f:
            scene = json.load(f)
        out = os.path.join(GOLDEN, name + '.png')
        assert render(scene).save(out)
        print('wrote', os.path.relpath(out, ROOT))


if __name__ == '__main__':
    main()
