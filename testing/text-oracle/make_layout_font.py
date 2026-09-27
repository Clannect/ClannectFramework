#!/usr/bin/env python3
"""Builds the shaping test fonts in modules/cfw-text/testdata/fonts:

    CfwTestLayout.ttf   a small font whose GSUB, GPOS, GDEF and kern tables use every lookup type and subtable
                        format the shaper implements (outlines are plain boxes; only the layout matters). The
                        script checks that the compiled tables really contain each type and format (fontTools'
                        feature compiler picks formats itself) and fails otherwise.
    CfwTestPlain.ttf    DejaVu Sans without GSUB, GPOS and GDEF (Latin, marks, Greek, Hebrew, Arabic and their
                        presentation forms, and the kern table): the paths for fonts without layout tables --
                        marks placed by glyph boxes, Arabic and Hebrew presentation forms, legacy kerning.
    CfwTestPlain.otf    the same (smaller) with CFF outlines, for boxes computed from CFF.

    python3 testing/text-oracle/make_layout_font.py
"""
import io
import os
import struct

from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.otlLib import builder as otl
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0, KernTable_format_unkown

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'modules', 'cfw-text', 'testdata', 'fonts', 'CfwTestLayout.ttf')

LATIN = 'abcdefghijklmnopqrstuvwxyzAVTWLY'
DIGITS = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine']
GREEK = {'alpha': 0x3B1, 'beta': 0x3B2, 'gamma': 0x3B3, 'delta': 0x3B4, 'Alpha': 0x391, 'Tau': 0x3A4, 'Upsilon': 0x3A5}
MARKS = {'gravecomb': 0x300, 'acutecomb': 0x301, 'circumflexcomb': 0x302, 'tildecomb': 0x303, 'dotaccentcomb': 0x307,
         'dotbelowcomb': 0x323, 'cedillacomb': 0x327, 'ogonekcomb': 0x328}
ARABIC = {'alef': 0x627, 'beh': 0x628, 'teh': 0x62A, 'lam': 0x644, 'meem': 0x645, 'noon': 0x646, 'heh': 0x647,
          'reh': 0x631, 'yeh': 0x64A, 'fatha': 0x64E, 'damma': 0x64F, 'kasra': 0x650, 'shadda': 0x651,
          'sukun': 0x652, 'tatweel': 0x640}
JOINING = ['beh', 'teh', 'lam', 'meem', 'noon', 'heh', 'yeh']
ARABIC_MARKS = ['fatha', 'damma', 'kasra', 'shadda', 'sukun']
OTHER = {'space': 0x20, 'period': 0x2E, 'comma': 0x2C, 'hyphen': 0x2D, 'fraction': 0x2044, 'slash': 0x2F,
         'uni200C': 0x200C, 'uni200D': 0x200D, 'uni034F': 0x34F, 'dotlessi': 0x131, 'parenleft': 0x28,
         'parenright': 0x29}

