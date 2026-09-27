#!/usr/bin/env python3
"""Shaping results from HarfBuzz (the outside reference for cfw-text's Shaper).

    python3 shape.py font.ttf out.json[.z] [--random N] [--seed S] [--layout]

Shapes a fixed corpus (and optionally N random strings drawn from per-script
character pools) with uharfbuzz at font-unit scale, default language, and
writes one case per string: the text as code points, the script (ISO 15924)
and direction HarfBuzz resolved, the features, and the glyphs as
[glyph, cluster, x_advance, y_advance, x_offset, y_offset]. ShaperTest.cpp
shapes the same cases and compares glyph for glyph.
"""
import json
import random
import sys
import zlib

import uharfbuzz as hb

CORPUS = [
    # Latin: ligatures, kerning, accents precomposed and decomposed.
    "office", "affluent fjord", "AVAWATo", "Tr.Ty,Yo LT", "Hello, World!",
    "The quick brown fox jumps over the lazy dog.", "fi fl ffi ffl ff",
    "\u00e9\u00e8\u00ea\u00eb", "e\u0301e\u0300e\u0302e\u0308", "a\u0308\u0301", "o\u0323\u0302",
    "q\u0307\u0323", "x\u0301\u0302\u0303\u0304\u0308", "A\u030a", "\u212b", "\u1e69", "s\u0323\u0307",
    "i\u0307\u0301", "\u0131\u0308", "n\u0303", "\u01fa", "g\u0306", "\u1e0b\u0323", "a\u0338",
    "a\u0335b\u0336", "\u0301", "\u0301\u0301a", "e\u0301\u0301", "a\u20dd", "a\u034fe",
    "a\u0316\u034f\u0301", "c\u0327\u0301",
    # Spaces and ignorables.
    "a b\u00a0c\u2002d\u2003e\u2009f\u200ag\u202fh\u205fi\u3000j\u2007k\u2008l",
    "a\u200bb\u200cc\u200dd\u2060e\u00adf\ufeffg", "a\u2011b", "a\u180eb", "\u115fa\u1160",
    "x\ufe0fy", "\u2764\ufe0f", "\u263a\ufe0e", "a\U000e0061b", "\U0001f1e8\U0001f1e6",
    "\U0001f44d\U0001f3fd", "\U0001f468\u200d\U0001f469\u200d\U0001f467",
    # Greek, Cyrillic, Armenian, Georgian.
    "\u039a\u03b1\u03bb\u03b7\u03bc\u03ad\u03c1\u03b1 \u03ba\u03cc\u03c3\u03bc\u03b5",
    "\u03b1\u0313\u0301\u0345", "\u1f04", "\u03c9\u0342\u0345",
    "\u041f\u0440\u0438\u0432\u0435\u0442, \u043c\u0438\u0440!", "\u0438\u0306 \u0435\u0308",
    "\u0535\u0580\u0587\u0561\u0576 \ufb13", "\u10e5\u10d0\u10e0\u10d7\u10e3\u10da\u10d8",
    # Arabic.
    "\u0633\u0644\u0627\u0645", "\u0627\u0644\u0639\u0631\u0628\u064a\u0629",
    "\u0628\u0650\u0633\u0652\u0645\u0650 \u0627\u0644\u0644\u0651\u0670\u0647\u0650",
    "\u0644\u0627 \u0644\u0623 \u0644\u0625 \u0644\u0622", "\u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645",
    "\u0628\u200d", "\u200d\u0628", "\u0628\u200c\u0628", "\u0634\u0651\u064e", "\u0634\u064e\u0651",
    "\u0627\u0653", "\u0627\u0654\u064f", "\u0643\u062a\u0627\u0628 (\u0661\u0662\u0663)", "\u06cc\u06a9\u06af",
    "\u0640\u0628\u0640", "\u0647\u0670\u0630\u0627", "[\u0639]", "\u0628\u0655\u0650",
    # Hebrew.
    "\u05e9\u05dc\u05d5\u05dd", "\u05e9\u05c1\u05b8\u05dc\u05d5\u05b9\u05dd", "\u05d1\u05bc\u05b0",
    "\u05d9\u05b4", "\u05e9\u05bc\u05c1", "\u05d0\u05b7", "\u05d5\u05b9", "\u05b0\u05bc",
    "\u05d1\u05b0\u05bc\u05b8", "\u05d4\u05b7\u05bc\u05d9\u05bc\u05b8\u05dd",
    # Thai, Lao, others DejaVu covers. (Not NKo or the other scripts of the Universal Shaping Engine,
    # which the shaper does not implement yet.)
    "\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35", "\u0e01\u0e33", "\u0e9e\u0eb2\u0eaa\u0eb2\u0ea5\u0eb2\u0ea7",
    "\u13a0\u13a1\u13a2", "\u2d30\u2d31\u2d32", "\u1401\u1402", "\u2801\u2803", "\u16a0\u16a2",
    "\u3042\u30a2\u6f22", "\u2200x\u2208\u211d", "1/2 \u00bd \u2153",
    # Bidi-irrelevant but mirrored in RTL runs.
    "(a[b]c)",
    # Fractions, digits in right-to-left scripts, emoji, variation sequences.
    "1\u20442 3\u204416 x\u2044y 12\u2044", "\u0661\u0662\u0663", "\u0627 \u0661\u0662",
    "\U0001f600\U0001f3fb", "\u270c\ufe0f\u270c\ufe0e", "\u845b\U000e0100\u845b\U000e0101",
    "\u0e01\u0e33\u0e14\u0e4b\u0e33 \u0e1b\u0e34\u0e48 \u0e0d\u0e38 \u0e0e\u0e39\u0e48",
    "\u0eab\u0ecd\u0eb2 \u0e81\u0eb3",
]

