#include "OtLayout.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <cmath>
#include <cstring>

#include "cfw/text/Unicode.h"

namespace cfw::ot {

namespace {

constexpr std::uint32_t kNotCovered = 0xFFFFFFFFu;
constexpr int kMaxNesting = 64;
constexpr std::size_t kMaxContextLength = 64;

// Big-endian reads that return 0 outside the table.
struct Data {
    const std::uint8_t *p = nullptr;
    std::size_t n = 0;

    Data() = default;
    explicit Data(Span<const std::byte> t) : p(reinterpret_cast<const std::uint8_t *>(t.data())), n(t.size()) {}
    std::uint16_t u16(std::size_t o) const noexcept {
        return o + 2 <= n ? static_cast<std::uint16_t>(p[o] << 8 | p[o + 1]) : 0;
    }
    std::int16_t i16(std::size_t o) const noexcept { return static_cast<std::int16_t>(u16(o)); }
    std::uint8_t u8(std::size_t o) const noexcept { return o < n ? p[o] : 0; }
    std::uint32_t u32(std::size_t o) const noexcept {
        return o + 4 <= n ? static_cast<std::uint32_t>(p[o]) << 24 | static_cast<std::uint32_t>(p[o + 1]) << 16 |
                                static_cast<std::uint32_t>(p[o + 2]) << 8 | p[o + 3]
                          : 0;
    }
};

std::uint32_t coverageIndex(const Data &d, std::uint32_t cov, GlyphId g) {
    if (cov == 0 || cov >= d.n) {
        return kNotCovered;
    }
    const std::uint16_t format = d.u16(cov);
    if (format == 1) {
        std::uint32_t lo = 0;
        std::uint32_t hi = d.u16(cov + 2);
        while (lo < hi) {
            const std::uint32_t mid = (lo + hi) / 2;
            const std::uint16_t v = d.u16(cov + 4 + mid * 2);
            if (v < g) {
                lo = mid + 1;
            } else if (v > g) {
                hi = mid;
            } else {
                return mid;
            }
        }
    } else if (format == 2) {
        std::uint32_t lo = 0;
        std::uint32_t hi = d.u16(cov + 2);
        while (lo < hi) {
            const std::uint32_t mid = (lo + hi) / 2;
            const std::uint32_t r = cov + 4 + mid * 6;
            if (d.u16(r + 2) < g) {
                lo = mid + 1;
            } else if (d.u16(r) > g) {
                hi = mid;
            } else {
                return static_cast<std::uint32_t>(d.u16(r + 4)) + g - d.u16(r);
            }
        }
    }
    return kNotCovered;
}

std::uint16_t classOf(const Data &d, std::uint32_t cd, GlyphId g) {
    if (cd == 0 || cd >= d.n) {
        return 0;
    }
    const std::uint16_t format = d.u16(cd);
    if (format == 1) {
        const std::uint16_t start = d.u16(cd + 2);
        const std::uint16_t count = d.u16(cd + 4);
        return g >= start && g - start < count ? d.u16(cd + 6 + 2u * (g - start)) : 0;
    }
    if (format == 2) {
        std::uint32_t lo = 0;
        std::uint32_t hi = d.u16(cd + 2);
        while (lo < hi) {
            const std::uint32_t mid = (lo + hi) / 2;
            const std::uint32_t r = cd + 4 + mid * 6;
            if (d.u16(r + 2) < g) {
                lo = mid + 1;
            } else if (d.u16(r) > g) {
                hi = mid;
            } else {
                return d.u16(r + 4);
            }
        }
    }
    return 0;
}

int popcount16(std::uint16_t v) {
    int n = 0;
    for (; v; v &= static_cast<std::uint16_t>(v - 1)) {
        ++n;
    }
    return n;
}

} // namespace

// ---- Buffer ----

void Buffer::clearOutput() {
    haveOutput = true;
    out.clear();
}

void Buffer::swapBuffers() {
    if (haveOutput) {
        // Anything not yet consumed follows the output.
        out.insert(out.end(), info.begin() + static_cast<std::ptrdiff_t>(idx), info.end());
        info.swap(out);
        out.clear();
    }
    haveOutput = false;
    idx = 0;
}

void Buffer::nextGlyph() {
    if (haveOutput) {
        out.push_back(info[idx]);
    }
    ++idx;
}

void Buffer::replaceGlyph(GlyphId g) {
    GlyphInfo copy = info[idx];
    copy.glyph = g;
    if (haveOutput) {
        out.push_back(copy);
    } else {
        info[idx] = copy;
    }
    ++idx;
}

void Buffer::outputGlyph(GlyphId g) {
    if (out.size() + (info.size() - idx) >= maxLen) {
        maxOps = -1; // too long: shaping stops here
        return;
    }
    GlyphInfo copy = info[idx];
    copy.glyph = g;
    out.push_back(copy);
}

void Buffer::deleteGlyph() {
    // The cluster survives in a neighbour: merge it backward or forward.
    const std::uint32_t cluster = info[idx].cluster;
    if ((idx + 1 < info.size() && cluster == info[idx + 1].cluster) || (!out.empty() && cluster == out.back().cluster)) {
        skipGlyph();
        return;
    }
    if (!out.empty()) {
        if (cluster < out.back().cluster) {
            const std::uint32_t old = out.back().cluster;
            for (std::size_t i = out.size(); i > 0 && out[i - 1].cluster == old; --i) {
                out[i - 1].cluster = cluster;
            }
        }
        skipGlyph();
        return;
    }
    if (idx + 1 < info.size()) {
        mergeClusters(idx, idx + 2);
    }
    skipGlyph();
}

bool Buffer::moveTo(std::size_t i) {
    if (!haveOutput) {
        if (i > info.size()) {
            return false;
        }
        idx = i;
        return true;
    }
    if (i > out.size() + (info.size() - idx)) {
        return false;
    }
    if (out.size() < i) {
        const std::size_t count = i - out.size();
        maxOps -= static_cast<int>(count);
        out.insert(out.end(), info.begin() + static_cast<std::ptrdiff_t>(idx),
                   info.begin() + static_cast<std::ptrdiff_t>(idx + count));
        idx += count;
    } else if (out.size() > i) {
        const std::size_t count = out.size() - i;
        if (idx < count) {
            // Make room at the front of the input.
            const std::size_t shift = count - idx;
            maxOps -= static_cast<int>(info.size() - idx);
            info.insert(info.begin() + static_cast<std::ptrdiff_t>(idx), shift, GlyphInfo{});
            idx += shift;
        }
        maxOps -= static_cast<int>(count);
        idx -= count;
        std::copy(out.end() - static_cast<std::ptrdiff_t>(count), out.end(), info.begin() + static_cast<std::ptrdiff_t>(idx));
        out.resize(i);
    }
    return true;
}

void Buffer::mergeClusters(std::size_t start, std::size_t end) {
    if (end - start < 2) {
        return;
    }
    maxOps -= static_cast<int>(end - start);
    std::uint32_t cluster = info[start].cluster;
    for (std::size_t i = start + 1; i < end; ++i) {
        cluster = std::min(cluster, info[i].cluster);
    }
    // Extend end and start to whole clusters.
    if (cluster != info[end - 1].cluster) {
        while (end < info.size() && info[end - 1].cluster == info[end].cluster) {
            ++end;
        }
    }
    if (cluster != info[start].cluster) {
        while (idx < start && info[start - 1].cluster == info[start].cluster) {
            --start;
        }
    }
    // Hitting the start of the input: continue in the output.
    if (idx == start && info[start].cluster != cluster) {
        const std::uint32_t old = info[start].cluster;
        for (std::size_t i = out.size(); i > 0 && out[i - 1].cluster == old; --i) {
            out[i - 1].cluster = cluster;
        }
    }
    for (std::size_t i = start; i < end; ++i) {
        info[i].cluster = cluster;
    }
}

void Buffer::mergeOutClusters(std::size_t start, std::size_t end) {
    if (end - start < 2) {
        return;
    }
    maxOps -= static_cast<int>(end - start);
    std::uint32_t cluster = out[start].cluster;
    for (std::size_t i = start + 1; i < end; ++i) {
        cluster = std::min(cluster, out[i].cluster);
    }
    // Extend start and end.
    while (start > 0 && out[start - 1].cluster == out[start].cluster) {
        --start;
    }
    while (end < out.size() && out[end - 1].cluster == out[end].cluster) {
        ++end;
    }
    // Hitting the end of the output: continue in the input.
    if (end == out.size()) {
        for (std::size_t i = idx; i < info.size() && info[i].cluster == out[end - 1].cluster; ++i) {
            info[i].cluster = cluster;
        }
    }
    for (std::size_t i = start; i < end; ++i) {
        out[i].cluster = cluster;
    }
}

void Buffer::reverseRange(std::size_t start, std::size_t end) {
    std::reverse(info.begin() + static_cast<std::ptrdiff_t>(start), info.begin() + static_cast<std::ptrdiff_t>(end));
    if (pos.size() >= end) {
        std::reverse(pos.begin() + static_cast<std::ptrdiff_t>(start), pos.begin() + static_cast<std::ptrdiff_t>(end));
    }
}

std::uint8_t Buffer::allocateLigId() {
    std::uint8_t id = static_cast<std::uint8_t>(++serial & 7u);
    if (id == 0) {
        id = static_cast<std::uint8_t>(++serial & 7u);
    }
    return id;
}

// ---- GDEF ----

void Gdef::init(const FontFace &face) {
    m_table = face.table(FontFace::tag("GDEF"));
    const Data d(m_table);
    if (d.n < 12) {
        m_classDef = m_markAttachClassDef = m_markGlyphSets = 0;
        return;
    }
    m_classDef = d.u16(4);
    m_markAttachClassDef = d.u16(10);
    const std::uint32_t version = d.u32(0);
    m_markGlyphSets = (version >= 0x00010002 && d.n >= 14) ? d.u16(12) : 0;
}

std::uint16_t Gdef::glyphProps(GlyphId g) const noexcept {
    const Data d(m_table);
    switch (classOf(d, m_classDef, g)) {
    case 1: return kBaseGlyph;
    case 2: return kLigature;
    case 3: return static_cast<std::uint16_t>(kMark | (classOf(d, m_markAttachClassDef, g) << 8));
    default: return 0;
    }
}

bool Gdef::markSetCovers(std::uint32_t set, GlyphId g) const noexcept {
    const Data d(m_table);
    if (m_markGlyphSets == 0 || set >= d.u16(m_markGlyphSets + 2)) {
        return false;
    }
    const std::uint32_t coverage = m_markGlyphSets + d.u32(m_markGlyphSets + 4 + set * 4);
    return coverageIndex(d, coverage, g) != kNotCovered;
}

// ---- GSUB/GPOS: script, language, features ----

void LayoutTable::init(const FontFace &face, TableKind kind) {
    init(face.table(FontFace::tag(kind == TableKind::Gsub ? "GSUB" : "GPOS")), kind);
}

void LayoutTable::init(Span<const std::byte> table, TableKind kind) {
    m_kind = kind;
    m_table = table;
    const Data d(m_table);
    if (d.n < 10) {
        m_table = {};
        return;
    }
    m_scriptList = d.u16(4);
    m_featureList = d.u16(6);
    m_lookupList = d.u16(8);
}

void LayoutTable::selectScript(Span<const std::uint32_t> scriptTags, std::uint32_t language) {
    m_langSys = 0;
    m_chosenScript = 0;
    m_foundScript = false;
    const Data d(m_table);
    if (!present() || m_scriptList == 0) {
        return;
    }
    const std::uint16_t count = d.u16(m_scriptList);
    const auto findScript = [&](std::uint32_t tag) -> std::uint32_t {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t rec = m_scriptList + 2 + i * 6;
            if (d.u32(rec) == tag) {
                return m_scriptList + d.u16(rec + 4);
            }
        }
        return 0;
    };
    std::uint32_t script = 0;
    for (const std::uint32_t tag : scriptTags) {
        if ((script = findScript(tag)) != 0) {
            m_chosenScript = tag;
            m_foundScript = true;
            break;
        }
    }
    for (const std::uint32_t fallback : {FontFace::tag("DFLT"), FontFace::tag("dflt"), FontFace::tag("latn")}) {
        if (script == 0 && (script = findScript(fallback)) != 0) {
            m_chosenScript = fallback;
        }
    }
    if (script == 0) {
        return;
    }
    if (language != 0) {
        const std::uint16_t langCount = d.u16(script + 2);
        for (std::uint32_t i = 0; i < langCount; ++i) {
            const std::uint32_t rec = script + 4 + i * 6;
            if (d.u32(rec) == language) {
                m_langSys = script + d.u16(rec + 4);
                return;
            }
        }
    }
    const std::uint16_t def = d.u16(script);
    m_langSys = def != 0 ? script + def : 0;
}

