#!/usr/bin/env python3
"""Builds the Indic engine's synthetic test fonts in modules/cfw-text/testdata/fonts,
and HarfBuzz's results for them in modules/cfw-text/testdata/shaping:

    CfwTestIndic.ttf     Devanagari and Malayalam with the second spec's tags (dev2, mlm2)
    CfwTestIndicOld.ttf  the same lookups under the first spec's tags (deva, mlym)

Box outlines; the lookups are what matters. They are built to reach what
real fonts leave alone: the below-base Ra form exists only before KA (a
chained rule with lookahead), so finding the base consonant depends on
whether "would substitute" tests may use context -- not for the second
Devanagari spec, but for the first, and for Malayalam in both. There are
also reph, half, post-base, pre-base-reordering and presentation forms,
nukta and a GPOS mark attachment.

    python3 testing/text-oracle/make_indic_font.py
"""
import json
import os
import random
import sys
import zlib

from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

sys.path.insert(0, os.path.dirname(__file__))
import shape  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
FONTS = os.path.join(ROOT, 'modules', 'cfw-text', 'testdata', 'fonts')
SHAPING = os.path.join(ROOT, 'modules', 'cfw-text', 'testdata', 'shaping')

CHARS = {
    # Devanagari
    'ka': 0x915, 'kha': 0x916, 'ta': 0x924, 'ya': 0x92F, 'ra': 0x930, 'va': 0x935, 'ssa': 0x937, 'sa': 0x938,
    'virama': 0x94D, 'nukta': 0x93C, 'imatra': 0x93F, 'aamatra': 0x93E, 'ematra': 0x947, 'umatra': 0x941,
    'anusvara': 0x902, 'aa': 0x906,
    # Malayalam
    'mka': 0xD15, 'mta': 0xD24, 'mya': 0xD2F, 'mra': 0xD30, 'mva': 0xD35, 'mvirama': 0xD4D, 'mematra': 0xD46,
    'maamatra': 0xD3E, 'mdotreph': 0xD4E,
    # Common
    'space': 0x20, 'zwnj': 0x200C, 'zwj': 0x200D, 'dottedcircle': 0x25CC,
}
MARKS = ['virama', 'nukta', 'umatra', 'ematra', 'anusvara', 'mvirama', 'rakar', 'yaphala', 'mrakar']
EXTRA = ['rakar', 'yaphala', 'reph', 'ka.half', 'ta.half', 'vapost', 'kssa', 'ka_rakar', 'mrakar', 'mreph',
         'myapost', 'mka.chillu', 'imatra.alt', 'reph_anusvara']

FEATURES = r'''
languagesystem DFLT dflt;
languagesystem dev2 dflt;
languagesystem mlm2 dflt;

table GDEF {
    GlyphClassDef [ka kha ta ya ra va ssa sa aa mka mta mya mra mva ka.half ta.half mka.chillu
                   dottedcircle space vapost myapost reph mreph mdotreph imatra imatra.alt aamatra mematra maamatra
                   reph_anusvara],
                  [kssa ka_rakar], [virama nukta umatra ematra anusvara mvirama rakar yaphala mrakar], ;
} GDEF;

lookup RAKAR { sub virama ra by rakar; } RAKAR;
lookup MRAKAR { sub mvirama mra by mrakar; } MRAKAR;

feature nukt { script dev2; sub ka nukta by kha; } nukt;
feature akhn { script dev2; sub ka virama ssa by kssa; } akhn;
feature rphf {
    script dev2; sub ra virama by reph;
    script mlm2; sub mra mvirama by mreph;
} rphf;
feature blwf {
    script dev2;
    sub virama' lookup RAKAR ra' ka;
    sub virama ya by yaphala;
    script mlm2;
    sub mvirama' lookup MRAKAR mra' mka;
} blwf;
feature half { script dev2; sub ka virama by ka.half; sub ta virama by ta.half; } half;
feature pstf {
    script dev2; sub virama va by vapost;
    script mlm2; sub mvirama mya by myapost;
} pstf;
feature pref { script mlm2; sub mvirama mva by mrakar; } pref;
feature init { script dev2; sub imatra by imatra.alt; } init;
feature pres { script dev2; sub ka rakar by ka_rakar; } pres;
feature abvs { script dev2; sub reph anusvara by reph_anusvara; } abvs;
feature haln { script mlm2; sub mka mvirama by mka.chillu; } haln;

feature mark {
    script dev2;
    markClass nukta <anchor 0 -50> @BELOW;
    markClass umatra <anchor 0 -50> @BELOW;
    pos base [ka ta ra ya va sa] <anchor 250 0> mark @BELOW;
} mark;
'''


