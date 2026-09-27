#!/usr/bin/env python3
"""Builds cfw-text's test fonts from DejaVu Sans (whose licence allows modified copies) and writes the
expected values FontFaceTest compares against, using fontTools as the outside reference.

    python3 testing/text-oracle/make_test_fonts.py path/to/DejaVuSans.ttf

Writes to modules/cfw-text/testdata/fonts/:
    DejaVuSans.ttf              the original, unmodified
    CfwTestCff.otf              name-keyed CFF: subset outlines as cubics in subroutines, plus glyphs
                                hand-written to use every Type 2 operator
    CfwTestCid.otf              the same outlines CID-keyed, two font DICTs, FDSelect format 3
    CfwTestComposite.ttf        TrueType composites: offsets, scales, 2x2, point matching, nesting
    CfwTestCollection.ttc       a collection of two faces
    expected.json               per font: metrics, names, a cmap hash, per-64-glyph outline hashes
"""
import hashlib
import io
import json
import os
import shutil
import sys

from fontTools import subset
from fontTools.cffLib import SubrsIndex, GlobalSubrsIndex
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont, TTCollection
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent, GlyphCoordinates

sys.path.insert(0, os.path.dirname(__file__))
import outlines  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'modules', 'cfw-text', 'testdata', 'fonts')
LATIN = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,;:!?()-éàüñçÅØßæœ'


def subset_font(source, text, keep_layout=True):
    font = TTFont(source)
    options = subset.Options()
    options.layout_features = ['*'] if keep_layout else []
    options.name_IDs = ['*']
    options.notdef_outline = True
    options.recalc_bounds = True
    s = subset.Subsetter(options)
    s.populate(text=text)
    s.subset(font)
    return font


def rename(font, family, style='Regular'):
    for rec in font['name'].names:
        if rec.nameID in (1, 16):
            rec.string = family
        elif rec.nameID in (4,):
            rec.string = f'{family} {style}'
        elif rec.nameID == 6:
            rec.string = family.replace(' ', '') + '-' + style


def hand_written_charstrings(private, global_subrs):
    """Glyphs that exercise every Type 2 operator; the outlines come out of fontTools' interpreter."""
    local = private.Subrs
    # Local subr 0: a line and a return; global subr 0: calls local subr 0 (nesting), then a curve.
    local.append(T2CharString(program=[0, 100, 'rlineto', 'return'], private=private, globalSubrs=global_subrs))
    global_subrs.append(T2CharString(program=[-107, 'callsubr', 50, 0, 50, 50, 0, 50, 'rrcurveto', 'return'],
                                     private=private, globalSubrs=global_subrs))
    programs = {
        # Hints before the path (width first), a hint mask, lines of every kind.
        'ops.hints': [500, 10, 20, 30, 40, 'hstemhm', 50, 60, 'vstemhm', 'hintmask', bytes([0b11100000]),
                      100, 100, 'rmoveto', 200, 'hlineto', 150, 'vlineto', -50, 50, -150, 'hlineto', 'cntrmask',
                      bytes([0b10000000]), -100, -200, 'rlineto', 'endchar'],
        # Curves: rrcurveto, hhcurveto (odd), vvcurveto (odd), hvcurveto and vhcurveto with a final extra.
        'ops.curves': [100, 'hmoveto', 50, 0, 100, 50, 100, 100, 'rrcurveto', 10, 50, 40, 20, 50,
                       40, 30, 10, 30, 'hhcurveto', 5, 50, 20, 20, 40, 'vvcurveto', 40, 30, 20, 50, 60, 40, 30,
                       20, 7, 'hvcurveto', -60, -40, -30, -50, -40, -40, -20, -60, 'vhcurveto',
                       -30, -20, -50, -30, 12, 'vhcurveto', 'endchar'],
        # rcurveline and rlinecurve.
        'ops.mixed': [120, 30, 'rmoveto', 30, 60, 60, 30, 90, 0, 20, -10, 30, 10, 40, 0, 40, 40, 'rcurveline',
                      30, -10, 40, -20, 10, -40, -20, -60, -80, -40, 'rlinecurve', 'endchar'],
        # flex, hflex, hflex1, flex1 (both branches).
        'ops.flex': [100, 100, 'rmoveto', 30, 20, 30, 20, 30, 0, 30, 0, 30, -20, 30, -20, 50, 'flex',
                     20, 30, 10, 20, 30, 20, 20, 'hflex', 10, 5, 20, 10, 30, 30, 20, -10, 20, 'hflex1',
                     10, 20, 10, 20, 10, 0, 10, 0, 10, -20, 30, 'flex1', 5, 20, 5, 20, 0, 10, -5, 10, -5, 20, -40,
                     'flex1', -300, 'hlineto', 'endchar'],
        # Subroutines: local and global, nested, with a width on the moveto.
        'ops.subrs': [600, 100, 100, 'rmoveto', -107, 'callgsubr', 100, 'hlineto', -107, 'callsubr', 'endchar'],
        # Arithmetic and storage, feeding the coordinates.
        'ops.math': [7, 3, 'add', 10, 'mul', 20, 'sub', 2, 'div', 50, 'exch', 'rmoveto',
                     100, 'dup', 'add', 'hlineto', -4, 'abs', 'neg', 'neg', 25, 'mul', 'vlineto',
                     # roll over the whole stack: FreeType (the reference here) rolls the bottom of the
                     # stack when N is smaller than the stack, against the Type 2 specification.
                     1, 2, 3, 3, 1, 'roll', 'drop', 'drop', 0, 'put', 0, 'get', 'hlineto',
                     5, 6, 7, 8, 'ifelse', 60, 'mul', 'vlineto', 9, 'sqrt', 10, 'mul', 1, 0, 'index', 'drop',
                     'hlineto', 1, 1, 'eq', 0, 'and', 1, 'or', 'not', 50, 'add', 'vlineto', 'endchar'],
    }
    return {name: T2CharString(program=prog, private=private, globalSubrs=global_subrs)
            for name, prog in programs.items()}