POOLS = {
    "Latn": "abcdefghijklmnopqrstuvwxyzAVWTYLfi .,\u00e9\u00fc\u00f1" + "\u0300\u0301\u0302\u0303\u0308\u0323\u0327\u0331\u034f\u200c\u200d\u00ad",
    "Grek": "\u03b1\u03b2\u03b3\u03b4\u03b5\u03b7\u03b9\u03bf\u03c5\u03c9\u0391\u0395 " + "\u0300\u0301\u0313\u0314\u0342\u0345\u0308",
    "Cyrl": "\u0430\u0431\u0432\u0433\u0434\u0435\u0438\u043e\u0443\u044f\u0410\u0413 " + "\u0306\u0308\u0301",
    "Arab": "\u0627\u0628\u062a\u062c\u062d\u062f\u0631\u0633\u0639\u0644\u0645\u0646\u0647\u0648\u064a\u0623\u0622\u0640\u06cc\u06a9 \u200c\u200d"
    + "\u064b\u064c\u064d\u064e\u064f\u0650\u0651\u0652\u0653\u0654\u0655\u0670",
    "Hebr": "\u05d0\u05d1\u05d3\u05d4\u05d5\u05d9\u05db\u05dc\u05de\u05e9\u05ea\u05e4 "
    + "\u05b0\u05b1\u05b4\u05b5\u05b7\u05b8\u05b9\u05bb\u05bc\u05bf\u05c1\u05c2",
}

FEATURES = [
    ("office", [["liga", 0, 0, 0xFFFFFFFF]]),
    ("office affine", [["liga", 0, 0, 3]]),
    ("AVAWATo", [["kern", 0, 0, 0xFFFFFFFF]]),
    ("AVAWATo", [["kern", 0, 2, 4]]),
    ("office", [["dlig", 1, 0, 0xFFFFFFFF]]),
    ("abc 123", [["salt", 1, 0, 0xFFFFFFFF]]),
    ("abc ABC", [["case", 1, 0, 0xFFFFFFFF]]),
    ("\u0633\u0644\u0627\u0645", [["rlig", 0, 0, 0xFFFFFFFF]]),
    ("\u0633\u0644\u0627\u0645", [["init", 0, 0, 0xFFFFFFFF], ["fina", 0, 0, 0xFFFFFFFF]]),
    ("office", [["aalt", 2, 0, 0xFFFFFFFF]]),
    ("office", [["aalt", 255, 0, 0xFFFFFFFF]]),
    ("office affine", [["liga", 0, 0, 0xFFFFFFFF], ["liga", 1, 2, 4], ["liga", 0, 3, 9]]),
    ("abc", [["smcp", 1, 1, 2], ["c2sc", 1, 0, 0xFFFFFFFF]]),
    ("1/2 3/4", [["frac", 1, 0, 0xFFFFFFFF]]),
    ("0123", [["onum", 1, 0, 0xFFFFFFFF], ["tnum", 1, 0, 0xFFFFFFFF], ["zero", 1, 0, 0xFFFFFFFF]]),
]


