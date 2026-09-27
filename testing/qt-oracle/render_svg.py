#!/usr/bin/env python3
"""Renders the SVG test icons with Qt's QSvgRenderer, as Clannect's editor renders its icons
(engine/src/editor/EditorIcons.cpp): an antialiased painter on a transparent canvas of size + 4, the image
drawn into (2, 2, size, size).

    python3 testing/qt-oracle/render_svg.py

Writes modules/cfw-gfx/testdata/golden-svg/<name>-<size>.png (RGBA, straight alpha) for sizes 16, 20, 24, 32.
"""
import os
import sys

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QRectF  # noqa: E402
from PySide6.QtGui import QGuiApplication, QImage, QPainter  # noqa: E402
from PySide6.QtSvg import QSvgRenderer  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'modules', 'cfw-gfx', 'testdata', 'svg')
OUT = os.path.join(ROOT, 'modules', 'cfw-gfx', 'testdata', 'golden-svg')
SIZES = [16, 20, 24, 32]


def main():
    app = QGuiApplication(sys.argv)  # noqa: F841
    os.makedirs(OUT, exist_ok=True)
    for name in sorted(os.listdir(SRC)):
        renderer = QSvgRenderer(os.path.join(SRC, name))
        assert renderer.isValid(), name
        for size in SIZES:
            canvas = size + 4
            image = QImage(canvas, canvas, QImage.Format_ARGB32_Premultiplied)
            image.fill(0)
            p = QPainter(image)
            p.setRenderHint(QPainter.Antialiasing, True)
            p.setRenderHint(QPainter.SmoothPixmapTransform, True)
            renderer.render(p, QRectF(2, 2, size, size))
            p.end()
            image.convertToFormat(QImage.Format_RGBA8888).save(os.path.join(OUT, f'{name[:-4]}-{size}.png'))


if __name__ == '__main__':
    main()
