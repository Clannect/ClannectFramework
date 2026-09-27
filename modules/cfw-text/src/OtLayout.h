#pragma once

// OpenType layout (GDEF, GSUB, GPOS): the glyph buffer, the plan of lookups,
// and their application. Internal to cfw-text; Shaper.cpp drives it. The
// semantics follow HarfBuzz, the reference the shaper is tested against:
// its buffer model (an output side for substitutions, cluster merging), its
// skipping iterator (lookup flags, mark filtering sets, ZWJ/ZWNJ), ligature
// component tracking for marks, and attachment propagation.

#include <cstdint>
#include <vector>

#include "cfw/core/Span.h"
#include "cfw/text/FontFace.h"

namespace cfw::ot {

// Glyph properties (GDEF class, as lookup flags see them).
enum : std::uint16_t {
    kBaseGlyph = 0x02,
    kLigature = 0x04,
    kMark = 0x08,
    kSubstituted = 0x10,
    kLigated = 0x20,
    kMultiplied = 0x40,
    kPreserve = kSubstituted | kLigated | kMultiplied,
};

// Unicode-derived flags carried by each glyph.
enum : std::uint16_t {
    kIgnorable = 0x01,    // default ignorable
    kHidden = 0x02,       // ignorable that GSUB must not skip (CGJ, tags, Mongolian FVS)
    kZwj = 0x04,
    kZwnj = 0x08,
    kContinuation = 0x10, // continues the previous grapheme (for cluster forming)
};

struct GlyphInfo {
    GlyphId glyph = 0;
    char32_t codepoint = 0;
    std::uint32_t cluster = 0;
    std::uint32_t mask = 0;
    std::uint16_t glyphProps = 0; // class bits | mark attachment class << 8
    std::uint16_t flags = 0;      // k* Unicode flags
    std::uint8_t generalCategory = 0;
    std::uint8_t combiningClass = 0; // modified combining class (HarfBuzz's)
    std::uint8_t ligProps = 0;       // lig id << 5 | is-ligature-base 0x10 | component or count
    std::uint8_t shaperAction = 0;   // per-shaper use (Arabic joining form)
    std::uint8_t spaceFallback = 0;  // for a space without its own glyph
};

struct GlyphPosition {
    std::int32_t xAdvance = 0;
    std::int32_t yAdvance = 0;
    std::int32_t xOffset = 0;
    std::int32_t yOffset = 0;
    std::int16_t attachChain = 0;
    std::uint8_t attachType = 0; // 1 mark, 2 cursive
};

// The glyph buffer: `info` holds the current glyphs; while substituting,
// processed glyphs go to `out` and are swapped back at the end of a pass.
class Buffer {
public:
    std::vector<GlyphInfo> info;
    std::vector<GlyphInfo> out;
    std::vector<GlyphPosition> pos;
    std::size_t idx = 0;
    bool haveOutput = false;
    bool backward = false; // right-to-left (or bottom-to-top) text
    std::uint32_t serial = 0;
    int maxOps = 0; // work budget against hostile fonts
    std::uint32_t randomState = 1;

    std::size_t len() const noexcept { return info.size(); }
    GlyphInfo &cur() noexcept { return info[idx]; }
    std::size_t backtrackLen() const noexcept { return haveOutput ? out.size() : idx; }
    std::size_t lookaheadLen() const noexcept { return info.size() - idx; }

    void clearOutput();
    void swapBuffers();
    void nextGlyph();
    void skipGlyph() { ++idx; }
    void replaceGlyph(GlyphId g);
    void outputGlyph(GlyphId g); // a copy of cur() with glyph g, without advancing
    void deleteGlyph();
    bool moveTo(std::size_t i);
    void mergeClusters(std::size_t start, std::size_t end);
    void mergeOutClusters(std::size_t start, std::size_t end);
    void reverseRange(std::size_t start, std::size_t end);
    void reverse() { reverseRange(0, info.size()); }
    std::uint8_t allocateLigId();
};

// Glyph properties from GDEF (or none).
class Gdef {
public:
    void init(const FontFace &face);
    [[nodiscard]] bool hasGlyphClasses() const noexcept { return m_classDef != 0; }
    [[nodiscard]] std::uint16_t glyphProps(GlyphId g) const noexcept;
    [[nodiscard]] bool markSetCovers(std::uint32_t set, GlyphId g) const noexcept;

private:
    void lookupsOf(std::uint32_t featureIndex, std::vector<std::uint16_t> &lookups) const;