def shape(font, text, features=None, direction=None, script=None, language=None):
    buf = hb.Buffer()
    buf.add_str(text)
    buf.guess_segment_properties()
    if direction:
        buf.direction = direction
    if script:
        buf.script = script
    # An OpenType language system tag, or one no font has: the default language system.
    buf.language = "x-hbot" + (language or "zzzz").strip().lower()
    hb_features = {}
    for tag, value, start, end in features or []:
        hb_features.setdefault(tag, []).append((start, min(end, 0xFFFFFFFF), value))
    hb.shape(font, buf, hb_features)
    glyphs = [[i.codepoint, i.cluster, p.x_advance, p.y_advance, p.x_offset, p.y_offset]
              for i, p in zip(buf.glyph_infos, buf.glyph_positions)]
    case = {"text": [ord(c) for c in text], "script": buf.script, "direction": buf.direction,
            "features": features or [], "glyphs": glyphs}
    if language:
        case["language"] = language
    return case


# For CfwTestLayout.ttf (make_layout_font.py): text reaching every rule of its tables.
LAYOUT_CORPUS = [
    "office", "affine fiffi", "fi\u0301 f\u0323i\u0301", "i\u0301 \u0131", "sop soq hsm sox", "zxz yffy",
    "gck jdl gcl", "tu wv tv", "pu qw pw", "abc bca cab ijq qri", "bkq ckr zkx yly", "rkb skc", "xpq", "zlz",
    "beb ceg be dg", "q", "AVAWAT To La Ya Lo Te Ve We", "fbdgj", "hklmnp", "ab ba", "cad ean", "xab yac",
    "kno lnk", "zzz", "mnu mnum", "a\u0301\u0300 a\u0302\u0303\u0301 a\u0323\u0328 a\u0327\u0301",
    "a\u0307\u0301", "o\u0301\u0323\u0300\u0328", "f_i\u0301", "fi\u0300\u0301", "ffi\u0301\u0300\u0302",
    "a\u034f\u0301 a\u200d\u0301 a\u200cb", "1\u20442 12\u20440 2\u2044",
    "\u0628\u062a\u0645", "\u0644\u0627", "\u0628\u0644\u0627", "\u0644\u0627\u064e\u0650",
    "\u0628\u0651\u064e\u062a\u0650\u0645", "\u0646\u0628\u062a\u0644\u0645\u0647\u064a",
    "\u0628\u0640\u0628", "\u0628\u200d\u0628", "\u0628\u200c\u0628", "\u0631\u0628\u0627\u0628",
    "\u0645\u0651\u064f\u0652", "(\u0628)",
    "\u0391\u03a4 \u03a4\u03b1 \u03a5\u03b1 \u03b1\u03b2 \u03b3\u03b4 \u03b2\u03b1 \u03b2\u03b4 \u03b3\u03b1",
    "\u03b1\u0301\u03b2",
]
LAYOUT_FEATURES = [
    ("abc aaa", [["salt", 1, 0, 0xFFFFFFFF]]), ("aaa", [["salt", 2, 0, 0xFFFFFFFF]]),
    ("aaa", [["salt", 3, 0, 0xFFFFFFFF]]), ("a a a", [["salt", 2, 2, 3]]),
    ("rrrrrr rr", []), ("rr", [["rand", 0, 0, 0xFFFFFFFF]]), ("rr", [["rand", 2, 0, 0xFFFFFFFF]]),
    ("bec", [["ss01", 1, 0, 0xFFFFFFFF]]), ("qqq", [["ss02", 1, 0, 0xFFFFFFFF]]),
    ("mnu mmn", [["ss03", 1, 0, 0xFFFFFFFF]]), ("zaz", [["ss04", 1, 0, 0xFFFFFFFF]]),
    ("hklmnp", [["ss05", 1, 0, 0xFFFFFFFF]]), ("acbe abef xab yac kno lnk", [["ss06", 1, 0, 0xFFFFFFFF]]),
    ("office", [["liga", 0, 0, 0xFFFFFFFF]]), ("AVAT", [["kern", 0, 0, 0xFFFFFFFF]]),
    ("sop tu", [["calt", 0, 0, 0xFFFFFFFF], ["clig", 0, 0, 0xFFFFFFFF]]), ("i", [["ccmp", 0, 0, 0xFFFFFFFF]]),
    ("a\u0301", [["mark", 0, 0, 0xFFFFFFFF]]), ("a\u0301\u0300", [["mkmk", 0, 0, 0xFFFFFFFF]]),
    ("12 1/2", [["numr", 1, 0, 0xFFFFFFFF]]), ("12", [["dnom", 1, 1, 2]]),
    ("\u0628\u062a\u0645", [["curs", 0, 0, 0xFFFFFFFF]]), ("\u0644\u0627", [["rlig", 0, 0, 0xFFFFFFFF]]),
    ("\u0628\u062a\u0645", [["init", 0, 0, 0xFFFFFFFF]]),
]
LAYOUT_POOLS = {
    "Latn": "abcdefghijklmnopqrstuvwxyzAVTWLY fi" + "\u0300\u0301\u0302\u0303\u0307\u0323\u0327\u0328\u034f\u200c\u200d",
    "Arab": "\u0627\u0628\u062a\u0644\u0645\u0646\u0647\u0631\u064a\u0640 \u200c\u200d" + "\u064e\u064f\u0650\u0651\u0652",
    "Grek": "\u03b1\u03b2\u03b3\u03b4\u0391\u03a4\u03a5 \u0301",
    "Zyyy": "012/\u2044 .,-()",
}