FEATURES = r'''
languagesystem DFLT dflt;
languagesystem latn dflt;
languagesystem latn TRK;
languagesystem grek dflt;
languagesystem arab dflt;

@LC = [a b c d e f g h i j k l m n o p q r s t u v w x y z];
@DIGITS = [zero one two three four five six seven eight nine];
@TOP = [gravecomb acutecomb circumflexcomb tildecomb dotaccentcomb];
@BOTTOM = [dotbelowcomb cedillacomb ogonekcomb];
@AMARK = [fatha damma kasra shadda sukun];

table GDEF {
    GlyphClassDef [@LC @DIGITS A V T W L Y alpha beta gamma delta Alpha Tau Upsilon alef reh tatweel
                   beh teh lam meem noon heh yeh beh.init beh.medi beh.fina teh.init teh.medi teh.fina
                   lam.init lam.medi lam.fina meem.init meem.medi meem.fina noon.init noon.medi noon.fina
                   heh.init heh.medi heh.fina yeh.init yeh.medi yeh.fina alef.fina reh.fina
                   a.alt1 a.alt2 r.alt1 r.alt2 r.alt3 x.alt e.rev q.ext s.ctx1 t.ctx2 u.ctx3 v.ctx5 w.ctx6
                   dotlessi zero.numr one.numr two.numr zero.dnom one.dnom two.dnom],
                  [f_i f_f_i f_f lam_alef lam_alef.fina],
                  [@TOP @BOTTOM @AMARK],
                  ;
} GDEF;

# ---- GSUB ----

lookup MULT { sub i by dotlessi dotaccentcomb; } MULT;              # type 2
lookup DOUBLE { sub x by x x.alt; } DOUBLE;                         # type 2, in a context
lookup SINGLE1 { sub [s t] by [s.ctx1 t.ctx2]; } SINGLE1;           # type 1
lookup SINGLE2 { sub u by u.ctx3; sub v by v.ctx5; sub w by w.ctx6; } SINGLE2;
lookup LIGCTX { sub f f by f_f; } LIGCTX;
lookup SINGLE3 { sub a by e.rev; sub b by q.ext; sub c by a.alt2; sub d by x.alt; sub [i j k l m n o p q r s t u v w x y z e f g h] by [s.ctx1 t.ctx2 u.ctx3 v.ctx5 w.ctx6 a.alt1 a.alt2 r.alt1 r.alt2 r.alt3 x.alt e.rev q.ext zero.numr one.numr two.numr zero.dnom one.dnom two.dnom f_i f_f_i f_f]; } SINGLE3;  # type 1 format 2

feature ccmp { lookup MULT; } ccmp;

feature liga {
    sub f f i by f_f_i;                                             # type 4
    sub f i by f_i;
} liga;

feature salt { sub a from [a.alt1 a.alt2]; } salt;                  # type 3
feature rand { sub r from [r.alt1 r.alt2 r.alt3]; } rand;

lookup CTXF1 {
    # type 6, glyph rules (format 1)
    sub s' lookup SINGLE1 o p;
    sub s' lookup SINGLE1 o q;
    sub h s' lookup SINGLE1 m;
    sub z x' lookup DOUBLE z;
    sub y f' lookup LIGCTX f' lookup LIGCTX y;
} CTXF1;
lookup CTXF3 {
    # type 6, coverage (format 3)
    sub [g j] [c d]' lookup SINGLE2 [k l];
} CTXF3;
feature calt { lookup CTXF1; lookup CTXF3; } calt;

lookup CTX5F1 {
    # type 5 (no context outside the input), glyph rules (format 1)
    sub t' lookup SINGLE1 u' lookup SINGLE2;
    sub w' lookup SINGLE2 v' lookup SINGLE2;
} CTX5F1;
lookup CTX5F3 {
    sub [p q]' lookup SINGLE3 [u v w]' lookup SINGLE2;
} CTX5F3;
lookup CTX5F2 {
    @C5A = [a b c d e f g h];
    @C5B = [i j k l m n o p];
    @C5C = [q r s t u v w x];
    sub @C5A' lookup SINGLE3 @C5B' @C5C';  # force-format 2
    sub @C5B' lookup SINGLE3 @C5A' @C5C';
    sub @C5C' lookup SINGLE2 @C5C' @C5A';
    sub @C5A' lookup SINGLE3 @C5A' @C5A';
    sub @C5B' lookup SINGLE3 @C5B' @C5B';
    sub @C5C' lookup SINGLE2 @C5B' @C5B';
} CTX5F2;
feature clig { lookup CTX5F1; lookup CTX5F3; lookup CTX5F2; } clig;

lookup CTXF2 {
    # type 6 with classes (format 2): many rules sharing classes
    @C6A = [b c d e f g h j];
    @C6B = [k l m n o p];
    @C6C = [q r s t u v w];
    @C6D = [x y z];
    sub @C6A @C6B' lookup SINGLE3 @C6C;  # force-format 2
    sub @C6A @C6C' lookup SINGLE2 @C6A;
    sub @C6D @C6B' lookup SINGLE3 @C6D;
    sub @C6D @C6C' lookup SINGLE2 @C6B;
    sub @C6C @C6B' lookup SINGLE3 @C6A;
    sub @C6B @C6D' lookup SINGLE3 @C6C;
} CTXF2;
feature rclt { lookup CTXF2; } rclt;

feature ss01 {
    rsub [b c] e' [d g] by e.rev;                                   # type 8
} ss01;

lookup EXTSUB useExtension { sub q by q.ext; } EXTSUB;            # type 7 (extension)
feature ss02 { lookup EXTSUB; } ss02;

feature numr { sub [zero one two] by [zero.numr one.numr two.numr]; } numr;
feature dnom { sub [zero one two] by [zero.dnom one.dnom two.dnom]; } dnom;
feature frac { sub fraction by slash; } frac;

feature locl {
    script latn;
    language TRK;
    sub i by dotlessi;
} locl;

feature rqd1 {
    script latn;
    language TRK required;
    sub k by x;
} rqd1;

feature init { script arab; sub [beh teh lam meem noon heh yeh] by [beh.init teh.init lam.init meem.init noon.init heh.init yeh.init]; } init;
feature medi { script arab; sub [beh teh lam meem noon heh yeh] by [beh.medi teh.medi lam.medi meem.medi noon.medi heh.medi yeh.medi]; } medi;
feature fina { script arab; sub [beh teh lam meem noon heh yeh alef reh] by [beh.fina teh.fina lam.fina meem.fina noon.fina heh.fina yeh.fina alef.fina reh.fina]; } fina;
feature rlig {
    script arab;
    lookupflag IgnoreMarks;
    sub lam.init alef.fina by lam_alef;
    sub lam.medi alef.fina by lam_alef.fina;
} rlig;

# ---- GPOS ----

markClass [gravecomb acutecomb circumflexcomb tildecomb] <anchor 150 700> @MTOP;
markClass dotaccentcomb <anchor 150 650 contourpoint 0> @MTOP;
markClass [dotbelowcomb ogonekcomb] <anchor 150 -60> @MBOTTOM;
markClass cedillacomb <anchor 150 0 <device 12 1> <device 12 -1>> @MBOTTOM;
markClass [fatha damma shadda sukun] <anchor 100 600> @ATOP;
markClass kasra <anchor 100 -80> @ABOTTOM;

feature kern {
    script latn;
    pos A V -80;                                                   # type 2 format 1
    pos T o -60;
    pos A W <-40 0 -40 0>;
    @KL = [L T Y];
    @KR = [a c e o];
    pos @KL @KR -30;                                               # type 2 format 2
    pos [V W] [a e o] -25;
    pos f 20;                                                      # type 1 format 1
    pos [b d] <5 10 15 0>;
    pos [g j] <0 -20 0 0>;
} kern;

lookup SINGLEPOS2 {                                                # type 1 format 2
    pos h <1 2 3 0>;
    pos k <4 5 6 0>;
    pos l <7 8 9 0>;
    pos m <10 11 12 0>;
    pos n <13 14 15 0>;
    pos p <16 17 18 0>;
} SINGLEPOS2;
 feature ss05 { lookup SINGLEPOS2; } ss05;
feature kern { script latn; 
} kern;

feature mark {
    pos base [@LC @DIGITS A V T W L Y dotlessi] <anchor 300 720> mark @MTOP <anchor 300 -10> mark @MBOTTOM;  # type 4
    pos ligature f_i <anchor 200 720> mark @MTOP <anchor 200 -10> mark @MBOTTOM
        ligComponent <anchor 600 720> mark @MTOP <anchor 600 -10> mark @MBOTTOM;                              # type 5
    pos ligature f_f_i <anchor 150 720> mark @MTOP ligComponent <anchor 450 720> mark @MTOP
        ligComponent <anchor 800 720> mark @MTOP;
    pos base [beh teh lam meem noon heh yeh alef reh tatweel beh.init beh.medi beh.fina teh.init teh.medi
              teh.fina lam.init lam.medi lam.fina meem.init meem.medi meem.fina noon.init noon.medi
              noon.fina heh.init heh.medi heh.fina yeh.init yeh.medi yeh.fina alef.fina reh.fina]
        <anchor 250 650> mark @ATOP <anchor 250 -100> mark @ABOTTOM;
    pos ligature [lam_alef lam_alef.fina] <anchor 500 700> mark @ATOP <anchor 500 -100> mark @ABOTTOM
        ligComponent <anchor 150 700> mark @ATOP <anchor 150 -100> mark @ABOTTOM;
} mark;

feature mkmk {
    lookupflag MarkAttachmentType @TOP;
    pos mark [gravecomb acutecomb circumflexcomb tildecomb] <anchor 150 950> mark @MTOP;        # type 6
    lookupflag 0;
    lookupflag UseMarkFilteringSet @BOTTOM;
    pos mark [dotbelowcomb ogonekcomb] <anchor 150 -300> mark @MBOTTOM;
    lookupflag 0;
    pos mark shadda <anchor 100 900> mark @ATOP;
} mkmk;

feature curs {
    script arab;
    lookupflag RightToLeft IgnoreMarks;
    pos cursive beh.init <anchor NULL> <anchor 0 100>;                                          # type 3
    pos cursive beh.medi <anchor 500 100> <anchor 0 60>;
    pos cursive teh.medi <anchor 500 60> <anchor 0 140>;
    pos cursive lam.medi <anchor 500 140> <anchor 0 100>;
    pos cursive meem.medi <anchor 500 100> <anchor 0 80>;
    pos cursive noon.init <anchor NULL> <anchor 0 120>;
    pos cursive meem.init <anchor NULL> <anchor 0 90>;
    pos cursive [beh.fina teh.fina meem.fina noon.fina heh.fina yeh.fina] <anchor 500 100> <anchor NULL>;
    pos cursive lam.init <anchor NULL> <anchor 0 70>;
} curs;

lookup CURSLTR {
    pos cursive m <anchor 0 0> <anchor 500 30>;
    pos cursive n <anchor 0 30> <anchor 500 -20>;
    pos cursive u <anchor 0 -20> <anchor 500 0>;
} CURSLTR;
feature ss03 { lookup CURSLTR; } ss03;

lookup POSA { pos a 50; } POSA;
lookup POSB { pos b <0 30 0 0>; } POSB;
lookup POSCTX1 {
    pos a' lookup POSA b' lookup POSB;                             # type 7 format 1  # force-format 1
    pos b' lookup POSB a' lookup POSA;
} POSCTX1;
feature dist {
    lookup POSCTX1;
    pos [c e] a' lookup POSA [d n];                                # type 8 (chained)
} dist;
lookup POSCTX3 {
    pos [a c]' lookup POSA [b e]' lookup POSB;                     # type 7 format 3  # force-format 3
} POSCTX3;
lookup POSCTX2 {
    @P2A = [a b c d];
    @P2B = [e f g h];
    pos @P2A' lookup POSA @P2B' lookup POSB;  # force-format 2
    pos @P2B' lookup POSB @P2A';
} POSCTX2;
lookup POSCHAIN1 {
    pos x a' lookup POSA b;  # force-format 1
    pos y a' lookup POSA c;
} POSCHAIN1;
lookup POSCHAIN2 {
    @P8A = [k l m];
    @P8B = [n o p];
    pos @P8A @P8B' lookup POSA @P8A;  # force-format 2
    pos @P8B @P8A' lookup POSA @P8B;
} POSCHAIN2;
feature ss06 { lookup POSCTX3; lookup POSCTX2; lookup POSCHAIN1; lookup POSCHAIN2; } ss06;
feature dist {
} dist;

lookup EXTPOS useExtension { pos z -40; } EXTPOS;                 # type 9 (extension)
feature ss04 { lookup EXTPOS; } ss04;
'''


