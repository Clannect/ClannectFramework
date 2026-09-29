// The Indic, Khmer and Myanmar shaping engines: syllables, then reordering
// between GSUB stages, as HarfBuzz (and before it Uniscribe) does it, so
// fonts made for those engines come out right. Each step names the rule of
// Microsoft's script development specs it implements; where HarfBuzz
// deliberately departs from them to match Uniscribe, so does this.

#include "Syllabic.h"

#include <algorithm>
#include <cstring>

namespace cfw::syllabic {

namespace {

using namespace mcat;

constexpr std::uint64_t flag(unsigned c) { return std::uint64_t{1} << c; }

// HarfBuzz's machine numbers for the table's categories.
constexpr std::uint8_t kMachine[] = {X,  C,  V,  N,  H,  ZWNJ, ZWJ,   M,      SM,     A,      A,  PLACEHOLDER, DOTTEDCIRCLE,
                                     RS, MPst, Repha, Ra, CM, Symbol, CS, SMPst, VAbv, VBlw, VPre, VPst, Robatic, Xgroup,
                                     Ygroup, IV, As, DB, GB, MH, MR, MW, MY, PT, VS, ML};
static_assert(sizeof kMachine == kIndicCatCount);

constexpr std::uint8_t pos(IndicPos p) { return static_cast<std::uint8_t>(p); }
constexpr std::uint8_t kPosStart = pos(IndicPos::START);
constexpr std::uint8_t kPosRaToBecomeReph = pos(IndicPos::RA_TO_BECOME_REPH);
constexpr std::uint8_t kPosPreM = pos(IndicPos::PRE_M);
constexpr std::uint8_t kPosPreC = pos(IndicPos::PRE_C);
constexpr std::uint8_t kPosBaseC = pos(IndicPos::BASE_C);
constexpr std::uint8_t kPosAfterMain = pos(IndicPos::AFTER_MAIN);
constexpr std::uint8_t kPosBeforeSub = pos(IndicPos::BEFORE_SUB);
constexpr std::uint8_t kPosBelowC = pos(IndicPos::BELOW_C);
constexpr std::uint8_t kPosAfterSub = pos(IndicPos::AFTER_SUB);
constexpr std::uint8_t kPosPostC = pos(IndicPos::POST_C);
constexpr std::uint8_t kPosAfterPost = pos(IndicPos::AFTER_POST);
constexpr std::uint8_t kPosSmvd = pos(IndicPos::SMVD);
constexpr std::uint8_t kPosEnd = pos(IndicPos::END);

bool ligated(const ot::GlyphInfo &g) { return (g.glyphProps & ot::kLigated) != 0; }
bool multiplied(const ot::GlyphInfo &g) { return (g.glyphProps & ot::kMultiplied) != 0; }
bool ligatedAndDidntMultiply(const ot::GlyphInfo &g) { return ligated(g) && !multiplied(g); }

// A category test that fails for glyphs that ligated ("all bets are off").
bool isOneOf(const ot::GlyphInfo &g, std::uint64_t flags) { return !ligated(g) && (flag(g.category) & flags) != 0; }

constexpr std::uint64_t kJoiners = flag(ZWJ) | flag(ZWNJ);
constexpr std::uint64_t kIndicConsonants =
    flag(C) | flag(CS) | flag(Ra) | flag(CM) | flag(V) | flag(PLACEHOLDER) | flag(DOTTEDCIRCLE);

bool isConsonant(const ot::GlyphInfo &g) { return isOneOf(g, kIndicConsonants); }
bool isJoiner(const ot::GlyphInfo &g) { return isOneOf(g, kJoiners); }
bool isHalant(const ot::GlyphInfo &g) { return isOneOf(g, flag(H)); }

// Marks syllables with a scanner; `types` gives each pattern's syllable type.
bool findSyllables(ot::Buffer &b, const Scanner &scanner, const std::uint8_t *types, std::uint8_t brokenType) {
    const std::size_t n = b.len();
    std::vector<std::uint8_t> cats(n);
    for (std::size_t i = 0; i < n; ++i) {
        cats[i] = b.info[i].category;
    }
    bool broken = false;
    unsigned serial = 1;
    for (std::size_t p = 0; p < n;) {
        std::size_t length = 0;
        const int pattern = scanner.match(cats.data() + p, n - p, length);
        const std::uint8_t type = types[pattern < 0 ? 0 : pattern];
        length = std::max<std::size_t>(length, 1);
        broken = broken || type == brokenType;
        for (std::size_t i = p; i < p + length; ++i) {
            b.info[i].syllable = static_cast<std::uint8_t>(serial << 4 | type);
        }
        serial = serial == 15 ? 1 : serial + 1;
        p += length;
    }
    return broken;
}

// Moves info[from] to `to` (to <= from), shifting what is between up by one.
void moveBack(std::vector<ot::GlyphInfo> &info, std::size_t from, std::size_t to) {
    const ot::GlyphInfo t = info[from];
    std::memmove(&info[to + 1], &info[to], (from - to) * sizeof(ot::GlyphInfo));
    info[to] = t;
}
// Moves info[from] to `to` (to >= from), shifting what is between down by one.
void moveForward(std::vector<ot::GlyphInfo> &info, std::size_t from, std::size_t to) {
    const ot::GlyphInfo t = info[from];
    std::memmove(&info[from], &info[from + 1], (to - from) * sizeof(ot::GlyphInfo));
    info[to] = t;
}

// ---- Indic ----

const Scanner &indicScanner() {
    static const Scanner scanner = [] {
        const Re c = cat(C, Ra);
        const Re n = seq(opt(seq(opt(cat(ZWNJ)), cat(RS))), opt(seq(cat(N), opt(cat(N)))));
        const Re z = cat(ZWJ, ZWNJ);
        const Re reph = alt(seq(cat(Ra), cat(H)), cat(Repha));
        const Re sm = cat(SM, SMPst);
        const Re cn = seq(c, opt(cat(ZWJ)), opt(n));
        const Re symbol = seq(cat(Symbol), opt(cat(N)));
        const Re matraGroup = seq(star(z), alt(cat(M), seq(opt(sm), cat(MPst))), opt(cat(N)), opt(cat(H)));
        const Re syllableTail = seq(opt(seq(opt(z), sm, opt(sm), opt(cat(ZWNJ)))), star(cat(A)));
        const Re halantGroup = seq(opt(z), cat(H), opt(seq(cat(ZWJ), opt(cat(N)))));
        const Re finalHalantGroup = alt(halantGroup, seq(cat(H), cat(ZWNJ)));
        const Re medialGroup = opt(cat(CM));
        const Re halantOrMatraGroup = alt(finalHalantGroup, star(matraGroup));
        const Re complexTail = seq(star(seq(halantGroup, cn)), medialGroup, halantOrMatraGroup, syllableTail);
        return Scanner({
            seq(opt(cat(Repha, CS)), cn, complexTail),                                           // consonant
            seq(opt(reph), cat(V), opt(n), alt(cat(ZWJ), complexTail)),                          // vowel
            seq(alt(seq(opt(cat(Repha, CS)), cat(PLACEHOLDER)), seq(opt(reph), cat(DOTTEDCIRCLE))), opt(n),
                complexTail),                                                                    // standalone
            seq(symbol, syllableTail),                                                           // symbol
            cat(SMPst),                                                                          // non-Indic
            seq(opt(reph), opt(n), complexTail),                                                 // broken
            any(),                                                                               // non-Indic
        });
    }();
    return scanner;
}

constexpr std::uint8_t kIndicTypes[] = {IndicConsonant, IndicVowel,  IndicStandalone, IndicSymbol,
                                        IndicNonIndic,  IndicBroken, IndicNonIndic};

// Whether a consonant takes a below-base or post-base form in this font:
// the blwf, vatu, pstf and pref lookups tried on virama+consonant and
// consonant+virama (fonts copied the first spec's order into the second).
std::uint8_t consonantPosition(const IndicPlan &plan, GlyphId consonant) {
    const GlyphId glyphs[3] = {plan.viramaGlyph, consonant, plan.viramaGlyph};
    const auto either = [&](const std::vector<std::uint16_t> &lookups) {
        return plan.wouldSubstitute(lookups, Span<const GlyphId>(glyphs, 2)) ||
               plan.wouldSubstitute(lookups, Span<const GlyphId>(glyphs + 1, 2));
    };
    if (either(plan.blwf) || either(plan.vatu)) {
        return kPosBelowC;
    }
    if (either(plan.pstf) || either(plan.pref)) {
        return kPosPostC;
    }
    return kPosBaseC;
}

void initialReorderingConsonantSyllable(const IndicPlan &plan, ot::Buffer &b, std::size_t start, std::size_t end) {
    std::vector<ot::GlyphInfo> &info = b.info;
    using unicode::Script;

    // Kannada: Ra,H,ZWJ behaves like Ra,ZWJ,H (legacy usage).
    if (plan.script == Script::Kannada && start + 3 <= end && isOneOf(info[start], flag(Ra)) &&
        isOneOf(info[start + 1], flag(H)) && isOneOf(info[start + 2], flag(ZWJ))) {
        b.mergeClusters(start + 1, start + 3);
        std::swap(info[start + 1], info[start + 2]);
    }

    // 1. The base consonant: from the end, the first consonant without a
    // below-base or post-base form, or the first consonant. A leading
    // Ra,H that forms a reph is not a candidate.
    std::size_t base = end;
    bool hasReph = false;
    {
        std::size_t limit = start;
        if (plan.rphfMask && start + 3 <= end &&
            ((plan.rephMode == IndicPlan::RephMode::Implicit && !isJoiner(info[start + 2])) ||
             (plan.rephMode == IndicPlan::RephMode::Explicit && info[start + 2].category == ZWJ))) {
            const GlyphId glyphs[3] = {info[start].glyph, info[start + 1].glyph,
                                       plan.rephMode == IndicPlan::RephMode::Explicit ? info[start + 2].glyph : GlyphId{0}};
            if (plan.wouldSubstitute(plan.rphf, Span<const GlyphId>(glyphs, 2)) ||
                (plan.rephMode == IndicPlan::RephMode::Explicit &&
                 plan.wouldSubstitute(plan.rphf, Span<const GlyphId>(glyphs, 3)))) {
                limit += 2;
                while (limit < end && isJoiner(info[limit])) {
                    ++limit;
                }
                base = start;
                hasReph = true;
            }
        } else if (plan.rephMode == IndicPlan::RephMode::LogRepha && info[start].category == Repha) {
            limit += 1;
            while (limit < end && isJoiner(info[limit])) {
                ++limit;
            }
            base = start;
            hasReph = true;
        }

        std::size_t i = end;
        bool seenBelow = false;
        do {
            --i;
            if (isConsonant(info[i])) {
                if (info[i].position != kPosBelowC && (info[i].position != kPosPostC || seenBelow)) {
                    base = i;
                    break;
                }
                if (info[i].position == kPosBelowC) {
                    seenBelow = true;
                }
                base = i;
            } else if (start < i && info[i].category == ZWJ && info[i - 1].category == H) {
                // A ZWJ after a halant asks for a half form and ends the search.
                break;
            }
        } while (i > limit);

        // With no other consonant, the Ra is the base and forms no reph.
        if (hasReph && base == start && limit - base <= 2) {
            hasReph = false;
        }
    }

    // 2 and 3 (split matras, nukta order): normalisation did them.

    for (std::size_t i = start; i < base; ++i) {
        info[i].position = std::min(kPosPreC, info[i].position);
    }
    if (base < end) {
        info[base].position = kPosBaseC;
    }
    if (hasReph) {
        info[start].position = kPosRaToBecomeReph;
    }

    // Old-spec fonts: the first post-base halant moves after the last
    // consonant (in Kannada only if no halant is there already).
    if (plan.oldSpec) {
        const bool disallowDoubleHalants = plan.script == Script::Kannada;
        for (std::size_t i = base + 1; i < end; ++i) {
            if (info[i].category == H) {
                std::size_t j = end - 1;
                for (; j > i; --j) {
                    if (isConsonant(info[j]) || (disallowDoubleHalants && info[j].category == H)) {
                        break;
                    }
                }
                if (info[j].category != H && j > i) {
                    moveForward(info, i, j);
                }
                break;
            }
        }
    }

    // Miscellaneous marks move with the character before them.
    {
        std::uint8_t lastPos = kPosStart;
        for (std::size_t i = start; i < end; ++i) {
            if (flag(info[i].category) & (kJoiners | flag(N) | flag(RS) | flag(CM) | flag(H))) {
                info[i].position = lastPos;
                if (info[i].category == H && info[i].position == kPosPreM) {
                    // Uniscribe does not move the halant with a left matra.
                    for (std::size_t j = i; j > start; --j) {
                        if (info[j - 1].position != kPosPreM) {
                            info[i].position = info[j - 1].position;
                            break;
                        }
                    }
                }
            } else if (info[i].position != kPosSmvd) {
                if (info[i].category == MPst && i > start && info[i - 1].category == SM) {
                    info[i - 1].position = info[i].position;
                }
                lastPos = info[i].position;
            }
        }
    }
    // Post-base consonants own what is before them since the last consonant or matra.
    {
        std::size_t last = base;
        for (std::size_t i = base + 1; i < end; ++i) {
            if (isConsonant(info[i])) {
                for (std::size_t j = last + 1; j < i; ++j) {
                    if (info[j].position < kPosSmvd) {
                        info[j].position = info[i].position;
                    }
                }
                last = i;
            } else if (flag(info[i].category) & (flag(M) | flag(MPst))) {
                last = i;
            }
        }
    }

    {
        // The syllable byte records each glyph's original index for the
        // cluster merging after the sort.
        const std::uint8_t syllable = info[start].syllable;
        for (std::size_t i = start; i < end; ++i) {
            info[i].syllable = static_cast<std::uint8_t>(i - start);
        }
        std::stable_sort(info.begin() + static_cast<std::ptrdiff_t>(start), info.begin() + static_cast<std::ptrdiff_t>(end),
                         [](const ot::GlyphInfo &x, const ot::GlyphInfo &y) { return x.position < y.position; });

        // The base again; left matras in logical order.
        std::size_t firstLeftMatra = end;
        std::size_t lastLeftMatra = end;
        base = end;
        for (std::size_t i = start; i < end; ++i) {
            if (info[i].position == kPosBaseC) {
                base = i;
                break;
            }
            if (info[i].position == kPosPreM) {
                if (firstLeftMatra == end) {
                    firstLeftMatra = i;
                }
                lastLeftMatra = i;
            }
        }
        if (firstLeftMatra < lastLeftMatra) {
            b.reverseRange(firstLeftMatra, lastLeftMatra + 1);
            std::size_t i = firstLeftMatra;
            for (std::size_t j = i; j <= lastLeftMatra; ++j) {
                if (flag(info[j].category) & (flag(M) | flag(MPst))) {
                    b.reverseRange(i, j + 1);
                    i = j + 1;
                }
            }
        }

        // Clusters after the base merge where the sort moved things.
        if (plan.oldSpec || end - start > 127) {
            b.mergeClusters(base, end);
        } else {
            for (std::size_t i = base; i < end; ++i) {
                if (info[i].syllable != 255) {
                    std::size_t lo = i;
                    std::size_t hi = i;
                    std::size_t j = start + info[i].syllable;
                    while (j != i) {
                        lo = std::min(lo, j);
                        hi = std::max(hi, j);
                        const std::size_t next = start + info[j].syllable;
                        info[j].syllable = 255;
                        j = next;
                    }
                    b.mergeClusters(std::max(base, lo), hi + 1);
                }
            }
        }
        for (std::size_t i = start; i < end; ++i) {
            info[i].syllable = syllable;
        }
    }

    // Feature masks.
    for (std::size_t i = start; i < end && info[i].position == kPosRaToBecomeReph; ++i) {
        info[i].mask |= plan.rphfMask;
    }
    std::uint32_t mask = plan.halfMask;
    if (!plan.oldSpec && !plan.blwfPostOnly) {
        mask |= plan.blwfMask;
    }
    for (std::size_t i = start; i < base; ++i) {
        info[i].mask |= mask;
    }
    mask = plan.blwfMask | plan.abvfMask | plan.pstfMask;
    for (std::size_t i = base + 1; i < end; ++i) {
        info[i].mask |= mask;
    }

    // Old-spec Devanagari: the eyelash Ra takes below-base forms before the base too.
    if (plan.oldSpec && plan.script == Script::Devanagari) {
        for (std::size_t i = start; i + 1 < base; ++i) {
            if (info[i].category == Ra && info[i + 1].category == H && (i + 2 == base || info[i + 2].category != ZWJ)) {
                info[i].mask |= plan.blwfMask;
                info[i + 1].mask |= plan.blwfMask;
            }
        }
    }

    // A halant,Ra pair the font makes a pre-base-reordering form of.
    constexpr std::size_t kPrefLength = 2;
    if (plan.prefMask && base + kPrefLength < end) {
        for (std::size_t i = base + 1; i + kPrefLength - 1 < end; ++i) {
            const GlyphId glyphs[2] = {info[i].glyph, info[i + 1].glyph};
            if (plan.wouldSubstitute(plan.pref, Span<const GlyphId>(glyphs, 2))) {
                info[i].mask |= plan.prefMask;
                info[i + 1].mask |= plan.prefMask;
                break;
            }
        }
    }

    // A ZWNJ disables half forms before it.
    for (std::size_t i = start + 1; i < end; ++i) {
        if (isJoiner(info[i])) {
            const bool nonJoiner = info[i].category == ZWNJ;
            std::size_t j = i;
            do {
                --j;
                if (nonJoiner) {
                    info[j].mask &= ~plan.halfMask;
                }
            } while (j > start && !isConsonant(info[j]));
        }
    }
}

void finalReorderingSyllable(const IndicPlan &plan, ot::Buffer &b, std::size_t start, std::size_t end) {
    std::vector<ot::GlyphInfo> &info = b.info;
    using unicode::Script;

    // A virama that ligated and multiplied (decomposed by the font) is a halant again.
    if (plan.viramaGlyph) {
        for (std::size_t i = start; i < end; ++i) {
            if (info[i].glyph == plan.viramaGlyph && ligated(info[i]) && multiplied(info[i])) {
                info[i].category = H;
                info[i].glyphProps &= static_cast<std::uint16_t>(~(ot::kLigated | ot::kMultiplied));
            }
        }
    }

    // 4. Final reordering. The base again.
    bool tryPref = plan.prefMask != 0;
    std::size_t base = start;
    for (; base < end; ++base) {
        if (info[base].position >= kPosBaseC) {
            if (tryPref && base + 1 < end) {
                for (std::size_t i = base + 1; i < end; ++i) {
                    if (info[i].mask & plan.prefMask) {
                        if (!(ot::isSubstituted(info[i]) && ligatedAndDidntMultiply(info[i]))) {
                            // A pref candidate that formed nothing: the base is around here.
                            base = i;
                            while (base < end && isHalant(info[base])) {
                                ++base;
                            }
                            if (base < end) {
                                info[base].position = kPosBaseC;
                            }
                            tryPref = false;
                        }
                        break;
                    }
                }
                if (base == end) {
                    break;
                }
            }
            // Malayalam: skip over unformed below (not post) forms.
            if (plan.script == Script::Malayalam) {
                for (std::size_t i = base + 1; i < end; ++i) {
                    while (i < end && isJoiner(info[i])) {
                        ++i;
                    }
                    if (i == end || !isHalant(info[i])) {
                        break;
                    }
                    ++i;
                    while (i < end && isJoiner(info[i])) {
                        ++i;
                    }
                    if (i < end && isConsonant(info[i]) && info[i].position == kPosBelowC) {
                        base = i;
                        info[base].position = kPosBaseC;
                    }
                }
            }
            if (start < base && info[base].position > kPosBaseC) {
                --base;
            }
            break;
        }
    }
    if (base == end && start < base && isOneOf(info[base - 1], flag(ZWJ))) {
        --base;
    }
    if (base < end) {
        while (start < base && isOneOf(info[base], flag(N) | flag(H))) {
            --base;
        }
    }

    // Pre-base matras move to after the last standalone halant before the
    // base (a halant followed by ZWJ does not count).
    if (start + 1 < end && start < base) {
        std::size_t newPos = base == end ? base - 2 : base - 1;
        if (plan.script != Script::Malayalam && plan.script != Script::Tamil) {
            for (;;) {
                while (newPos > start && !isOneOf(info[newPos], flag(M) | flag(MPst) | flag(H))) {
                    --newPos;
                }
                if (isHalant(info[newPos]) && info[newPos].position != kPosPreM) {
                    if (newPos + 1 < end && info[newPos + 1].category == ZWJ && newPos > start) {
                        --newPos;
                        continue; // keep searching
                    }
                } else {
                    newPos = start; // no move
                }
                break;
            }
        }
        if (start < newPos && info[newPos].position != kPosPreM) {
            for (std::size_t i = newPos; i > start; --i) {
                if (info[i - 1].position == kPosPreM) {
                    const std::size_t oldPos = i - 1;
                    if (oldPos < base && base <= newPos) {
                        --base;
                    }
                    moveForward(info, oldPos, newPos);
                    b.mergeClusters(newPos, std::min(end, base + 1));
                    --newPos;
                }
            }
        } else {
            for (std::size_t i = start; i < base; ++i) {
                if (info[i].position == kPosPreM) {
                    b.mergeClusters(i, std::min(end, base + 1));
                    break;
                }
            }
        }
    }

    // The reph: a Ra,H sequence moves only if it formed the reph; an
    // encoded repha only if it did not (the font placed it itself).
    if (start + 1 < end && info[start].position == kPosRaToBecomeReph &&
        ((info[start].category == Repha) != ligatedAndDidntMultiply(info[start]))) {
        std::size_t newRephPos = start;
        const std::uint8_t rephPos = plan.rephPosition;
        // After the first explicit halant between the reph and the base
        // (and a joiner after it).
        const auto afterHalant = [&]() {
            newRephPos = start + 1;
            while (newRephPos < base && !isHalant(info[newRephPos])) {
                ++newRephPos;
            }
            if (newRephPos < base && isHalant(info[newRephPos])) {
                if (newRephPos + 1 < base && isJoiner(info[newRephPos + 1])) {
                    ++newRephPos;
                }
                return true;
            }
            return false;
        };
        bool found = false;
        if (rephPos != kPosAfterPost) {
            // 2. After the first halant between the reph and the base.
            found = afterHalant();
            // 3. After the main consonant.
            if (!found && rephPos == kPosAfterMain) {
                newRephPos = base;
                while (newRephPos + 1 < end && info[newRephPos + 1].position <= kPosAfterMain) {
                    ++newRephPos;
                }
                found = newRephPos < end;
            }
            // 4. Before post-base consonants.
            if (!found && rephPos == kPosAfterSub) {
                newRephPos = base;
                while (newRephPos + 1 < end && !(flag(info[newRephPos + 1].position) &
                                                 (flag(kPosPostC) | flag(kPosAfterPost) | flag(kPosSmvd)))) {
                    ++newRephPos;
                }
                found = newRephPos < end;
            }
        }
        // 5. As step 2.
        if (!found) {
            found = afterHalant();
        }
        // 6. The end of the syllable (before a final halant after a matra).
        if (!found) {
            newRephPos = end - 1;
            while (newRephPos > start && info[newRephPos].position == kPosSmvd) {
                --newRephPos;
            }
            if (isHalant(info[newRephPos])) {
                for (std::size_t i = base + 1; i < newRephPos; ++i) {
                    if (flag(info[i].category) & (flag(M) | flag(MPst))) {
                        --newRephPos;
                    }
                }
            }
        }
        b.mergeClusters(start, newRephPos + 1);
        moveForward(info, start, newRephPos);
        if (start < base && base <= newRephPos) {
            --base;
        }
    }

    // Pre-base-reordering consonants the pref feature formed move where a
    // pre-base matra would, or before the base.
    if (tryPref && base + 1 < end) {
        for (std::size_t i = base + 1; i < end; ++i) {
            if (info[i].mask & plan.prefMask) {
                if (ligatedAndDidntMultiply(info[i])) {
                    std::size_t newPos = base;
                    if (plan.script != Script::Malayalam && plan.script != Script::Tamil) {
                        while (newPos > start && !isOneOf(info[newPos - 1], flag(M) | flag(MPst) | flag(H))) {
                            --newPos;
                        }
                    }
                    if (newPos > start && isHalant(info[newPos - 1])) {
                        if (newPos < end && isJoiner(info[newPos])) {
                            ++newPos;
                        }
                    }
                    const std::size_t oldPos = i;
                    b.mergeClusters(newPos, oldPos + 1);
                    moveBack(info, oldPos, newPos);
                    if (newPos <= base && base < oldPos) {
                        ++base;
                    }
                }
                break;
            }
        }
    }

    // A left matra at the start of a word takes its initial form.
    if (info[start].position == kPosPreM) {
        const auto gc = start ? info[start - 1].generalCategory : 0;
        // Cf through Mn in the category order: format, unassigned, private
        // use, surrogate, letters and marks.
        const bool wordStart = !start || !(gc >= static_cast<std::uint8_t>(unicode::GeneralCategory::Cf) &&
                                           gc <= static_cast<std::uint8_t>(unicode::GeneralCategory::Mn));
        if (wordStart) {
            info[start].mask |= plan.initMask;
        }
    }
}

// ---- Khmer ----

const Scanner &khmerScanner() {
    static const Scanner scanner = [] {
        const Re c = cat(C, Ra, V);
        const Re joiner = cat(ZWJ, ZWNJ);
        const Re cn = seq(c, opt(seq(opt(joiner), cat(Robatic))));
        const Re xgroup = star(seq(star(joiner), cat(Xgroup)));
        const Re ygroup = star(cat(Ygroup));
        const Re matraGroup = seq(opt(cat(VPre)), xgroup, opt(cat(VBlw)), xgroup, opt(seq(opt(joiner), cat(VAbv))), xgroup,
                                  opt(cat(VPst)));
        const Re syllableTail = seq(xgroup, matraGroup, xgroup, opt(seq(cat(H), c)), ygroup);
        const Re broken = seq(opt(cat(Robatic)), star(seq(cat(H), cn)), alt(cat(H), syllableTail));
        return Scanner({
            seq(alt(cn, cat(PLACEHOLDER), cat(DOTTEDCIRCLE)), broken), // consonant
            broken,                                                     // broken
            any(),                                                      // other
        });
    }();
    return scanner;
}

constexpr std::uint8_t kKhmerTypes[] = {KhmerConsonant, KhmerBroken, KhmerNonKhmer};

void reorderKhmerSyllable(const KhmerPlan &plan, ot::Buffer &b, std::size_t start, std::size_t end) {
    std::vector<ot::GlyphInfo> &info = b.info;
    const std::uint32_t postBase = plan.blwfMask | plan.abvfMask | plan.pstfMask;
    for (std::size_t i = start + 1; i < end; ++i) {
        info[i].mask |= postBase;
    }
    unsigned coengs = 0;
    for (std::size_t i = start + 1; i < end; ++i) {
        // Coeng,Ro (subscript type 2) moves before the base and takes pref.
        if (info[i].category == H && coengs <= 2 && i + 1 < end) {
            ++coengs;
            if (info[i + 1].category == Ra) {
                info[i].mask |= plan.prefMask;
                info[i + 1].mask |= plan.prefMask;
                b.mergeClusters(start, i + 2);
                const ot::GlyphInfo t0 = info[i];
                const ot::GlyphInfo t1 = info[i + 1];
                std::memmove(&info[start + 2], &info[start], (i - start) * sizeof(ot::GlyphInfo));
                info[start] = t0;
                info[start + 1] = t1;
                // What follows takes cfar (MS Khmer fonts tell the orders apart by it).
                if (plan.cfarMask) {
                    for (std::size_t j = i + 2; j < end; ++j) {
                        info[j].mask |= plan.cfarMask;
                    }
                }
                coengs = 2;
            }
        } else if (info[i].category == VPre) {
            // The left part of a matra moves to the start.
            b.mergeClusters(start, i + 1);
            moveBack(info, i, start);
        }
    }
}

// ---- Myanmar ----

const Scanner &myanmarScanner() {
    static const Scanner scanner = [] {
        const Re j = cat(ZWJ, ZWNJ);
        const Re k = seq(cat(Ra), cat(As), cat(H)); // kinzi
        const Re sm = cat(SM, SMPst);
        const Re c = cat(C, Ra);
        const Re medialGroup = seq(opt(cat(MY)), opt(cat(As)), opt(cat(MR)),
                                   opt(seq(alt(seq(cat(MW), opt(cat(MH)), opt(cat(ML))), seq(cat(MH), opt(cat(ML))), cat(ML)),
                                           opt(cat(As)))));
        const Re mainVowelGroup = seq(star(seq(cat(VPre), opt(cat(VS)))), star(cat(VAbv)), star(cat(VBlw)), star(cat(A)),
                                      opt(seq(cat(DB), opt(cat(As)))));
        const Re postVowelGroup = seq(cat(VPst), opt(cat(MH)), opt(cat(ML)), star(cat(As)), star(cat(VAbv)), star(cat(A)),
                                      opt(seq(cat(DB), opt(cat(As)))));
        const Re toneGroup = alt(sm, seq(cat(PT), star(cat(A)), opt(cat(DB)), opt(cat(As))));
        const Re complexTail =
            seq(star(cat(As)), medialGroup, mainVowelGroup, star(postVowelGroup), star(toneGroup), opt(j));
        const Re syllableTail = seq(star(seq(cat(H), alt(c, cat(IV)), opt(cat(VS)))), alt(cat(H), complexTail));
        return Scanner({
            seq(opt(alt(k, cat(CS))), alt(c, cat(IV), cat(GB), cat(DOTTEDCIRCLE)), opt(cat(VS)), syllableTail), // consonant
            alt(j, cat(SMPst)),                                                                               // other
            seq(opt(k), opt(cat(VS)), syllableTail),                                                          // broken
            any(),                                                                                            // other
        });
    }();
    return scanner;
}

constexpr std::uint8_t kMyanmarTypes[] = {MyanmarConsonant, MyanmarNonMyanmar, MyanmarBroken, MyanmarNonMyanmar};

constexpr std::uint64_t kMyanmarConsonants = flag(C) | flag(CS) | flag(Ra) | flag(IV) | flag(GB) | flag(DOTTEDCIRCLE);

void reorderMyanmarSyllable(ot::Buffer &b, std::size_t start, std::size_t end) {
    std::vector<ot::GlyphInfo> &info = b.info;
    std::size_t base = end;
    bool hasReph = false;
    {
        std::size_t limit = start;
        if (start + 3 <= end && info[start].category == Ra && info[start + 1].category == As &&
            info[start + 2].category == H) {
            limit += 3;
            base = start;
            hasReph = true;
        }
        if (!hasReph) {
            base = limit;
        }
        for (std::size_t i = limit; i < end; ++i) {
            if (isOneOf(info[i], kMyanmarConsonants)) {
                base = i;
                break;
            }
        }
    }

    std::size_t i = start;
    for (; i < start + (hasReph ? 3 : 0); ++i) {
        info[i].position = kPosAfterMain;
    }
    for (; i < base; ++i) {
        info[i].position = kPosPreC;
    }
    if (i < end) {
        info[i].position = kPosBaseC;
        ++i;
    }
    std::uint8_t p = kPosAfterMain;
    for (; i < end; ++i) {
        const std::uint8_t c = info[i].category;
        if (c == MR) { // pre-base reordering
            info[i].position = kPosPreC;
        } else if (c == VPre) { // left matra
            info[i].position = kPosPreM;
        } else if (c == VS) {
            info[i].position = info[i - 1].position;
        } else if (p == kPosAfterMain && c == VBlw) {
            p = kPosBelowC;
            info[i].position = p;
        } else if (p == kPosBelowC && c == A) {
            info[i].position = kPosBeforeSub;
        } else if (p == kPosBelowC && c == VBlw) {
            info[i].position = p;
        } else if (p == kPosBelowC) {
            p = kPosAfterSub;
            info[i].position = p;
        } else {
            info[i].position = p;
        }
    }

    // HarfBuzz's buffer sort: an insertion sort that merges the clusters it moves across.
    for (std::size_t k = start + 1; k < end; ++k) {
        std::size_t j = k;
        while (j > start && info[j - 1].position > info[k].position) {
            --j;
        }
        if (j == k) {
            continue;
        }
        b.mergeClusters(j, k + 1);
        moveBack(info, k, j);
    }

    // Left matras back in logical order.
    std::size_t firstLeftMatra = end;
    std::size_t lastLeftMatra = end;
    for (std::size_t k = start; k < end; ++k) {
        if (info[k].position == kPosPreM) {
            if (firstLeftMatra == end) {
                firstLeftMatra = k;
            }
            lastLeftMatra = k;
        }
    }
    if (firstLeftMatra < lastLeftMatra) {
        b.reverseRange(firstLeftMatra, lastLeftMatra + 1);
        std::size_t k = firstLeftMatra;
        for (std::size_t j = k; j <= lastLeftMatra; ++j) {
            if (info[j].category == VPre) {
                b.reverseRange(k, j + 1);
                k = j + 1;
            }
        }
    }
}

} // namespace

void setMachineCategories(ot::Buffer &b, bool positions) {
    for (ot::GlyphInfo &g : b.info) {
        g.category = kMachine[static_cast<std::size_t>(indicCategory(g.codepoint))];
        if (positions) {
            g.position = static_cast<std::uint8_t>(indicPosition(g.codepoint));
        }
    }
}

bool findIndicSyllables(ot::Buffer &b) { return findSyllables(b, indicScanner(), kIndicTypes, IndicBroken); }
bool findKhmerSyllables(ot::Buffer &b) { return findSyllables(b, khmerScanner(), kKhmerTypes, KhmerBroken); }
bool findMyanmarSyllables(ot::Buffer &b) { return findSyllables(b, myanmarScanner(), kMyanmarTypes, MyanmarBroken); }

void IndicPlan::configure(unicode::Script s, bool oldSpecTags) {
    using unicode::Script;
    struct Config {
        Script script;
        char32_t virama;
        IndicPos rephPos;
        RephMode rephMode;
        bool blwfPostOnly;
    };
    static constexpr Config kConfigs[] = {
        {Script::Devanagari, 0x094D, IndicPos::BEFORE_POST, RephMode::Implicit, false},
        {Script::Bengali, 0x09CD, IndicPos::AFTER_SUB, RephMode::Implicit, false},
        {Script::Gurmukhi, 0x0A4D, IndicPos::BEFORE_SUB, RephMode::Implicit, false},
        {Script::Gujarati, 0x0ACD, IndicPos::BEFORE_POST, RephMode::Implicit, false},
        {Script::Oriya, 0x0B4D, IndicPos::AFTER_MAIN, RephMode::Implicit, false},
        {Script::Tamil, 0x0BCD, IndicPos::AFTER_POST, RephMode::Implicit, false},
        {Script::Telugu, 0x0C4D, IndicPos::AFTER_POST, RephMode::Explicit, true},
        {Script::Kannada, 0x0CCD, IndicPos::AFTER_POST, RephMode::Implicit, true},
        {Script::Malayalam, 0x0D4D, IndicPos::AFTER_MAIN, RephMode::LogRepha, false},
    };
    script = s;
    oldSpec = false;
    rephPosition = static_cast<std::uint8_t>(IndicPos::BEFORE_POST);
    rephMode = RephMode::Implicit;
    blwfPostOnly = false;
    for (const Config &c : kConfigs) {
        if (c.script == s) {
            oldSpec = oldSpecTags;
            rephPosition = static_cast<std::uint8_t>(c.rephPos);
            rephMode = c.rephMode;
            blwfPostOnly = c.blwfPostOnly;
        }
    }
    // Windows matches without context for the second spec (except in
    // Malayalam), with it for the first.
    zeroContext = !oldSpec && s != Script::Malayalam;
}

bool IndicPlan::wouldSubstitute(const std::vector<std::uint16_t> &lookups, Span<const GlyphId> glyphs) const {
    if (!gsub) {
        return false;
    }
    for (const std::uint16_t l : lookups) {
        if (ot::wouldSubstitute(*gsub, l, glyphs, zeroContext)) {
            return true;
        }
    }
    return false;
}

void initialReorderingIndic(const FontFace &face, const IndicPlan &plan, ot::Buffer &b, bool broken) {
    if (plan.viramaGlyph) {
        for (ot::GlyphInfo &g : b.info) {
            if (g.position == kPosBaseC) {
                g.position = consonantPosition(plan, g.glyph);
            }
        }
    }
    if (broken) {
        insertDottedCircles(face, b, IndicBroken, DOTTEDCIRCLE, Repha, kPosEnd);
    }
    for (std::size_t start = 0; start < b.len();) {
        const std::size_t end = nextSyllable(b, start);
        const unsigned type = b.info[start].syllable & 0x0Fu;
        if (type != IndicSymbol && type != IndicNonIndic) {
            // Vowels, placeholders and dotted circles act as consonants.
            initialReorderingConsonantSyllable(plan, b, start, end);
        }
        start = end;
    }
}

void finalReorderingIndic(const IndicPlan &plan, ot::Buffer &b) {
    for (std::size_t start = 0; start < b.len();) {
        const std::size_t end = nextSyllable(b, start);
        finalReorderingSyllable(plan, b, start, end);
        start = end;
    }
}

void reorderKhmer(const FontFace &face, const KhmerPlan &plan, ot::Buffer &b, bool broken) {
    if (broken) {
        insertDottedCircles(face, b, KhmerBroken, DOTTEDCIRCLE, -1);
    }
    for (std::size_t start = 0; start < b.len();) {
        const std::size_t end = nextSyllable(b, start);
        const unsigned type = b.info[start].syllable & 0x0Fu;
        if (type == KhmerConsonant || type == KhmerBroken) {
            reorderKhmerSyllable(plan, b, start, end);
        }
        start = end;
    }
}

void reorderMyanmar(const FontFace &face, ot::Buffer &b, bool broken) {
    if (broken) {
        insertDottedCircles(face, b, MyanmarBroken, DOTTEDCIRCLE, -1);
    }
    for (std::size_t start = 0; start < b.len();) {
        const std::size_t end = nextSyllable(b, start);
        const unsigned type = b.info[start].syllable & 0x0Fu;
        if (type == MyanmarConsonant || type == MyanmarBroken) {
            reorderMyanmarSyllable(b, start, end);
        }
        start = end;
    }
}

} // namespace cfw::syllabic
