# Qt oracle

Qt is not part of Clannect Framework, and nothing here is built or shipped. Qt is used only as an outside
reference: renderings are made once with Qt, then committed as expected results (decisions 0007 and 0014).

`render_scenes.py` renders the cfw-gfx golden scenes (`modules/cfw-gfx/testdata/scenes/*.json`) with Qt's
QPainter and writes `modules/cfw-gfx/testdata/golden/<scene>.png`. It needs Python 3, PySide6 6.11 and NumPy:

```
pip install PySide6==6.11.* numpy
QT_QPA_PLATFORM=offscreen python3 testing/qt-oracle/render_scenes.py [scene ...]
```

Re-run it only when a scene changes, and review the new PNGs before committing them.