std::uint32_t LayoutTable::requiredFeatureTag() const noexcept {
    const Data d(m_table);
    if (m_langSys == 0) {
        return 0;
    }
    const std::uint16_t required = d.u16(m_langSys + 2);
    if (required == 0xFFFF || required >= d.u16(m_featureList)) {
        return 0;
    }
    return d.u32(m_featureList + 2 + required * 6u);
}

void LayoutTable::lookupsOf(std::uint32_t featureIndex, std::vector<std::uint16_t> &lookups) const {
    lookups.clear();
    const Data d(m_table);
    if (featureIndex >= d.u16(m_featureList)) {
        return;
    }
    const std::uint32_t f = m_featureList + d.u16(m_featureList + 2 + featureIndex * 6 + 4);
    const std::uint16_t n = d.u16(f + 2);
    const std::uint32_t count = lookupCount();
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint16_t lookup = d.u16(f + 4 + k * 2);
        if (lookup < count) {
            lookups.push_back(lookup);
        }
    }
}

bool LayoutTable::featureLookups(std::uint32_t feature, std::vector<std::uint16_t> &lookups) const {
    lookups.clear();
    const Data d(m_table);
    if (m_langSys == 0) {
        return false;
    }
    // The first of the language system's features with the tag (the
    // required feature is not among them; see requiredFeatureLookups()).
    const std::uint16_t featureCount = d.u16(m_featureList);
    const std::uint16_t n = d.u16(m_langSys + 4);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint16_t index = d.u16(m_langSys + 6 + i * 2);
        if (index < featureCount && d.u32(m_featureList + 2 + index * 6u) == feature) {
            lookupsOf(index, lookups);
            return true;
        }
    }
    return false;
}

void LayoutTable::requiredFeatureLookups(std::vector<std::uint16_t> &lookups) const {
    lookups.clear();
    const Data d(m_table);
    if (m_langSys != 0 && d.u16(m_langSys + 2) != 0xFFFF) {
        lookupsOf(d.u16(m_langSys + 2), lookups);
    }
}

std::uint32_t LayoutTable::lookupCount() const noexcept {
    const Data d(m_table);
    return m_lookupList ? d.u16(m_lookupList) : 0;
}

// ---- Applying lookups ----

namespace {

struct Context;

// HarfBuzz's skipping iterator.
struct Iterator {
    enum class MatchKind : std::uint8_t { None, Glyph, Class, Coverage };
    enum Skip { SkipNo, SkipYes, SkipMaybe };
    enum Match { MatchNo, MatchYes, MatchMaybe };

    Context *c = nullptr;
    std::size_t idx = 0;
    unsigned numItems = 0;
    std::size_t end = 0;
    std::uint32_t matchProps = 0;
    bool ignoreZwnj = false;
    bool ignoreZwj = false;
    std::uint32_t mask = 0;
    std::uint8_t syllable = 0; // match only this syllable (per-syllable lookups; 0: any)
    MatchKind kind = MatchKind::None;
    std::uint32_t data = 0;   // position of the next value to match (u16 array in the table)
    std::uint32_t base = 0;   // coverage offsets are relative to this
    std::uint32_t classDef = 0;

    void init(Context &context, bool contextMatch);
    void reset(std::size_t start, unsigned items);
    void setMatch(MatchKind k, std::uint32_t values, std::uint32_t valueBase, std::uint32_t cd) {
        kind = k;
        data = values;
        base = valueBase;
        classDef = cd;
    }
    Skip maySkip(const GlyphInfo &info) const;
    Match mayMatch(const GlyphInfo &info) const;
    bool next();
    bool prev();
};

struct Context {
    const FontFace &face;
    const Gdef &gdef;
    const LayoutTable &table;
    Data d;
    Buffer &buffer;
    TableKind kind;
    std::uint32_t lookupMask = 1;
    std::uint32_t lookupProps = 0;
    bool autoZwnj = true;
    bool perSyllable = false;
    bool autoZwj = true;
    bool random = false;
    int nestingLeft = kMaxNesting;
    bool hasGlyphClasses = false;
    int lastBase = -1;
    std::size_t lastBaseUntil = 0;
    Iterator input;
    Iterator context;

    Context(const FontFace &f, const Gdef &g, const LayoutTable &t, Buffer &b)
        : face(f), gdef(g), table(t), d(t.table()), buffer(b), kind(t.kind()), hasGlyphClasses(g.hasGlyphClasses()) {}

    bool checkGlyphProperty(const GlyphInfo &info, std::uint32_t matchProps) const {
        const std::uint16_t props = info.glyphProps;
        if (props & matchProps & 0x0E) { // IgnoreBaseGlyphs/Ligatures/Marks
            return false;
        }
        if (props & kMark) {
            if (matchProps & 0x10) { // UseMarkFilteringSet
                return gdef.markSetCovers(matchProps >> 16, info.glyph);
            }
            if (matchProps & 0xFF00) { // MarkAttachmentType
                return (matchProps & 0xFF00) == (props & 0xFF00);
            }
        }
        return true;
    }

    void setGlyphClass(GlyphId glyph, std::uint16_t classGuess = 0, bool ligature = false, bool component = false) {
        GlyphInfo &cur = buffer.cur();
        std::uint16_t props = static_cast<std::uint16_t>(cur.glyphProps | kSubstituted);
        if (ligature) {
            props = static_cast<std::uint16_t>((props | kLigated) & ~kMultiplied);
        }
        if (component) {
            props |= kMultiplied;
        }
        if (hasGlyphClasses) {
            cur.glyphProps = static_cast<std::uint16_t>((props & kPreserve) | gdef.glyphProps(glyph));
        } else if (classGuess) {
            cur.glyphProps = static_cast<std::uint16_t>((props & kPreserve) | classGuess);
        } else {
            cur.glyphProps = props;
        }
    }
    void replaceGlyph(GlyphId g) {
        setGlyphClass(g);
        buffer.replaceGlyph(g);
    }
    void replaceGlyphInPlace(GlyphId g) {
        setGlyphClass(g);
        buffer.cur().glyph = g;
    }
    void replaceGlyphWithLigature(GlyphId g, std::uint16_t klass) {
        setGlyphClass(g, klass, true);
        buffer.replaceGlyph(g);
    }
    void outputGlyphForComponent(GlyphId g, std::uint16_t klass) {
        setGlyphClass(g, klass, false, true);
        buffer.outputGlyph(g);
    }