def build(path, old):
    names = ['.notdef'] + list(CHARS) + EXTRA
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({u: n for n, u in CHARS.items()})
    glyphs = {}
    metrics = {}
    for i, n in enumerate(names):
        pen = TTGlyphPen(None)
        mark = n in MARKS
        w = 0 if mark else 400 + (i * 37) % 300
        if n not in ('space', 'zwnj', 'zwj'):
            x0, y0 = (-150, -200) if mark else (40, 0)
            x1, y1 = (x0 + 120 + (i * 13) % 60, y0 + 120) if mark else (w - 40, 500 + (i * 29) % 200)
            pen.moveTo((x0, y0))
            pen.lineTo((x0, y1))
            pen.lineTo((x1, y1))
            pen.lineTo((x1, y0))
            pen.closePath()
        glyphs[n] = pen.glyph()
        metrics[n] = (w, 0)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    family = 'CfwTestIndicOld' if old else 'CfwTestIndic'
    fb.setupNameTable({'familyName': family, 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    addOpenTypeFeaturesFromString(fb.font, FEATURES)
    if old:  # the first spec's script tags
        for tb in ('GSUB', 'GPOS'):
            records = fb.font[tb].table.ScriptList.ScriptRecord
            for r in records:
                r.ScriptTag = {'dev2': 'deva', 'mlm2': 'mlym'}.get(r.ScriptTag, r.ScriptTag)
            records.sort(key=lambda r: r.ScriptTag)
    fb.font.save(path)


def corpus(rng):
    deva = [chr(CHARS[n]) for n in ('ka', 'ta', 'ya', 'ra', 'va', 'ssa', 'sa', 'aa')]
    dev_marks = [chr(CHARS[n]) for n in ('nukta', 'imatra', 'aamatra', 'ematra', 'umatra', 'anusvara')]
    mlym = [chr(CHARS[n]) for n in ('mka', 'mta', 'mya', 'mra', 'mva')]
    ml_marks = [chr(CHARS[n]) for n in ('mematra', 'maamatra')]
    texts = []
    v, mv = chr(CHARS['virama']), chr(CHARS['mvirama'])
    ra, mra, ka, mka = chr(CHARS['ra']), chr(CHARS['mra']), chr(CHARS['ka']), chr(CHARS['mka'])
    # Every consonant pair and triple joined by the virama.
    for a in deva:
        for b in deva:
            texts.append(a + v + b)
            texts.append(a + v + b + chr(CHARS['imatra']))
            texts.append(ra + v + a + v + b)
    for a in mlym:
        for b in mlym:
            texts.append(a + mv + b)
            texts.append(a + mv + b + chr(CHARS['mematra']))
            texts.append(mra + mv + a + mv + b)
    texts += [ka + v + ra + v + ka, ka + v + ra + ka, mka + mv + mra + mv + mka, chr(CHARS['mdotreph']) + mka,
              ka + v + '‍' + ra, ka + v + '‌' + ra + chr(CHARS['imatra']), ra + v + '‍' + ka]
    for _ in range(600):
        if rng.random() < 0.6:
            s = ''.join(rng.choice(deva) + (v if rng.random() < 0.6 else '') for _ in range(rng.randint(1, 4)))
            s += ''.join(rng.choice(dev_marks) for _ in range(rng.randint(0, 2)))
        else:
            s = ''.join(rng.choice(mlym) + (mv if rng.random() < 0.6 else '') for _ in range(rng.randint(1, 4)))
            s += ''.join(rng.choice(ml_marks) for _ in range(rng.randint(0, 2)))
        texts.append(s)
    return texts


def main():
    import uharfbuzz as hb
    rng = random.Random(7)
    texts = corpus(rng)
    for name, old in (('CfwTestIndic', False), ('CfwTestIndicOld', True)):
        path = os.path.join(FONTS, name + '.ttf')
        build(path, old)
        font = hb.Font(hb.Face(hb.Blob.from_file_path(path)))
        cases = [shape.shape(font, t) for t in texts]
        data = json.dumps({'font': name + '.ttf', 'harfbuzz': hb.__version__, 'cases': cases},
                          separators=(',', ':')) + '\n'
        with open(os.path.join(SHAPING, name + '.json.z'), 'wb') as f:
            f.write(zlib.compress(data.encode(), 9))
        print(name, len(cases), 'cases')


if __name__ == '__main__':
    main()