def build_cff(source_font, family, cid=False):
    glyph_order = [g for g in source_font.getGlyphOrder()]
    gs = source_font.getGlyphSet()
    hmtx = source_font['hmtx']
    fb = FontBuilder(unitsPerEm=source_font['head'].unitsPerEm, isTTF=False)
    extra = ['ops.hints', 'ops.curves', 'ops.mixed', 'ops.flex', 'ops.subrs', 'ops.math']
    # CID-keyed fonts name their glyphs by CID.
    rename_to = {n: (n if (not cid or i == 0) else 'cid%05d' % i) for i, n in enumerate(glyph_order + extra)}
    names = [rename_to[n] for n in glyph_order + extra]
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({cp: rename_to[n] for cp, n in source_font.getBestCmap().items()})
    charstrings = {}
    for name in glyph_order:
        pen = T2CharStringPen(hmtx[name][0], gs)
        gs[name].draw(pen)
        charstrings[rename_to[name]] = pen.getCharString()
    for name in extra:
        charstrings[rename_to[name]] = T2CharString(program=[500, 'endchar'])  # replaced below
    fb.setupCFF(family.replace(' ', ''), {'FullName': family}, charstrings, {})
    cff = fb.font['CFF '].cff
    top = cff.topDictIndex[0]
    private = top.Private
    private.defaultWidthX = 0
    private.nominalWidthX = 0
    private.Subrs = SubrsIndex()
    cff.GlobalSubrs = GlobalSubrsIndex()
    top.GlobalSubrs = cff.GlobalSubrs
    hand = hand_written_charstrings(private, cff.GlobalSubrs)
    # Move every other outline's body into a subroutine (local or global, alternately) so that
    # ordinary glyphs call subroutines too.
    for i, source_name in enumerate(glyph_order):
        name = rename_to[source_name]
        cs = top.CharStrings[name]
        cs.decompile()
        prog = list(cs.program)
        if i % 2 == 1 and 'endchar' in prog and len(prog) > 6:
            body = prog[:-1]
            target = private.Subrs if i % 4 == 1 else cff.GlobalSubrs
            target.append(T2CharString(program=body + ['return'], private=private, globalSubrs=cff.GlobalSubrs))
            index = len(target) - 1
            bias = 107 if len(target) < 1240 else 1131
            prog = [index - bias, 'callsubr' if target is private.Subrs else 'callgsubr', 'endchar']
        top.CharStrings[name] = T2CharString(program=prog, private=private, globalSubrs=cff.GlobalSubrs)
    for name, cs in hand.items():
        top.CharStrings[rename_to[name]] = cs
    metrics = {rename_to[n]: (hmtx[n][0], 0) for n in glyph_order}
    for n in hand:
        metrics[rename_to[n]] = (500, 0)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=source_font['hhea'].ascent, descent=source_font['hhea'].descent)
    fb.setupNameTable({'familyName': family, 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=1556, sTypoDescender=-492, sTypoLineGap=410, usWinAscent=1901, usWinDescent=483,
                fsSelection=0x80 | 0x40, usWeightClass=500, sxHeight=1120, sCapHeight=1493)
    fb.setupPost(underlinePosition=-130, underlineThickness=90)
    font = fb.font
    font.recalcBBoxes = False  # fontTools cannot run the arithmetic operators to compute bounds
    if cid:
        # Convert to CID-keyed: glyph i uses font DICT 0 for even i, 1 for odd i (FDSelect format 3).
        from fontTools.cffLib import FDArrayIndex, FDSelect, FontDict, PrivateDict
        top.ROS = ('Adobe', 'Identity', 0)
        top.CIDCount = len(names)
        fdarray = FDArrayIndex()
        for k in range(2):
            fd = FontDict()
            fd.setCFF2(False)
            priv = private if k == 0 else PrivateDict()
            if k == 1:
                priv.defaultWidthX = 0
                priv.nominalWidthX = 0
                priv.Subrs = private.Subrs  # same subroutines, reached through the second DICT
            fd.Private = priv
            fdarray.append(fd)
        top.FDArray = fdarray
        top.FDSelect = FDSelect(format=3)
        top.FDSelect.gidArray = [i % 2 if 0 < i < len(names) - 6 else 0 for i in range(len(names))]
        for i, n in enumerate(names):
            cs = top.CharStrings[n]
            cs.private = fdarray[top.FDSelect.gidArray[i]].Private
        del top.Private
        top.charset = names
    top.FontBBox = [-2000, -1000, 4000, 3000]
    buf = io.BytesIO()
    font.save(buf)
    return TTFont(io.BytesIO(buf.getvalue()), recalcBBoxes=False)


def build_composite(source_font):
    font = TTFont(io.BytesIO(source_font_bytes(source_font)))
    glyf = font['glyf']
    hmtx = font['hmtx']
    order = font.getGlyphOrder()
    base, other = 'A', 'o'

    def component(name, x=0, y=0, flags=0x0004, transform=None, points=None):
        c = GlyphComponent()
        c.glyphName = name
        c.flags = flags
        if points:
            c.firstPt, c.secondPt = points
        else:
            c.x, c.y = x, y
        if transform:
            c.transform = transform
        return c

    specs = {
        'comp.offset': [component(base), component(other, 1300, -200)],
        'comp.scale': [component(base), component(other, 1200, 0, transform=[[0.5, 0], [0, 0.5]])],
        'comp.xyscale': [component(other, 0, 0, transform=[[0.75, 0], [0, 1.25]])],
        'comp.2x2': [component(other, 100, 50, transform=[[0.5, 0.25], [-0.25, 0.75]])],
        # SCALED_COMPONENT_OFFSET is set, and ignored (as FreeType and fontTools do).
        'comp.scaledoffset': [component(other, 400, 200, flags=0x0004 | 0x0800, transform=[[0.5, 0], [0, 0.5]])],
        'comp.points': [component(base), component(other, points=(3, 0))],
        'comp.nested': [component('comp.offset', 0, 0), component('comp.scale', 100, 1500,
                                                                   transform=[[0.25, 0], [0, 0.25]])],
    }
    for name, comps in specs.items():
        g = Glyph()
        g.numberOfContours = -1
        g.components = comps
        order.append(name)
        glyf.glyphs[name] = g
        hmtx.metrics[name] = (2400, 0)
    font.setGlyphOrder(order)
    glyf.glyphOrder = order
    for name in specs:
        glyf[name].recalcBounds(glyf)
        hmtx.metrics[name] = (2400, glyf[name].xMin)
    font['maxp'].numGlyphs = len(order)
    font['maxp'].maxComponentDepth = 3
    rename(font, 'CfwTest Composite')
    buf = io.BytesIO()
    font.save(buf)
    return TTFont(io.BytesIO(buf.getvalue()))


def source_font_bytes(font):
    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue()


def expectations(path, index=0):
    font = TTFont(path, fontNumber=index) if path.endswith('.ttc') else TTFont(path)
    # Canonical outlines through the same code as outlines.py (with FreeType's composite placement).
    out = io.StringIO()
    old = sys.stdout
    sys.argv = ['outlines.py', path, str(index)]
    sys.stdout = out
    try:
        outlines.main()
    finally:
        sys.stdout = old
    lines = out.getvalue().splitlines()
    blocks = []
    for b in range(0, len(lines), 64):
        text = '\n'.join(lines[b:b + 64]) + '\n'
        blocks.append(hashlib.sha256(text.encode()).hexdigest()[:16])
    order = font.getGlyphOrder()
    rev = {n: i for i, n in enumerate(order)}
    cmap = ''.join('%X %d\n' % (c, rev[g]) for c, g in sorted(font.getBestCmap().items()) if rev[g])
    os2 = font['OS/2']
    hhea = font['hhea']
    typo = bool(os2.fsSelection & 0x80)
    names = font['name']
    family = names.getDebugName(16) or names.getDebugName(1)
    style = names.getDebugName(17) or names.getDebugName(2)
    return {
        'glyphs': len(order),
        'blocks': blocks,
        'cmap': hashlib.sha256(cmap.encode()).hexdigest()[:16],
        'mapped': cmap.count('\n'),
        'family': family,
        'style': style,
        'unitsPerEm': font['head'].unitsPerEm,
        'ascender': os2.sTypoAscender if typo else hhea.ascent,
        'descender': os2.sTypoDescender if typo else hhea.descent,
        'lineGap': os2.sTypoLineGap if typo else hhea.lineGap,
        'weight': os2.usWeightClass,
        'cff': 'CFF ' in font,
    }


def main():
    source = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    shutil.copy(source, os.path.join(OUT, 'DejaVuSans.ttf'))
    latin = subset_font(source, LATIN)
    cff = build_cff(latin, 'CfwTest Cff')
    cff.save(os.path.join(OUT, 'CfwTestCff.otf'))
    cid = build_cff(latin, 'CfwTest Cid', cid=True)
    cid.save(os.path.join(OUT, 'CfwTestCid.otf'))
    composite = build_composite(latin)
    composite.save(os.path.join(OUT, 'CfwTestComposite.ttf'))
    a = subset_font(source, 'Hamburgefonstiv 0123')
    rename(a, 'CfwTest Collection A')
    b = subset_font(source, 'ABC xyz 789')
    rename(b, 'CfwTest Collection B')
    collection = TTCollection()
    collection.fonts = [a, b]
    collection.save(os.path.join(OUT, 'CfwTestCollection.ttc'))
    expected = {}
    for name in ['DejaVuSans.ttf', 'CfwTestCff.otf', 'CfwTestCid.otf', 'CfwTestComposite.ttf']:
        expected[name] = expectations(os.path.join(OUT, name))
    for i in range(2):
        expected[f'CfwTestCollection.ttc#{i}'] = expectations(os.path.join(OUT, 'CfwTestCollection.ttc'), i)
    with open(os.path.join(OUT, 'expected.json'), 'w') as f:
        json.dump(expected, f, indent=1)
    for k, v in expected.items():
        print(k, v['glyphs'], 'glyphs', v['mapped'], 'mapped', v['family'])


if __name__ == '__main__':
    main()
