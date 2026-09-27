#include "cfw/text/Unicode.h"

#include <algorithm>
#include <cstdint>

namespace cfw::unicode {

namespace data {

struct Record {
    std::uint8_t generalCategory;
    std::uint8_t script;
    std::uint8_t bidiClass;
    std::uint8_t lineBreak;
    std::uint8_t graphemeAndConjunct; // grapheme break | Indic_Conjunct_Break << 5
    std::uint8_t joiningAndWidth;     // joining type | east asian width << 3
    std::uint8_t flags;
    std::uint8_t combiningClass;
};

struct Pair {
    char32_t from;
    char32_t to;
};

struct Triple {
    char32_t a;
    char32_t b;
    char32_t c;
};

} // namespace data

} // namespace cfw::unicode

#include "UnicodeTables.inc"

namespace cfw::unicode {

namespace {

const data::Record &record(char32_t c) noexcept {
    if (c > 0x10FFFF) {
        c = 0x10FFFF; // a noncharacter: unassigned properties
    }
    const std::uint32_t block = data::kStage1[c >> data::kShift];
    const std::uint32_t offset = (block << data::kShift) | (c & ((1u << data::kShift) - 1u));
    return data::kRecords[data::kStage2[offset]];
}

char32_t lookup(const data::Pair *begin, const data::Pair *end, char32_t c) noexcept {
    const data::Pair *it = std::lower_bound(begin, end, c, [](const data::Pair &p, char32_t v) { return p.from < v; });
    return it != end && it->from == c ? it->to : c;
}

} // namespace

Properties properties(char32_t c) noexcept {
    const data::Record &r = record(c);
    return {
        static_cast<GeneralCategory>(r.generalCategory),
        static_cast<Script>(r.script),
        static_cast<BidiClass>(r.bidiClass),
        static_cast<LineBreakClass>(r.lineBreak),
        static_cast<GraphemeBreak>(r.graphemeAndConjunct & 31u),
        static_cast<IndicConjunctBreak>(r.graphemeAndConjunct >> 5),
        static_cast<JoiningType>(r.joiningAndWidth & 7u),
        static_cast<EastAsianWidth>(r.joiningAndWidth >> 3),
        static_cast<BracketType>((r.flags >> 3) & 3u),
        (r.flags & 1u) != 0,
        (r.flags & 2u) != 0,
        (r.flags & 4u) != 0,
        r.combiningClass,
    };
}

bool isMark(GeneralCategory gc) noexcept {
    return gc == GeneralCategory::Mn || gc == GeneralCategory::Mc || gc == GeneralCategory::Me;
}

char32_t pairedBracket(char32_t c) noexcept {
    return lookup(std::begin(data::kBrackets), std::end(data::kBrackets), c);
}

char32_t mirrored(char32_t c) noexcept { return lookup(std::begin(data::kMirrors), std::end(data::kMirrors), c); }

namespace {

// Hangul syllables decompose and compose algorithmically (Unicode 3.12).
constexpr char32_t kSBase = 0xAC00;
constexpr char32_t kLBase = 0x1100;
constexpr char32_t kVBase = 0x1161;
constexpr char32_t kTBase = 0x11A7;
constexpr char32_t kLCount = 19;
constexpr char32_t kVCount = 21;
constexpr char32_t kTCount = 28;
constexpr char32_t kNCount = kVCount * kTCount;
constexpr char32_t kSCount = kLCount * kNCount;

} // namespace

bool decompose(char32_t c, char32_t &a, char32_t &b) noexcept {
    if (c >= kSBase && c < kSBase + kSCount) {
        const char32_t s = c - kSBase;
        if (s % kTCount != 0) {
            a = kSBase + s / kTCount * kTCount; // LV
            b = kTBase + s % kTCount;
        } else {
            a = kLBase + s / kNCount;
            b = kVBase + s % kNCount / kTCount;
        }
        return true;
    }
    const data::Triple *end = std::end(data::kDecompositions);
    const data::Triple *it = std::lower_bound(std::begin(data::kDecompositions), end, c,
                                              [](const data::Triple &t, char32_t v) { return t.a < v; });
    if (it == end || it->a != c) {
        return false;
    }
    a = it->b;
    b = it->c;
    return true;
}

bool compose(char32_t a, char32_t b, char32_t &composite) noexcept {
    if (a >= kLBase && a < kLBase + kLCount && b >= kVBase && b < kVBase + kVCount) {
        composite = kSBase + ((a - kLBase) * kVCount + (b - kVBase)) * kTCount;
        return true;
    }
    if (a >= kSBase && a < kSBase + kSCount && (a - kSBase) % kTCount == 0 && b > kTBase && b < kTBase + kTCount) {
        composite = a + (b - kTBase);
        return true;
    }
    const data::Triple *end = std::end(data::kCompositions);
    const data::Triple *it = std::lower_bound(std::begin(data::kCompositions), end, std::make_pair(a, b),
                                              [](const data::Triple &t, const std::pair<char32_t, char32_t> &v) {
                                                  return t.a < v.first || (t.a == v.first && t.b < v.second);
                                              });
    if (it == end || it->a != a || it->b != b) {
        return false;
    }
    composite = it->c;
    return true;
}

const char *iso15924(Script script) noexcept {
    const auto i = static_cast<std::size_t>(script);
    return i < std::size(data::kScriptCodes) ? data::kScriptCodes[i] : "Zzzz";
}

std::optional<Script> scriptFromIso15924(StringView code) noexcept {
    if (code.size() != 4) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < std::size(data::kScriptCodes); ++i) {
        bool same = true;
        for (std::size_t k = 0; k < 4; ++k) {
            const char a = data::kScriptCodes[i][k];
            char b = code[k];
            if (k == 0 && b >= 'a' && b <= 'z') {
                b = static_cast<char>(b - 'a' + 'A');
            } else if (k > 0 && b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b - 'A' + 'a');
            }
            same = same && a == b;
        }
        if (same) {
            return static_cast<Script>(i);
        }
    }
    return std::nullopt;
}

} // namespace cfw::unicode