    bool applyLookup(std::uint16_t index); // the lookup's subtables at the current glyph
    bool recurse(std::uint16_t index) {
        if (nestingLeft == 0 || buffer.maxOps-- <= 0) {
            return false;
        }
        --nestingLeft;
        const std::uint32_t savedProps = lookupProps;
        const bool ok = applyLookup(index);
        lookupProps = savedProps;
        input.init(*this, false); // back to the outer lookup's properties
        context.init(*this, true);
        ++nestingLeft;
        return ok;
    }
    std::uint32_t lookupOffset(std::uint16_t index) const {
        const std::uint32_t list = table.lookupList();
        if (index >= d.u16(list)) {
            return 0;
        }
        return list + d.u16(list + 2 + index * 2u);
    }
    std::uint32_t lookupPropsOf(std::uint32_t lookup) const {
        const std::uint16_t flag = d.u16(lookup + 2);
        std::uint32_t props = flag;
        if (flag & 0x10) {
            props |= static_cast<std::uint32_t>(d.u16(lookup + 6 + 2u * d.u16(lookup + 4))) << 16;
        }
        return props;
    }

    bool applySubtable(std::uint16_t type, std::uint32_t st);
    bool applyGsub(std::uint16_t type, std::uint32_t st);
    bool applyGpos(std::uint16_t type, std::uint32_t st);

    // Context matching (GSUB 5/6, GPOS 7/8).
    bool matchInput(unsigned count, Iterator::MatchKind kind, std::uint32_t values, std::uint32_t valueBase,
                    std::uint32_t classDef, std::size_t &matchEnd, std::size_t *positions, unsigned *totalComponents);
    bool matchBacktrack(unsigned count, Iterator::MatchKind kind, std::uint32_t values, std::uint32_t valueBase,
                        std::uint32_t classDef);
    bool matchLookahead(unsigned count, Iterator::MatchKind kind, std::uint32_t values, std::uint32_t valueBase,
                        std::uint32_t classDef, std::size_t start);
    void applyNested(unsigned count, std::size_t *positions, unsigned lookupCount, std::uint32_t records,
                     std::size_t matchEnd);
    bool ligate(unsigned count, const std::size_t *positions, std::size_t matchEnd, GlyphId lig, unsigned totalComponents);

    bool applyValue(std::uint16_t format, std::uint32_t values, GlyphPosition &pos) const;
    void anchor(std::uint32_t at, float &x, float &y) const {
        x = d.i16(at + 2);
        y = d.i16(at + 4);
    }
    bool markArrayApply(std::uint32_t markArray, std::uint32_t markIndex, std::uint32_t glyphIndex,
                        std::uint32_t anchors, std::uint32_t anchorBase, unsigned classCount, std::size_t glyphPos);
};

void Iterator::init(Context &context, bool contextMatch) {
    c = &context;
    kind = MatchKind::None;
    data = 0;
    matchProps = c->lookupProps;
    ignoreZwnj = c->kind == TableKind::Gpos || (contextMatch && c->autoZwnj);
    ignoreZwj = contextMatch || c->autoZwj;
    mask = contextMatch ? 0xFFFFFFFFu : c->lookupMask;
}

void Iterator::reset(std::size_t start, unsigned items) {
    idx = start;
    numItems = items;
    end = c->buffer.len();
    syllable = c->perSyllable && c->buffer.idx < c->buffer.len() ? c->buffer.cur().syllable : 0;
}

Iterator::Skip Iterator::maySkip(const GlyphInfo &info) const {
    if (!c->checkGlyphProperty(info, matchProps)) {
        return SkipYes;
    }
    // Hidden ignorables (CGJ, tags, Mongolian selectors) are skipped in GPOS only.
    if ((info.flags & kIgnorable) && !isSubstituted(info) && (ignoreZwnj || !(info.flags & kZwnj)) &&
        (ignoreZwj || !(info.flags & kZwj)) && (c->kind == TableKind::Gpos || !(info.flags & kHidden))) {
        return SkipMaybe;
    }
    return SkipNo;
}

Iterator::Match Iterator::mayMatch(const GlyphInfo &info) const {
    if (!(info.mask & mask) || (syllable != 0 && info.syllable != syllable)) {
        return MatchNo;
    }
    if (kind == MatchKind::None) {
        return MatchMaybe;
    }
    const Data &d = c->d;
    const std::uint16_t value = d.u16(data);
    bool ok = false;
    switch (kind) {
    case MatchKind::Glyph: ok = info.glyph == value; break;
    case MatchKind::Class: ok = classOf(d, classDef, info.glyph) == value; break;
    case MatchKind::Coverage: ok = coverageIndex(d, base + value, info.glyph) != kNotCovered; break;
    case MatchKind::None: break;
    }
    return ok ? MatchYes : MatchNo;
}

bool Iterator::next() {
    while (idx + numItems < end) {
        ++idx;
        const GlyphInfo &info = c->buffer.info[idx];
        const Skip skip = maySkip(info);
        if (skip == SkipYes) {
            continue;
        }
        const Match match = mayMatch(info);
        if (match == MatchYes || (match == MatchMaybe && skip == SkipNo)) {
            --numItems;
            if (kind != MatchKind::None) {
                data += 2;
            }
            return true;
        }
        if (skip == SkipNo) {
            return false;
        }
    }
    return false;
}

bool Iterator::prev() {
    const Buffer &b = c->buffer;
    while (idx > numItems - 1 && idx > 0) {
        --idx;
        const GlyphInfo &info = b.haveOutput ? b.out[idx] : b.info[idx];
        const Skip skip = maySkip(info);
        if (skip == SkipYes) {
            continue;
        }
        const Match match = mayMatch(info);
        if (match == MatchYes || (match == MatchMaybe && skip == SkipNo)) {
            --numItems;
            if (kind != MatchKind::None) {
                data += 2;
            }
            return true;
        }
        if (skip == SkipNo) {
            return false;
        }
    }
    return false;
}

bool Context::matchInput(unsigned count, Iterator::MatchKind k, std::uint32_t values, std::uint32_t valueBase,
                         std::uint32_t cd, std::size_t &matchEnd, std::size_t *positions, unsigned *totalComponents) {
    if (count > kMaxContextLength) {
        return false;
    }
    Iterator &it = input;
    it.reset(buffer.idx, count - 1);
    it.setMatch(k, values, valueBase, cd);
    unsigned total = 0;
    const unsigned firstLigId = ligId(buffer.cur());
    const unsigned firstLigComp = ligComp(buffer.cur());
    enum { NotChecked, MayNotSkip, MaySkip } ligbase = NotChecked;
    positions[0] = buffer.idx;
    for (unsigned i = 1; i < count; ++i) {
        if (!it.next()) {
            return false;
        }
        positions[i] = it.idx;
        const GlyphInfo &g = buffer.info[it.idx];
        const unsigned thisLigId = ligId(g);
        const unsigned thisLigComp = ligComp(g);
        if (firstLigId && firstLigComp) {
            // Components must all attach to the same ligature component, unless the
            // ligature base they belong to is itself skipped.
            if (firstLigId != thisLigId || firstLigComp != thisLigComp) {
                if (ligbase == NotChecked) {
                    bool found = false;
                    std::size_t j = buffer.out.size();
                    while (j && ligId(buffer.out[j - 1]) == firstLigId) {
                        if (ligComp(buffer.out[j - 1]) == 0) {
                            --j;
                            found = true;
                            break;
                        }
                        --j;
                    }
                    ligbase = (found && it.maySkip(buffer.out[j]) == Iterator::SkipYes) ? MaySkip : MayNotSkip;
                }
                if (ligbase == MayNotSkip) {
                    return false;
                }
            }
        } else if (thisLigId && thisLigComp && thisLigId != firstLigId) {
            return false;
        }
        total += ligNumComps(g);
    }
    matchEnd = it.idx + 1;
    if (count == 1) {
        matchEnd = buffer.idx + 1;
    }
    if (totalComponents) {
        *totalComponents = total + ligNumComps(buffer.cur());
    }
    positions[0] = buffer.idx;
    return true;
}

bool Context::matchBacktrack(unsigned count, Iterator::MatchKind k, std::uint32_t values, std::uint32_t valueBase,
                             std::uint32_t cd) {
    Iterator &it = context;
    it.reset(buffer.backtrackLen(), count);
    it.setMatch(k, values, valueBase, cd);
    for (unsigned i = 0; i < count; ++i) {
        if (!it.prev()) {
            return false;
        }
    }
    return true;
}

bool Context::matchLookahead(unsigned count, Iterator::MatchKind k, std::uint32_t values, std::uint32_t valueBase,
                             std::uint32_t cd, std::size_t start) {
    Iterator &it = context;
    it.reset(start - 1, count);
    it.setMatch(k, values, valueBase, cd);
    for (unsigned i = 0; i < count; ++i) {
        if (!it.next()) {
            return false;
        }
    }
    return true;
}

void Context::applyNested(unsigned count, std::size_t *positions, unsigned lookupCount, std::uint32_t records,
                          std::size_t matchEnd) {
    // Positions become distances from the start of the output.
    const std::size_t bl = buffer.backtrackLen();
    std::ptrdiff_t end = static_cast<std::ptrdiff_t>(bl + matchEnd - buffer.idx);
    const std::ptrdiff_t shift = static_cast<std::ptrdiff_t>(bl) - static_cast<std::ptrdiff_t>(buffer.idx);
    for (unsigned j = 0; j < count; ++j) {
        positions[j] = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(positions[j]) + shift);
    }
    for (unsigned i = 0; i < lookupCount; ++i) {
        const unsigned index = d.u16(records + i * 4);
        const std::uint16_t lookup = d.u16(records + i * 4 + 2);
        if (index >= count) {
            continue;
        }
        const std::size_t origLen = buffer.backtrackLen() + buffer.lookaheadLen();
        if (positions[index] >= origLen) {
            continue;
        }
        if (!buffer.moveTo(positions[index])) {
            break;
        }
        if (buffer.maxOps <= 0) {
            break;
        }
        if (!recurse(lookup)) {
            continue;
        }
        const std::size_t newLen = buffer.backtrackLen() + buffer.lookaheadLen();
        std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(newLen) - static_cast<std::ptrdiff_t>(origLen);
        if (delta == 0) {
            continue;
        }
        end += delta;
        if (end < static_cast<std::ptrdiff_t>(positions[index])) {
            delta += static_cast<std::ptrdiff_t>(positions[index]) - end;
            end = static_cast<std::ptrdiff_t>(positions[index]);
        }
        unsigned next = index + 1;
        if (delta > 0) {
            if (static_cast<std::size_t>(delta) + count > kMaxContextLength) {
                break;
            }
        } else {
            delta = std::max<std::ptrdiff_t>(delta, static_cast<std::ptrdiff_t>(next) - static_cast<std::ptrdiff_t>(count));
            next = static_cast<unsigned>(static_cast<std::ptrdiff_t>(next) - delta);
        }
        // Shift the later positions.
        std::memmove(positions + static_cast<std::ptrdiff_t>(next) + delta, positions + next,
                     (count - next) * sizeof(std::size_t));
        next = static_cast<unsigned>(static_cast<std::ptrdiff_t>(next) + delta);
        count = static_cast<unsigned>(static_cast<std::ptrdiff_t>(count) + delta);
        for (unsigned j = index + 1; j < next; ++j) {
            positions[j] = positions[j - 1] + 1;
        }
        for (; next < count; ++next) {
            positions[next] = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(positions[next]) + delta);
        }
    }
    (void)buffer.moveTo(static_cast<std::size_t>(std::max<std::ptrdiff_t>(end, 0)));
}