def main():
    font_path, out_path = sys.argv[1], sys.argv[2]
    n_random = int(sys.argv[sys.argv.index("--random") + 1]) if "--random" in sys.argv else 0
    seed = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 1
    blob = hb.Blob.from_file_path(font_path)
    face = hb.Face(blob)
    font = hb.Font(face)
    layout = "--layout" in sys.argv
    corpus, features_list, pools = (LAYOUT_CORPUS, LAYOUT_FEATURES, LAYOUT_POOLS) if layout else (CORPUS, FEATURES, POOLS)
    cases = [shape(font, t) for t in corpus]
    if layout:
        cases += [shape(font, t, language="TRK") for t in ("iki kiki", "office \u0131i")]
        cases += [shape(font, t, direction="rtl") for t in ("mnu AVA fi", "\u0628\u062a\u0645 12")]
        cases += [shape(font, t, direction="ltr") for t in ("\u0628\u062a\u0645", "\u0644\u0627\u064e")]
    cases.append(shape(font, "(a[b]c) \u00ab\u2264\u00bb", direction="rtl"))
    cases.append(shape(font, "\u0633\u0644\u0627\u0645", direction="ltr"))
    cases += [shape(font, t, f) for t, f in features_list]
    rng = random.Random(seed)
    scripts = sorted(pools)
    for _ in range(n_random):
        s = rng.choice(scripts)
        pool = pools[s]
        text = "".join(rng.choice(pool) for _ in range(rng.randint(1, 12)))
        cases.append(shape(font, text, script=s))
    data = json.dumps({"font": font_path.rsplit("/", 1)[-1], "harfbuzz": hb.__version__, "cases": cases},
                      separators=(",", ":")) + "\n"
    with open(out_path, "wb") as f:  # zlib-compressed if the name ends in .z
        f.write(zlib.compress(data.encode(), 9) if out_path.endswith(".z") else data.encode())


if __name__ == "__main__":
    main()