# fontTools picks contextual subtable formats by size; a rule marked `# force-format N` (the first rule of
# its lookup) gets format N instead, so that every format is exercised.
FORCED = {i + 1: int(line.split('force-format')[1].split()[0])
          for i, line in enumerate(FEATURES.split('\n')) if 'force-format' in line}
_compiled_size = otl.ChainContextualBuilder.getCompiledSize_


def _forced_size(self, subtables):
    want = FORCED.get(getattr(self.location, 'line', None))
    if want is not None and subtables and getattr(subtables[0], 'Format', None) == want:
        return -1
    return _compiled_size(self, subtables)


otl.ChainContextualBuilder.getCompiledSize_ = _forced_size


def box(width, height=700, depth=0, thin=False):
    pen = TTGlyphPen(None)
    x0, x1 = (40, width - 40) if not thin else (width // 2 - 30, width // 2 + 30)
    pen.moveTo((x0, depth))
    pen.lineTo((x0, height))
    pen.lineTo((x1, height))
    pen.lineTo((x1, depth))
    pen.closePath()
    return pen.glyph()


def build():
    names = ['.notdef']
    cmap = {}
    for c in LATIN:
        names.append(c)
        cmap[ord(c)] = c
    for i, d in enumerate(DIGITS):
        names.append(d)
        cmap[0x30 + i] = d
    for table in (GREEK, MARKS, ARABIC, OTHER):
        for n, u in table.items():
            names.append(n)
            cmap[u] = n
    extra = ['f_i', 'f_f_i', 'f_f', 'a.alt1', 'a.alt2', 'r.alt1', 'r.alt2', 'r.alt3', 'x.alt', 'e.rev', 'q.ext',
             's.ctx1', 't.ctx2', 'u.ctx3', 'v.ctx5', 'w.ctx6', 'zero.numr', 'one.numr', 'two.numr', 'zero.dnom',
             'one.dnom', 'two.dnom', 'lam_alef', 'lam_alef.fina', 'alef.fina', 'reh.fina']
    for base in JOINING:
        extra += [base + '.init', base + '.medi', base + '.fina']
    names += extra

    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    glyphs = {}
    metrics = {}
    for i, n in enumerate(names):
        mark = n in MARKS or n in ARABIC_MARKS
        width = 0 if mark else (0 if n in ('uni200C', 'uni200D', 'uni034F') else 400 + (i * 37) % 500)
        if n == 'space':
            glyphs[n] = TTGlyphPen(None).glyph()
        elif mark:
            glyphs[n] = box(300, 900 if n not in ('dotbelowcomb', 'cedillacomb', 'ogonekcomb', 'kasra') else -40,
                            750 if n not in ('dotbelowcomb', 'cedillacomb', 'ogonekcomb', 'kasra') else -200)
        else:
            glyphs[n] = box(max(width, 100))
        metrics[n] = (width, 40 if width else 0)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=900, descent=-250)
    fb.setupNameTable({'familyName': 'CfwTest Layout', 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=900, sTypoDescender=-250, usWinAscent=950, usWinDescent=300)
    fb.setupPost()
    font = fb.font
    font.cfg['fontTools.otlLib.builder:WRITE_GPOS7'] = True
    addOpenTypeFeaturesFromString(font, FEATURES)

    # Legacy kern table for scripts without GPOS kerning (Greek): format 0 pairs and a
    # hand-assembled format 2 (class) subtable.
    kern = newTable('kern')
    kern.version = 0
    fmt0 = KernTable_format_0()
    fmt0.version, fmt0.coverage, fmt0.format = 0, 1, 0
    fmt0.kernTable = {('Alpha', 'Tau'): -70, ('Tau', 'alpha'): -50, ('Upsilon', 'alpha'): -45, ('alpha', 'beta'): 12,
                      ('gamma', 'delta'): -8}
    order = font.getGlyphOrder()
    gid = {n: i for i, n in enumerate(order)}
    left = [gid['beta'], gid['gamma']]
    right = [gid['alpha'], gid['delta']]
    first_l, first_r = min(left), min(right)
    # rowWidth 4 (two right classes), classes as byte offsets; array of 2x2 values at the end.
    header = 6
    body = 8
    lct = header + body
    lcount = max(left) - first_l + 1
    rct = lct + 4 + 2 * lcount
    rcount = max(right) - first_r + 1
    arr = rct + 4 + 2 * rcount
    lclass = [0] * lcount
    for g in left:
        lclass[g - first_l] = arr + (4 if g == gid['gamma'] else 0)
    rclass = [0] * rcount
    for g in right:
        rclass[g - first_r] = 2 if g == gid['delta'] else 0
    values = [-20, 7, 15, -33]  # beta-alpha, beta-delta, gamma-alpha, gamma-delta
    data = struct.pack('>HHHH', 4, lct, rct, arr)
    data += struct.pack('>HH', first_l, lcount) + b''.join(struct.pack('>H', v) for v in lclass)
    data += struct.pack('>HH', first_r, rcount) + b''.join(struct.pack('>H', v) for v in rclass)
    data += b''.join(struct.pack('>h', v) for v in values)
    fmt2 = KernTable_format_unkown(2)
    fmt2.data = struct.pack('>HHBB', 0, header + len(data), 2, 1) + data
    kern.kernTables = [fmt0, fmt2]
    font['kern'] = kern
    return font


def check(font):
    """Every lookup type and the subtable formats the shaper implements must be present."""
    found = set()
    for tag in ('GSUB', 'GPOS'):
        for lookup in font[tag].table.LookupList.Lookup:
            for st in lookup.SubTable:
                kind = lookup.LookupType
                if kind in (7, 9) and tag == ('GSUB' if kind == 7 else 'GPOS'):
                    found.add((tag, kind, 1))
                    kind, st = st.ExtensionLookupType, st.ExtSubTable
                fmt = getattr(st, 'Format', None) or 1
                if tag == 'GSUB' and kind == 1:  # decompiled as a mapping: the format follows from the deltas
                    gid = font.getReverseGlyphMap()
                    deltas = {(gid[b] - gid[a]) % 65536 for a, b in st.mapping.items()}
                    fmt = 1 if len(deltas) == 1 else 2
                found.add((tag, kind, fmt))
    want = {('GSUB', 1, 1), ('GSUB', 1, 2), ('GSUB', 2, 1), ('GSUB', 3, 1), ('GSUB', 4, 1), ('GSUB', 5, 1), ('GSUB', 5, 2), ('GSUB', 5, 3),
            ('GSUB', 6, 1), ('GSUB', 6, 2), ('GSUB', 6, 3), ('GSUB', 7, 1), ('GSUB', 8, 1),
            ('GPOS', 1, 1), ('GPOS', 1, 2), ('GPOS', 2, 1), ('GPOS', 2, 2), ('GPOS', 3, 1), ('GPOS', 4, 1),
            ('GPOS', 5, 1), ('GPOS', 6, 1), ('GPOS', 7, 1), ('GPOS', 7, 2), ('GPOS', 7, 3), ('GPOS', 8, 1), ('GPOS', 8, 2),
            ('GPOS', 8, 3), ('GPOS', 9, 1)}
    missing = want - found
    print('lookup types/formats:', sorted(found))
    assert not missing, f'missing {sorted(missing)}'


PLAIN_TEXT = (list(range(0x20, 0x7F)) + list(range(0xA0, 0x180)) + list(range(0x300, 0x370)) +
              list(range(0x370, 0x400)) + list(range(0x590, 0x5F5)) + list(range(0xFB1D, 0xFB50)) +
              list(range(0x621, 0x656)) + [0x670] + list(range(0xFE70, 0xFEFD)) + list(range(0xFC5E, 0xFC63)) +
              [0x2010, 0x2044, 0x200C, 0x200D, 0x25CC])


def plain_fonts():
    from fontTools import subset
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.ttLib import TTFont
    source = os.path.join(os.path.dirname(OUT), 'DejaVuSans.ttf')

    def cut(unicodes):
        font = TTFont(source)
        options = subset.Options()
        options.layout_features = []
        options.drop_tables += ['GSUB', 'GPOS', 'GDEF', 'BASE', 'JSTF', 'MATH']
        options.name_IDs = ['*']
        options.notdef_outline = True
        options.hinting = False
        options.legacy_kern = True
        sub = subset.Subsetter(options)
        sub.populate(unicodes=unicodes)
        sub.subset(font)
        for rec in font['name'].names:
            if rec.nameID in (1, 16):
                rec.string = 'CfwTest Plain'
            elif rec.nameID == 4:
                rec.string = 'CfwTest Plain Regular'
            elif rec.nameID == 6:
                rec.string = 'CfwTestPlain-Regular'
        return font

    ttf = cut(PLAIN_TEXT)
    small = cut(list(range(0x20, 0x7F)) + list(range(0x300, 0x370)) + list(range(0x391, 0x3CA)))
    order = small.getGlyphOrder()
    gs = small.getGlyphSet()
    hmtx = small['hmtx']
    fb = FontBuilder(unitsPerEm=small['head'].unitsPerEm, isTTF=False)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(small.getBestCmap())
    charstrings = {}
    for name in order:
        pen = T2CharStringPen(hmtx[name][0], gs)
        gs[name].draw(pen)
        charstrings[name] = pen.getCharString()
    fb.setupCFF('CfwTestPlain', {'FullName': 'CfwTest Plain'}, charstrings, {})
    fb.setupHorizontalMetrics({n: hmtx[n] for n in order})
    fb.setupHorizontalHeader(ascent=small['hhea'].ascent, descent=small['hhea'].descent)
    fb.setupNameTable({'familyName': 'CfwTest Plain', 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    fb.font['kern'] = small['kern']
    return ttf, fb.font


def main():
    for font, name in zip(plain_fonts(), ('CfwTestPlain.ttf', 'CfwTestPlain.otf')):
        path = os.path.join(os.path.dirname(OUT), name)
        font.save(path)
        print(f'{path}: {os.path.getsize(path)} bytes')
    font = build()
    buf = io.BytesIO()
    font.save(buf)
    from fontTools.ttLib import TTFont
    check(TTFont(io.BytesIO(buf.getvalue())))
    with open(OUT, 'wb') as f:
        f.write(buf.getvalue())
    print(f'{OUT}: {len(buf.getvalue())} bytes')


if __name__ == '__main__':
    main()