bool Context::ligate(unsigned count, const std::size_t *positions, std::size_t matchEnd, GlyphId lig,
                     unsigned totalComponents) {
    buffer.mergeClusters(buffer.idx, matchEnd);
    bool isBaseLigature = isBaseGlyph(buffer.info[positions[0]]);
    bool isMarkLigature = isMark(buffer.info[positions[0]]);
    for (unsigned i = 1; i < count; ++i) {
        if (!isMark(buffer.info[positions[i]])) {
            isBaseLigature = false;
            isMarkLigature = false;
            break;
        }
    }
    const bool isLig = !isBaseLigature && !isMarkLigature;
    const std::uint16_t klass = isLig ? kLigature : 0;
    const unsigned id = isLig ? buffer.allocateLigId() : 0;
    unsigned lastLigId = ligId(buffer.cur());
    unsigned lastNumComponents = ligNumComps(buffer.cur());
    unsigned componentsSoFar = lastNumComponents;
    if (isLig) {
        setLigPropsForLigature(buffer.cur(), id, totalComponents);
        if (buffer.cur().generalCategory == static_cast<std::uint8_t>(unicode::GeneralCategory::Mn)) {
            buffer.cur().generalCategory = static_cast<std::uint8_t>(unicode::GeneralCategory::Lo);
        }
    }
    replaceGlyphWithLigature(lig, klass);
    for (unsigned i = 1; i < count; ++i) {
        while (buffer.idx < positions[i]) {
            if (isLig) {
                unsigned thisComp = ligComp(buffer.cur());
                if (thisComp == 0) {
                    thisComp = lastNumComponents;
                }
                const unsigned newComp = componentsSoFar - lastNumComponents + std::min(thisComp, lastNumComponents);
                setLigPropsForMark(buffer.cur(), id, newComp);
            }
            buffer.nextGlyph();
        }
        lastLigId = ligId(buffer.cur());
        lastNumComponents = ligNumComps(buffer.cur());
        componentsSoFar += lastNumComponents;
        buffer.skipGlyph(); // the component itself is gone
    }
    if (!isMarkLigature && lastLigId) {
        // Marks after the last component follow it into the new ligature.
        for (std::size_t i = buffer.idx; i < buffer.len(); ++i) {
            if (lastLigId != ligId(buffer.info[i])) {
                break;
            }
            const unsigned thisComp = ligComp(buffer.info[i]);
            if (!thisComp) {
                break;
            }
            const unsigned newComp = componentsSoFar - lastNumComponents + std::min(thisComp, lastNumComponents);
            setLigPropsForMark(buffer.info[i], id, newComp);
        }
    }
    return true;
}

bool Context::applyValue(std::uint16_t format, std::uint32_t v, GlyphPosition &pos) const {
    bool applied = false;
    const auto take = [&]() {
        const std::int16_t value = d.i16(v);
        v += 2;
        applied = applied || value != 0;
        return value;
    };
    if (format & 0x01) {
        pos.xOffset += take();
    }
    if (format & 0x02) {
        pos.yOffset += take();
    }
    if (format & 0x04) {
        pos.xAdvance += take(); // horizontal text only
    }
    if (format & 0x08) {
        (void)take(); // vertical advance: not used horizontally
    }
    return applied; // device tables (0x10-0x80) only matter for hinted sizes
}

bool Context::markArrayApply(std::uint32_t markArray, std::uint32_t markIndex, std::uint32_t glyphIndex,
                             std::uint32_t anchors, std::uint32_t anchorBase, unsigned classCount,
                             std::size_t glyphPos) {
    const std::uint32_t record = markArray + 2 + markIndex * 4;
    const unsigned markClass = d.u16(record);
    const std::uint32_t markAnchor = markArray + d.u16(record + 2);
    if (markClass >= classCount) {
        return false;
    }
    const std::uint16_t glyphAnchorOffset = d.u16(anchors + (glyphIndex * classCount + markClass) * 2u);
    if (glyphAnchorOffset == 0) {
        return false; // no anchor: let later subtables try
    }
    float markX = 0;
    float markY = 0;
    float baseX = 0;
    float baseY = 0;
    anchor(markAnchor, markX, markY);
    anchor(anchorBase + glyphAnchorOffset, baseX, baseY);
    // The base's cross-stream offset, including along its cursive chain.
    std::int32_t baseOffset = buffer.pos[glyphPos].yOffset;
    for (std::size_t k = glyphPos, steps = 0; (buffer.pos[k].attachType & 2) && buffer.pos[k].attachChain != 0 &&
                                              steps < buffer.pos.size();
         ++steps) {
        const std::ptrdiff_t parent = static_cast<std::ptrdiff_t>(k) + buffer.pos[k].attachChain;
        if (parent < 0 || static_cast<std::size_t>(parent) >= buffer.pos.size()) {
            break;
        }
        k = static_cast<std::size_t>(parent);
        baseOffset += buffer.pos[k].yOffset;
    }
    GlyphPosition &o = buffer.pos[buffer.idx];
    o.xOffset = static_cast<std::int32_t>(std::lround(baseX - markX));
    o.yOffset = static_cast<std::int32_t>(std::lround(baseY - markY)) + baseOffset;
    o.attachType = 1;
    o.attachChain = static_cast<std::int16_t>(static_cast<std::ptrdiff_t>(glyphPos) - static_cast<std::ptrdiff_t>(buffer.idx));
    ++buffer.idx;
    return true;
}

bool Context::applyLookup(std::uint16_t index) {
    const std::uint32_t lookup = lookupOffset(index);
    if (lookup == 0) {
        return false;
    }
    lookupProps = lookupPropsOf(lookup);
    input.init(*this, false);
    context.init(*this, true);
    std::uint16_t type = d.u16(lookup);
    const std::uint16_t count = d.u16(lookup + 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t st = lookup + d.u16(lookup + 6 + i * 2);
        std::uint16_t t = type;
        const bool extension = (kind == TableKind::Gsub && type == 7) || (kind == TableKind::Gpos && type == 9);
        if (extension) {
            t = d.u16(st + 2);
            st = st + d.u32(st + 4);
        }
        if (applySubtable(t, st)) {
            return true;
        }
    }
    return false;
}

bool Context::applySubtable(std::uint16_t type, std::uint32_t st) {
    if (st >= d.n) {
        return false;
    }
    return kind == TableKind::Gsub ? applyGsub(type, st) : applyGpos(type, st);
}

// Context and chaining context, shared by GSUB (5, 6) and GPOS (7, 8).
bool applyContext(Context &c, std::uint32_t st) {
    const Data &d = c.d;
    const std::uint16_t format = d.u16(st);
    const GlyphId g = c.buffer.cur().glyph;
    std::size_t positions[kMaxContextLength];
    std::size_t matchEnd = 0;
    if (format == 1 || format == 2) {
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered) {
            return false;
        }
        std::uint32_t cd = 0;
        std::uint32_t setIndex = index;
        std::uint32_t sets = st + 6;
        if (format == 2) {
            cd = st + d.u16(st + 4);
            setIndex = classOf(d, cd, g);
            sets = st + 8;
            if (setIndex >= d.u16(st + 6)) {
                return false;
            }
        } else if (setIndex >= d.u16(st + 4)) {
            return false;
        }
        const std::uint16_t setOffset = d.u16(sets + setIndex * 2);
        if (setOffset == 0) {
            return false;
        }
        const std::uint32_t set = st + setOffset;
        const std::uint16_t rules = d.u16(set);
        for (std::uint32_t r = 0; r < rules; ++r) {
            const std::uint32_t rule = set + d.u16(set + 2 + r * 2);
            const unsigned inputCount = d.u16(rule);
            const unsigned lookupCount = d.u16(rule + 2);
            if (inputCount == 0) {
                continue;
            }
            if (c.matchInput(inputCount, format == 1 ? Iterator::MatchKind::Glyph : Iterator::MatchKind::Class,
                             rule + 4, 0, cd, matchEnd, positions, nullptr)) {
                c.applyNested(inputCount, positions, lookupCount, rule + 4 + (inputCount - 1) * 2, matchEnd);
                return true;
            }
        }
        return false;
    }
    if (format == 3) {
        const unsigned inputCount = d.u16(st + 2);
        const unsigned lookupCount = d.u16(st + 4);
        if (inputCount == 0 || coverageIndex(d, st + d.u16(st + 6), g) == kNotCovered) {
            return false;
        }
        if (c.matchInput(inputCount, Iterator::MatchKind::Coverage, st + 8, st, 0, matchEnd, positions, nullptr)) {
            c.applyNested(inputCount, positions, lookupCount, st + 6 + inputCount * 2, matchEnd);
            return true;
        }
    }
    return false;
}