    Span<const std::byte> m_table;
    std::uint32_t m_classDef = 0;
    std::uint32_t m_markAttachClassDef = 0;
    std::uint32_t m_markGlyphSets = 0;
};

enum class TableKind : std::uint8_t { Gsub, Gpos };

struct PlannedLookup {
    std::uint16_t index;
    std::uint32_t mask;
    bool autoZwnj;
    bool autoZwj;
    bool random = false; // the 'rand' feature: alternate 255 picks at random
};

// A GSUB or GPOS table with lookups selected for a script and language.
class LayoutTable {
public:
    void init(const FontFace &face, TableKind kind);
    // A table from bytes (kept by the caller), e.g. a synthesised one.
    void init(Span<const std::byte> table, TableKind kind);
    [[nodiscard]] bool present() const noexcept { return !m_table.empty(); }
    [[nodiscard]] TableKind kind() const noexcept { return m_kind; }

    // Chooses the script (trying `scriptTags` in order, then DFLT, dflt,
    // latn) and language system (`language`, or the default).
    void selectScript(Span<const std::uint32_t> scriptTags, std::uint32_t language);
    // The lookups of `feature` in the selected language system (false if
    // the language system does not list it).
    bool featureLookups(std::uint32_t feature, std::vector<std::uint16_t> &lookups) const;
    // The lookups of the language system's required feature (none if none).
    void requiredFeatureLookups(std::vector<std::uint16_t> &lookups) const;
    [[nodiscard]] std::uint32_t requiredFeatureTag() const noexcept;
    // The script tag selectScript() chose (0 if none).
    [[nodiscard]] std::uint32_t chosenScript() const noexcept { return m_chosenScript; }
    // True if one of the requested script tags was found (not a fallback).
    [[nodiscard]] bool foundScript() const noexcept { return m_foundScript; }
    [[nodiscard]] std::uint32_t lookupCount() const noexcept;

    Span<const std::byte> table() const noexcept { return m_table; }
    std::uint32_t lookupList() const noexcept { return m_lookupList; }

private:
    void lookupsOf(std::uint32_t featureIndex, std::vector<std::uint16_t> &lookups) const;

    Span<const std::byte> m_table;
    TableKind m_kind = TableKind::Gsub;
    std::uint32_t m_scriptList = 0;
    std::uint32_t m_featureList = 0;
    std::uint32_t m_lookupList = 0;
    std::uint32_t m_chosenScript = 0;
    bool m_foundScript = false;
    std::uint32_t m_langSys = 0; // selected language system (offset), 0 if none
};

// Applies one stage's lookups to the buffer.
void applyLookups(const FontFace &face, const Gdef &gdef, const LayoutTable &table, Span<const PlannedLookup> lookups,
                  Buffer &buffer);

// After GPOS: attachment offsets accumulate along their chains.
void propagateAttachments(Buffer &buffer);

// Legacy 'kern' table (format 0), for fonts without GPOS kerning.
bool applyKernTable(const FontFace &face, const Gdef &gdef, std::uint32_t mask, Buffer &buffer);
// About the font's 'kern' table: present (the Microsoft version, with
// subtables); with cross-stream subtables; with state-machine subtables.
bool hasKernTable(const FontFace &face);
bool hasCrossStreamKerning(const FontFace &face);
bool hasMachineKerning(const FontFace &face);

// Ligature properties.
inline std::uint8_t ligId(const GlyphInfo &g) { return static_cast<std::uint8_t>(g.ligProps >> 5); }
inline bool ligIsBase(const GlyphInfo &g) { return (g.ligProps & 0x10) != 0; }
inline unsigned ligComp(const GlyphInfo &g) { return ligIsBase(g) ? 0u : (g.ligProps & 0x0Fu); }
inline unsigned ligNumComps(const GlyphInfo &g) {
    return ((g.glyphProps & kLigature) && ligIsBase(g)) ? (g.ligProps & 0x0Fu) : 1u;
}
inline void setLigPropsForLigature(GlyphInfo &g, unsigned id, unsigned comps) {
    g.ligProps = static_cast<std::uint8_t>((id << 5) | 0x10 | (comps & 0x0F));
}
inline void setLigPropsForMark(GlyphInfo &g, unsigned id, unsigned comp) {
    g.ligProps = static_cast<std::uint8_t>((id << 5) | (comp & 0x0F));
}
inline bool isMark(const GlyphInfo &g) { return (g.glyphProps & kMark) != 0; }
inline bool isLigature(const GlyphInfo &g) { return (g.glyphProps & kLigature) != 0; }
inline bool isBaseGlyph(const GlyphInfo &g) { return (g.glyphProps & kBaseGlyph) != 0; }
inline bool isMultiplied(const GlyphInfo &g) { return (g.glyphProps & kMultiplied) != 0; }
inline bool isSubstituted(const GlyphInfo &g) { return (g.glyphProps & kSubstituted) != 0; }
inline bool isDefaultIgnorable(const GlyphInfo &g) { return (g.flags & kIgnorable) && !isSubstituted(g); }

} // namespace cfw::ot