bool applyChainContext(Context &c, std::uint32_t st) {
    const Data &d = c.d;
    const std::uint16_t format = d.u16(st);
    const GlyphId g = c.buffer.cur().glyph;
    std::size_t positions[kMaxContextLength];
    std::size_t matchEnd = 0;
    if (format == 1 || format == 2) {
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered) {
            return false;
        }
        std::uint32_t backCd = 0;
        std::uint32_t inputCd = 0;
        std::uint32_t lookCd = 0;
        std::uint32_t setIndex = index;
        std::uint32_t sets = st + 6;
        std::uint16_t setCount = d.u16(st + 4);
        if (format == 2) {
            backCd = st + d.u16(st + 4);
            inputCd = st + d.u16(st + 6);
            lookCd = st + d.u16(st + 8);
            setIndex = classOf(d, inputCd, g);
            setCount = d.u16(st + 10);
            sets = st + 12;
        }
        if (setIndex >= setCount) {
            return false;
        }
        const std::uint16_t setOffset = d.u16(sets + setIndex * 2);
        if (setOffset == 0) {
            return false;
        }
        const Iterator::MatchKind kind = format == 1 ? Iterator::MatchKind::Glyph : Iterator::MatchKind::Class;
        const std::uint32_t set = st + setOffset;
        const std::uint16_t rules = d.u16(set);
        for (std::uint32_t r = 0; r < rules; ++r) {
            const std::uint32_t rule = set + d.u16(set + 2 + r * 2);
            const unsigned backCount = d.u16(rule);
            const std::uint32_t back = rule + 2;
            const std::uint32_t in = back + backCount * 2;
            const unsigned inputCount = d.u16(in);
            if (inputCount == 0) {
                continue;
            }
            const std::uint32_t look = in + 2 + (inputCount - 1) * 2;
            const unsigned lookCount = d.u16(look);
            const std::uint32_t lookups = look + 2 + lookCount * 2;
            const unsigned lookupCount = d.u16(lookups);
            if (c.matchInput(inputCount, kind, in + 2, 0, inputCd, matchEnd, positions, nullptr) &&
                c.matchLookahead(lookCount, kind, look + 2, 0, lookCd, matchEnd) &&
                c.matchBacktrack(backCount, kind, back, 0, backCd)) {
                c.applyNested(inputCount, positions, lookupCount, lookups + 2, matchEnd);
                return true;
            }
        }
        return false;
    }
    if (format == 3) {
        const unsigned backCount = d.u16(st + 2);
        const std::uint32_t back = st + 4;
        const std::uint32_t in = back + backCount * 2;
        const unsigned inputCount = d.u16(in);
        if (inputCount == 0 || coverageIndex(d, st + d.u16(in + 2), g) == kNotCovered) {
            return false;
        }
        const std::uint32_t look = in + 2 + inputCount * 2;
        const unsigned lookCount = d.u16(look);
        const std::uint32_t lookups = look + 2 + lookCount * 2;
        const unsigned lookupCount = d.u16(lookups);
        if (c.matchInput(inputCount, Iterator::MatchKind::Coverage, in + 4, st, 0, matchEnd, positions, nullptr) &&
            c.matchLookahead(lookCount, Iterator::MatchKind::Coverage, look + 2, st, 0, matchEnd) &&
            c.matchBacktrack(backCount, Iterator::MatchKind::Coverage, back, st, 0)) {
            c.applyNested(inputCount, positions, lookupCount, lookups + 2, matchEnd);
            return true;
        }
    }
    return false;
}

bool Context::applyGsub(std::uint16_t type, std::uint32_t st) {
    const GlyphId g = buffer.cur().glyph;
    const std::uint16_t format = d.u16(st);
    switch (type) {
    case 1: { // single
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered) {
            return false;
        }
        if (format == 1) {
            replaceGlyph(static_cast<GlyphId>((g + d.i16(st + 4)) & 0xFFFF));
            return true;
        }
        if (format == 2 && index < d.u16(st + 4)) {
            replaceGlyph(d.u16(st + 6 + index * 2));
            return true;
        }
        return false;
    }
    case 2: { // multiple
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered || index >= d.u16(st + 4)) {
            return false;
        }
        const std::uint32_t seq = st + d.u16(st + 6 + index * 2);
        const unsigned count = d.u16(seq);
        if (count == 1) {
            replaceGlyph(d.u16(seq + 2));
            return true;
        }
        if (count == 0) {
            buffer.deleteGlyph();
            return true;
        }
        const std::uint16_t klass = isLigature(buffer.cur()) ? kBaseGlyph : 0;
        const unsigned id = ligId(buffer.cur());
        for (unsigned i = 0; i < count; ++i) {
            if (!id) {
                setLigPropsForMark(buffer.cur(), 0, i);
            }
            outputGlyphForComponent(d.u16(seq + 2 + i * 2), klass);
        }
        buffer.skipGlyph();
        return true;
    }
    case 3: { // alternate
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered || index >= d.u16(st + 4)) {
            return false;
        }
        const std::uint32_t set = st + d.u16(st + 6 + index * 2);
        const unsigned count = d.u16(set);
        const auto shift = static_cast<unsigned>(std::countr_zero(lookupMask));
        unsigned alt = (lookupMask & buffer.cur().mask) >> shift;
        if (alt == 255 && random && count != 0) { // 'rand': HarfBuzz's minstd sequence
            buffer.randomState = buffer.randomState * 48271u % 2147483647u;
            alt = buffer.randomState % count + 1;
        }
        if (count == 0 || alt == 0 || alt > count) {
            return false;
        }
        replaceGlyph(d.u16(set + 2 + (alt - 1) * 2));
        return true;
    }
    case 4: { // ligature
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered || index >= d.u16(st + 4)) {
            return false;
        }
        const std::uint32_t set = st + d.u16(st + 6 + index * 2);
        const std::uint16_t ligatures = d.u16(set);
        std::size_t positions[kMaxContextLength];
        for (std::uint32_t l = 0; l < ligatures; ++l) {
            const std::uint32_t lig = set + d.u16(set + 2 + l * 2);
            const GlyphId ligGlyph = d.u16(lig);
            const unsigned count = d.u16(lig + 2);
            if (count == 0) {
                continue;
            }
            if (count == 1) {
                replaceGlyph(ligGlyph);
                return true;
            }
            std::size_t matchEnd = 0;
            unsigned total = 0;
            if (matchInput(count, Iterator::MatchKind::Glyph, lig + 4, 0, 0, matchEnd, positions, &total)) {
                return ligate(count, positions, matchEnd, ligGlyph, total);
            }
        }
        return false;
    }
    case 5: return applyContext(*this, st);
    case 6: return applyChainContext(*this, st);
    case 8: { // reverse chaining single
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered || nestingLeft != kMaxNesting) {
            return false;
        }
        const unsigned backCount = d.u16(st + 4);
        const std::uint32_t look = st + 6 + backCount * 2;
        const unsigned lookCount = d.u16(look);
        const std::uint32_t subst = look + 2 + lookCount * 2;
        if (index >= d.u16(subst)) {
            return false;
        }
        if (matchBacktrack(backCount, Iterator::MatchKind::Coverage, st + 6, st, 0) &&
            matchLookahead(lookCount, Iterator::MatchKind::Coverage, look + 2, st, 0, buffer.idx + 1)) {
            replaceGlyphInPlace(d.u16(subst + 2 + index * 2));
            return true;
        }
        return false;
    }
    default: return false;
    }
}

void reverseCursiveMinorOffset(std::vector<GlyphPosition> &pos, std::size_t i, std::size_t newParent, int depth = 0) {
    const int chain = pos[i].attachChain;
    const std::uint8_t type = pos[i].attachType;
    if (!chain || !(type & 2) || depth > kMaxNesting) {
        return;
    }
    pos[i].attachChain = 0;
    const std::size_t j = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) + chain);
    if (j == newParent || j >= pos.size()) {
        return;
    }
    reverseCursiveMinorOffset(pos, j, newParent, depth + 1);
    pos[j].yOffset = -pos[i].yOffset;
    pos[j].attachChain = static_cast<std::int16_t>(-chain);
    pos[j].attachType = type;
}

bool Context::applyGpos(std::uint16_t type, std::uint32_t st) {
    const GlyphId g = buffer.cur().glyph;
    const std::uint16_t format = d.u16(st);
    switch (type) {
    case 1: { // single adjustment
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered) {
            return false;
        }
        const std::uint16_t vf = d.u16(st + 4);
        const std::uint32_t size = static_cast<std::uint32_t>(popcount16(vf)) * 2;
        if (format == 1) {
            applyValue(vf, st + 6, buffer.pos[buffer.idx]);
        } else if (format == 2) {
            if (index >= d.u16(st + 6)) {
                return false;
            }
            applyValue(vf, st + 8 + index * size, buffer.pos[buffer.idx]);
        } else {
            return false;
        }
        ++buffer.idx;
        return true;
    }
    case 2: { // pair adjustment
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), g);
        if (index == kNotCovered) {
            return false;
        }
        Iterator &it = input;
        it.reset(buffer.idx, 1);
        it.setMatch(Iterator::MatchKind::None, 0, 0, 0);
        if (!it.next()) {
            return false;
        }
        const std::size_t second = it.idx;
        const std::uint16_t vf1 = d.u16(st + 4);
        const std::uint16_t vf2 = d.u16(st + 6);
        const std::uint32_t len1 = static_cast<std::uint32_t>(popcount16(vf1)) * 2;
        const std::uint32_t len2 = static_cast<std::uint32_t>(popcount16(vf2)) * 2;
        if (format == 1) {
            if (index >= d.u16(st + 8)) {
                return false;
            }
            const std::uint32_t set = st + d.u16(st + 10 + index * 2);
            const std::uint32_t recordSize = 2 + len1 + len2;
            std::uint32_t lo = 0;
            std::uint32_t hi = d.u16(set);
            const GlyphId target = buffer.info[second].glyph;
            while (lo < hi) {
                const std::uint32_t mid = (lo + hi) / 2;
                const std::uint32_t rec = set + 2 + mid * recordSize;
                const std::uint16_t v = d.u16(rec);
                if (v < target) {
                    lo = mid + 1;
                } else if (v > target) {
                    hi = mid;
                } else {
                    applyValue(vf1, rec + 2, buffer.pos[buffer.idx]);
                    applyValue(vf2, rec + 2 + len1, buffer.pos[second]);
                    buffer.idx = len2 ? second + 1 : second;
                    return true;
                }
            }
            return false;
        }
        if (format == 2) {
            const unsigned class1Count = d.u16(st + 12);
            const unsigned class2Count = d.u16(st + 14);
            const unsigned k1 = classOf(d, st + d.u16(st + 8), g);
            const unsigned k2 = classOf(d, st + d.u16(st + 10), buffer.info[second].glyph);
            if (k1 >= class1Count || k2 >= class2Count) {
                return false;
            }
            const std::uint32_t rec = st + 16 + (k1 * class2Count + k2) * (len1 + len2);
            applyValue(vf1, rec, buffer.pos[buffer.idx]);
            applyValue(vf2, rec + len1, buffer.pos[second]);
            buffer.idx = len2 ? second + 1 : second;
            return true;
        }
        return false;
    }
    case 3: { // cursive attachment
        const std::uint32_t cov = st + d.u16(st + 2);
        const std::uint32_t index = coverageIndex(d, cov, g);
        if (index == kNotCovered || index >= d.u16(st + 4)) {
            return false;
        }
        const std::uint16_t entry = d.u16(st + 6 + index * 4);
        if (!entry) {
            return false;
        }
        Iterator &it = input;
        it.reset(buffer.idx, 1);
        it.setMatch(Iterator::MatchKind::None, 0, 0, 0);
        if (!it.prev()) {
            return false;
        }
        const std::uint32_t prevIndex = coverageIndex(d, cov, buffer.info[it.idx].glyph);
        if (prevIndex == kNotCovered || prevIndex >= d.u16(st + 4)) {
            return false;
        }
        const std::uint16_t exit = d.u16(st + 6 + prevIndex * 4 + 2);
        if (!exit) {
            return false;
        }
        const std::size_t i = it.idx;
        const std::size_t j = buffer.idx;
        float entryX = 0;
        float entryY = 0;
        float exitX = 0;
        float exitY = 0;
        anchor(st + exit, exitX, exitY);
        anchor(st + entry, entryX, entryY);
        std::vector<GlyphPosition> &pos = buffer.pos;
        if (!buffer.backward) {
            pos[i].xAdvance = static_cast<std::int32_t>(std::lround(exitX)) + pos[i].xOffset;
            const std::int32_t dd = static_cast<std::int32_t>(std::lround(entryX)) + pos[j].xOffset;
            pos[j].xAdvance -= dd;
            pos[j].xOffset -= dd;
        } else {
            const std::int32_t dd = static_cast<std::int32_t>(std::lround(exitX)) + pos[i].xOffset;
            pos[i].xAdvance -= dd;
            pos[i].xOffset -= dd;
            pos[j].xAdvance = static_cast<std::int32_t>(std::lround(entryX)) + pos[j].xOffset;
        }
        std::size_t child = i;
        std::size_t parent = j;
        std::int32_t yOffset = static_cast<std::int32_t>(std::lround(entryY - exitY));
        if (!(lookupProps & 0x01)) { // not RightToLeft
            std::swap(child, parent);
            yOffset = -yOffset;
        }
        reverseCursiveMinorOffset(pos, child, parent);
        pos[child].attachType = 2;
        pos[child].attachChain = static_cast<std::int16_t>(static_cast<std::ptrdiff_t>(parent) - static_cast<std::ptrdiff_t>(child));
        pos[child].yOffset = yOffset;
        if (pos[parent].attachChain == -pos[child].attachChain) {
            pos[parent].attachChain = 0;
            pos[parent].yOffset = 0;
        }
        ++buffer.idx;
        return true;
    }
    case 4:   // mark to base
    case 5: { // mark to ligature
        const std::uint32_t markIndex = coverageIndex(d, st + d.u16(st + 2), g);
        if (markIndex == kNotCovered) {
            return false;
        }
        // Search back for a non-mark glyph; the search is cached across the
        // marks of a run (shared by mark-to-base and mark-to-ligature, as in
        // HarfBuzz).
        Iterator &it = input;
        const std::uint32_t savedProps = it.matchProps;
        it.matchProps = 0x08; // IgnoreMarks
        it.setMatch(Iterator::MatchKind::None, 0, 0, 0);
        if (lastBaseUntil > buffer.idx) {
            lastBaseUntil = 0;
            lastBase = -1;
        }
        const std::uint32_t baseCoverage = st + d.u16(st + 4);
        for (std::size_t j = buffer.idx; j > lastBaseUntil; --j) {
            const std::size_t k = j - 1;
            const GlyphInfo &info = buffer.info[k];
            const Iterator::Skip skip = it.maySkip(info);
            if (skip == Iterator::SkipYes) {
                continue;
            }
            const Iterator::Match match = it.mayMatch(info);
            bool matched = match == Iterator::MatchYes || (match == Iterator::MatchMaybe && skip == Iterator::SkipNo);
            if (matched) {
                // Attach to the first glyph of a multiple substitution only
                // (unless the font covers the others as bases).
                bool accept = !isMultiplied(info) || ligComp(info) == 0;
                if (type == 4) {
                    accept = accept || k == 0 || isMark(buffer.info[k - 1]) || !isMultiplied(buffer.info[k - 1]) ||
                             ligId(info) != ligId(buffer.info[k - 1]) || ligComp(info) != ligComp(buffer.info[k - 1]) + 1;
                }
                if (!accept && coverageIndex(d, baseCoverage, info.glyph) == kNotCovered) {
                    matched = false;
                }
            }
            if (matched) {
                lastBase = static_cast<int>(k);
                break;
            }
        }
        lastBaseUntil = buffer.idx;
        it.matchProps = savedProps;
        if (lastBase < 0) {
            return false;
        }
        const std::size_t base = static_cast<std::size_t>(lastBase);
        const std::uint32_t baseIndex = coverageIndex(d, st + d.u16(st + 4), buffer.info[base].glyph);
        if (baseIndex == kNotCovered) {
            return false;
        }
        const unsigned classCount = d.u16(st + 6);
        const std::uint32_t markArray = st + d.u16(st + 8);
        if (markIndex >= d.u16(markArray)) {
            return false;
        }
        if (type == 4) {
            const std::uint32_t baseArray = st + d.u16(st + 10);
            if (baseIndex >= d.u16(baseArray)) {
                return false;
            }
            return markArrayApply(markArray, markIndex, baseIndex, baseArray + 2, baseArray, classCount, base);
        }
        const std::uint32_t ligArray = st + d.u16(st + 10);
        if (baseIndex >= d.u16(ligArray)) {
            return false;
        }
        const std::uint32_t attach = ligArray + d.u16(ligArray + 2 + baseIndex * 2);
        const unsigned compCount = d.u16(attach);
        if (compCount == 0) {
            return false;
        }
        const unsigned id = ligId(buffer.info[base]);
        const unsigned markId = ligId(buffer.cur());
        const unsigned markComp = ligComp(buffer.cur());
        const unsigned comp = (id && id == markId && markComp > 0) ? std::min(compCount, markComp) - 1 : compCount - 1;
        return markArrayApply(markArray, markIndex, comp, attach + 2, attach, classCount, base);
    }
    case 6: { // mark to mark
        const std::uint32_t mark1Index = coverageIndex(d, st + d.u16(st + 2), g);
        if (mark1Index == kNotCovered) {
            return false;
        }
        Iterator &it = input;
        const std::uint32_t savedProps = it.matchProps;
        it.matchProps = lookupProps & ~0x0Eu;
        it.reset(buffer.idx, 1);
        it.setMatch(Iterator::MatchKind::None, 0, 0, 0);
        const bool found = it.prev();
        it.matchProps = savedProps;
        if (!found || !isMark(buffer.info[it.idx])) {
            return false;
        }
        const std::size_t j = it.idx;
        const unsigned id1 = ligId(buffer.cur());
        const unsigned id2 = ligId(buffer.info[j]);
        const unsigned comp1 = ligComp(buffer.cur());
        const unsigned comp2 = ligComp(buffer.info[j]);
        bool good = false;
        if (id1 == id2) {
            good = id1 == 0 || comp1 == comp2;
        } else {
            good = (id1 > 0 && !comp1) || (id2 > 0 && !comp2);
        }
        if (!good) {
            return false;
        }
        const std::uint32_t mark2Index = coverageIndex(d, st + d.u16(st + 4), buffer.info[j].glyph);
        if (mark2Index == kNotCovered) {
            return false;
        }
        const unsigned classCount = d.u16(st + 6);
        const std::uint32_t mark1Array = st + d.u16(st + 8);
        const std::uint32_t mark2Array = st + d.u16(st + 10);
        if (mark1Index >= d.u16(mark1Array) || mark2Index >= d.u16(mark2Array)) {
            return false;
        }
        return markArrayApply(mark1Array, mark1Index, mark2Index, mark2Array + 2, mark2Array, classCount, j);
    }
    case 7: return applyContext(*this, st);
    case 8: return applyChainContext(*this, st);
    default: return false;
    }
}

} // namespace

namespace {

// The subtable's first-glyph coverage (what HarfBuzz's lookup accelerator
// collects), or 0.
std::uint32_t firstCoverage(const Data &d, std::uint16_t type, std::uint32_t st) {
    const std::uint16_t format = d.u16(st);
    if ((type == 5 && format == 3)) {
        return d.u16(st + 2) ? st + d.u16(st + 6) : 0;
    }
    if (type == 6 && format == 3) {
        const std::uint32_t input = st + 4 + 2u * d.u16(st + 2);
        return d.u16(input) ? st + d.u16(input + 2) : 0;
    }
    return st + d.u16(st + 2);
}

// Whether glyphs[1..] match the rule's input values (glyph ids, classes or
// coverages), and the rule's input is exactly as long as `glyphs`.
bool wouldMatchInput(const Data &d, Span<const GlyphId> glyphs, unsigned count, std::uint32_t values,
                     std::uint32_t valueBase, int kind, std::uint32_t classDef) {
    if (count != glyphs.size()) {
        return false;
    }
    for (unsigned i = 1; i < count; ++i) {
        const std::uint16_t v = d.u16(values + 2u * (i - 1));
        const GlyphId g = glyphs[i];
        const bool ok = kind == 0   ? g == v
                        : kind == 1 ? classOf(d, classDef, g) == v
                                    : coverageIndex(d, valueBase + v, g) != kNotCovered;
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool subtableWouldApply(const Data &d, std::uint16_t type, std::uint32_t st, Span<const GlyphId> glyphs, bool zeroContext) {
    const std::uint16_t format = d.u16(st);
    const GlyphId first = glyphs[0];
    switch (type) {
    case 1:
    case 2:
    case 3:
    case 8:
        return glyphs.size() == 1 && coverageIndex(d, st + d.u16(st + 2), first) != kNotCovered;
    case 4: {
        const std::uint32_t index = coverageIndex(d, st + d.u16(st + 2), first);
        if (index == kNotCovered || index >= d.u16(st + 4)) {
            return false;
        }
        const std::uint32_t set = st + d.u16(st + 6 + 2 * index);
        for (unsigned k = 0, n = d.u16(set); k < n; ++k) {
            const std::uint32_t lig = set + d.u16(set + 2 + 2 * k);
            const unsigned components = d.u16(lig + 2);
            if (components != glyphs.size()) {
                continue;
            }
            bool ok = true;
            for (unsigned i = 1; i < components && ok; ++i) {
                ok = glyphs[i] == d.u16(lig + 4 + 2 * (i - 1));
            }
            if (ok) {
                return true;
            }
        }
        return false;
    }
    case 5:
        if (format == 1 || format == 2) {
            std::uint32_t index = 0;
            std::uint32_t classDef = 0;
            if (format == 1) {
                index = coverageIndex(d, st + d.u16(st + 2), first);
                if (index == kNotCovered) {
                    return false;
                }
            } else {
                classDef = st + d.u16(st + 4);
                index = classOf(d, classDef, first);
            }
            const std::uint32_t setsAt = st + (format == 1 ? 4 : 6);
            if (index >= d.u16(setsAt) || d.u16(setsAt + 2 + 2 * index) == 0) {
                return false;
            }
            const std::uint32_t set = st + d.u16(setsAt + 2 + 2 * index);
            for (unsigned k = 0, n = d.u16(set); k < n; ++k) {
                const std::uint32_t rule = set + d.u16(set + 2 + 2 * k);
                if (wouldMatchInput(d, glyphs, d.u16(rule), rule + 4, 0, format == 1 ? 0 : 1, classDef)) {
                    return true;
                }
            }
            return false;
        }
        if (format == 3) {
            return wouldMatchInput(d, glyphs, d.u16(st + 2), st + 8, st, 2, 0);
        }
        return false;
    case 6:
        if (format == 1 || format == 2) {
            std::uint32_t index = 0;
            std::uint32_t classDef = 0;
            if (format == 1) {
                index = coverageIndex(d, st + d.u16(st + 2), first);
                if (index == kNotCovered) {
                    return false;
                }
            } else {
                classDef = st + d.u16(st + 6); // the input class definition
                index = classOf(d, classDef, first);
            }
            const std::uint32_t setsAt = st + (format == 1 ? 4 : 10);
            if (index >= d.u16(setsAt) || d.u16(setsAt + 2 + 2 * index) == 0) {
                return false;
            }
            const std::uint32_t set = st + d.u16(setsAt + 2 + 2 * index);
            for (unsigned k = 0, n = d.u16(set); k < n; ++k) {
                const std::uint32_t rule = set + d.u16(set + 2 + 2 * k);
                const unsigned backtrack = d.u16(rule);
                const std::uint32_t input = rule + 2 + 2 * backtrack;
                const unsigned inputCount = d.u16(input);
                const unsigned lookahead = d.u16(input + 2 + 2 * (inputCount ? inputCount - 1 : 0));
                if (zeroContext && (backtrack || lookahead)) {
                    continue;
                }
                if (wouldMatchInput(d, glyphs, inputCount, input + 2, 0, format == 1 ? 0 : 1, classDef)) {
                    return true;
                }
            }
            return false;
        }
        if (format == 3) {
            const unsigned backtrack = d.u16(st + 2);
            const std::uint32_t input = st + 4 + 2 * backtrack;
            const unsigned inputCount = d.u16(input);
            const unsigned lookahead = d.u16(input + 2 + 2 * inputCount);
            if (zeroContext && (backtrack || lookahead)) {
                return false;
            }
            return wouldMatchInput(d, glyphs, inputCount, input + 4, st, 2, 0);
        }
        return false;
    default: return false;
    }
}

} // namespace

bool wouldSubstitute(const LayoutTable &gsub, std::uint16_t index, Span<const GlyphId> glyphs, bool zeroContext) {
    if (glyphs.empty() || !gsub.present() || gsub.kind() != TableKind::Gsub) {
        return false;
    }
    const Data d(gsub.table());
    const std::uint32_t list = gsub.lookupList();
    if (list == 0 || index >= d.u16(list)) {
        return false;
    }
    const std::uint32_t lookup = list + d.u16(list + 2 + index * 2u);
    const std::uint16_t lookupType = d.u16(lookup);
    const std::uint16_t count = d.u16(lookup + 4);
    std::vector<std::pair<std::uint16_t, std::uint32_t>> subtables;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t st = lookup + d.u16(lookup + 6 + i * 2);
        std::uint16_t type = lookupType;
        if (type == 7) {
            type = d.u16(st + 2);
            st = st + d.u32(st + 4);
        }
        if (st < d.n) {
            subtables.emplace_back(type, st);
        }
    }
    // The lookup covers the first glyph somewhere (HarfBuzz's accelerator).
    bool covered = false;
    for (const auto &[type, st] : subtables) {
        const std::uint32_t cov = firstCoverage(d, type, st);
        covered = covered || (cov != 0 && coverageIndex(d, cov, glyphs[0]) != kNotCovered);
    }
    if (!covered) {
        return false;
    }
    for (const auto &[type, st] : subtables) {
        if (subtableWouldApply(d, type, st, glyphs, zeroContext)) {
            return true;
        }
    }
    return false;
}

void applyLookups(const FontFace &face, const Gdef &gdef, const LayoutTable &table, Span<const PlannedLookup> lookups,
                  Buffer &buffer) {
    if (!table.present()) {
        return;
    }
    Context c(face, gdef, table, buffer);
    for (const PlannedLookup &l : lookups) {
        if (buffer.len() == 0 || l.mask == 0 || buffer.maxOps <= 0) {
            return;
        }
        const std::uint32_t lookup = c.lookupOffset(l.index);
        if (lookup == 0) {
            continue;
        }
        c.lookupMask = l.mask;
        c.autoZwj = l.autoZwj;
        c.autoZwnj = l.autoZwnj;
        c.perSyllable = l.perSyllable && table.kind() == TableKind::Gsub;
        c.random = l.random;
        c.lookupProps = c.lookupPropsOf(lookup);
        c.lastBase = -1;
        c.lastBaseUntil = 0;
        std::uint16_t type = c.d.u16(lookup);
        if (table.kind() == TableKind::Gsub && type == 7 && c.d.u16(lookup + 4) > 0) {
            type = c.d.u16(lookup + c.d.u16(lookup + 6) + 2); // extension: the wrapped type
        }
        const auto applyHere = [&]() {
            const std::uint32_t props = c.lookupProps;
            const bool ok = c.applyLookup(l.index);
            c.lookupProps = props;
            return ok;
        };
        if (table.kind() == TableKind::Gsub && type == 8) {
            // Reverse chaining: in place, from the end.
            buffer.haveOutput = false;
            if (buffer.len() == 0) {
                continue;
            }
            buffer.idx = buffer.len() - 1;
            while (true) {
                if (buffer.maxOps <= 0) {
                    break;
                }
                GlyphInfo &cur = buffer.cur();
                if ((cur.mask & c.lookupMask) && c.checkGlyphProperty(cur, c.lookupProps)) {
                    (void)applyHere();
                }
                if (buffer.idx == 0) {
                    break;
                }
                --buffer.idx;
            }
            buffer.idx = 0;
            continue;
        }
        if (table.kind() == TableKind::Gsub) {
            buffer.clearOutput();
        }
        buffer.idx = 0;
        // Each step consumes or passes a glyph; the cap only guarantees an
        // end should a malformed subtable report success without moving.
        std::size_t steps = 0;
        const std::size_t maxSteps = 16 * (std::max(buffer.maxLen, buffer.len()) + 1);
        while (buffer.idx < buffer.len()) {
            if (buffer.maxOps <= 0 || ++steps > maxSteps) {
                break;
            }
            bool applied = false;
            const GlyphInfo &cur = buffer.cur();
            if ((cur.mask & c.lookupMask) && c.checkGlyphProperty(cur, c.lookupProps)) {
                applied = applyHere();
            }
            if (!applied) {
                buffer.nextGlyph();
            }
        }
        if (table.kind() == TableKind::Gsub) {
            buffer.swapBuffers();
        }
        buffer.idx = 0;
    }
}

void propagateAttachments(Buffer &buffer) {
    std::vector<GlyphPosition> &pos = buffer.pos;
    const std::size_t len = pos.size();
    const bool forward = !buffer.backward;
    // As HarfBuzz: a cursive glyph takes the cross-stream (y) offset of the
    // glyph it hangs on, a mark the x offset of its base, less the advances
    // in between.
    const auto propagate = [&](auto &&self, std::size_t i, int nesting) -> void {
        const int chain = pos[i].attachChain;
        const std::uint8_t type = pos[i].attachType;
        pos[i].attachChain = 0;
        const std::ptrdiff_t jj = static_cast<std::ptrdiff_t>(i) + chain;
        if (jj < 0 || static_cast<std::size_t>(jj) >= len || nesting == 0) {
            return;
        }
        const auto j = static_cast<std::size_t>(jj);
        if (pos[j].attachChain) {
            self(self, j, nesting - 1);
        }
        if (type & 2) { // cursive
            pos[i].yOffset += pos[j].yOffset;
            return;
        }
        pos[i].xOffset += pos[j].xOffset;
        if (j < i) {
            if (forward) {
                for (std::size_t k = j; k < i; ++k) {
                    pos[i].xOffset -= pos[k].xAdvance;
                    pos[i].yOffset -= pos[k].yAdvance;
                }
            } else {
                for (std::size_t k = j + 1; k < i + 1; ++k) {
                    pos[i].xOffset += pos[k].xAdvance;
                    pos[i].yOffset += pos[k].yAdvance;
                }
            }
        } else if (forward) {
            for (std::size_t k = i; k < j; ++k) {
                pos[i].xOffset += pos[k].xAdvance;
                pos[i].yOffset += pos[k].yAdvance;
            }
        } else {
            for (std::size_t k = i + 1; k < j + 1; ++k) {
                pos[i].xOffset -= pos[k].xAdvance;
                pos[i].yOffset -= pos[k].yAdvance;
            }
        }
    };
    for (std::size_t n = 0; n < len; ++n) {
        const std::size_t i = forward ? n : len - 1 - n;
        if (pos[i].attachChain) {
            propagate(propagate, i, kMaxNesting);
        }
    }
}

namespace {

// The legacy 'kern' table (Microsoft version 0): its subtables, as HarfBuzz
// reads them (the last one's length is ignored: fonts overflow it).
template <class F>
void forEachKernSubtable(const Data &d, F &&f) {
    if (d.n < 4 || d.u16(0) != 0) {
        return;
    }
    const std::uint16_t count = d.u16(2);
    std::uint32_t at = 4;
    for (std::uint32_t t = 0; t < count && at + 6 <= d.n; ++t) {
        const std::uint16_t length = d.u16(at + 2);
        const std::uint8_t format = d.u8(at + 4);
        const std::uint8_t coverage = d.u8(at + 5);
        const std::size_t end = t + 1 < count ? std::min<std::size_t>(at + length, d.n) : d.n;
        f(at, end, format, coverage);
        if (length < 6) {
            return; // a broken chain of subtables
        }
        at += length;
    }
}

} // namespace

bool hasKernTable(const FontFace &face) {
    const Data d(face.table(FontFace::tag("kern")));
    return d.n >= 4 && d.u16(0) == 0 && d.u16(2) != 0;
}

bool hasCrossStreamKerning(const FontFace &face) {
    bool cross = false;
    forEachKernSubtable(Data(face.table(FontFace::tag("kern"))),
                        [&](std::uint32_t, std::size_t, std::uint8_t, std::uint8_t coverage) { cross = cross || (coverage & 0x04); });
    return cross;
}

bool hasMachineKerning(const FontFace &face) {
    bool machine = false;
    forEachKernSubtable(Data(face.table(FontFace::tag("kern"))),
                        [&](std::uint32_t, std::size_t, std::uint8_t format, std::uint8_t) { machine = machine || format == 1; });
    return machine;
}

bool applyKernTable(const FontFace &face, const Gdef &gdef, std::uint32_t mask, Buffer &buffer) {
    const Data table(face.table(FontFace::tag("kern")));
    LayoutTable none;
    Context c(face, gdef, none, buffer);
    c.kind = TableKind::Gpos; // positioning: ZWNJ and hidden glyphs are skipped
    c.lookupMask = mask;
    c.lookupProps = 0x08; // IgnoreMarks
    Iterator &it = c.input;
    it.init(c, false);
    bool applied = false;
    bool seenCrossStream = false;
    bool reversed = false;
    forEachKernSubtable(table, [&](std::uint32_t st, std::size_t end, std::uint8_t format, std::uint8_t coverage) {
        if (!(coverage & 0x01) || (format != 0 && format != 2)) {
            return; // horizontal pairs and classes only (no state machines)
        }
        Data d = table;
        d.n = end;
        const bool crossStream = coverage & 0x04;
        if (crossStream && !seenCrossStream) {
            seenCrossStream = true; // all glyphs hang on each other across the stream
            for (GlyphPosition &p : buffer.pos) {
                p.attachType = 2;
                p.attachChain = static_cast<std::int16_t>(buffer.backward ? 1 : -1);
            }
        }
        // Pairs are left-to-right on screen: right-to-left text is reversed.
        if (buffer.backward != reversed) {
            buffer.reverse();
            reversed = !reversed;
        }
        const auto kerning = [&](GlyphId left, GlyphId right) -> std::int32_t {
            if (format == 0) {
                const std::uint32_t key = static_cast<std::uint32_t>(left) << 16 | right;
                std::uint32_t lo = 0;
                std::uint32_t hi = d.u16(st + 6);
                while (lo < hi) {
                    const std::uint32_t mid = (lo + hi) / 2;
                    const std::uint32_t k = d.u32(st + 14 + mid * 6);
                    if (k < key) {
                        lo = mid + 1;
                    } else if (k > key) {
                        hi = mid;
                    } else {
                        return d.i16(st + 14 + mid * 6 + 4);
                    }
                }
                return 0;
            }
            // Format 2: class values are byte offsets that sum to the value's.
            // Glyphs outside a class array (or of class 1, "out of bounds")
            // do not kern.
            const auto classOfGlyph = [&](std::uint32_t classTable, GlyphId g) -> std::optional<std::uint32_t> {
                const std::uint32_t ct = st + classTable;
                const std::uint16_t first = d.u16(ct);
                const std::uint16_t n = d.u16(ct + 2);
                const std::uint32_t i = static_cast<std::uint32_t>(g) - first;
                if (g < first || i >= n) {
                    return std::nullopt;
                }
                const std::uint16_t value = d.u16(ct + 4 + i * 2);
                return value == 1 ? std::nullopt : std::optional<std::uint32_t>(value);
            };
            const std::optional<std::uint32_t> leftClass = classOfGlyph(d.u16(st + 8), left);
            const std::optional<std::uint32_t> rightClass = classOfGlyph(d.u16(st + 10), right);
            if (!leftClass || !rightClass) {
                return 0;
            }
            const std::uint32_t offset = *leftClass + *rightClass;
            const std::uint32_t array = d.u16(st + 12);
            if (offset < array || st + array + (offset - array) / 2 * 2 + 2 > d.n) {
                return 0;
            }
            return d.i16(st + array + (offset - array) / 2 * 2);
        };
        for (std::size_t idx = 0; idx < buffer.len();) {
            if (!(buffer.info[idx].mask & mask)) {
                ++idx;
                continue;
            }
            it.reset(idx, 1);
            if (!it.next()) {
                ++idx;
                continue;
            }
            const std::size_t i = idx;
            const std::size_t j = it.idx;
            const std::int32_t kern = kerning(buffer.info[i].glyph, buffer.info[j].glyph);
            if (kern != 0) {
                applied = true;
                if (crossStream) {
                    buffer.pos[j].yOffset = kern;
                } else {
                    const std::int32_t kern1 = kern >> 1;
                    const std::int32_t kern2 = kern - kern1;
                    buffer.pos[i].xAdvance += kern1;
                    buffer.pos[j].xAdvance += kern2;
                    buffer.pos[j].xOffset += kern2;
                }
            }
            idx = j;
        }
    });
    if (reversed) {
        buffer.reverse();
    }
    return applied;
}

} // namespace cfw::ot
