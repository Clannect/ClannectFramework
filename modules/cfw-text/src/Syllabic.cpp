#include "Syllabic.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "cfw/text/Unicode.h"

namespace cfw::syllabic {

namespace {

const std::uint8_t *record(char32_t u) noexcept {
    if (u > 0x10FFFF) {
        u = 0;
    }
    const std::uint16_t block = data::kStage1[u >> data::kShift];
    const std::size_t index = (std::size_t{block} << data::kShift) + (u & ((1u << data::kShift) - 1));
    return data::kRecords[data::kStage2[index]];
}

bool isUnicodeMark(const ot::GlyphInfo &g) {
    const auto gc = static_cast<unicode::GeneralCategory>(g.generalCategory);
    return gc == unicode::GeneralCategory::Mn || gc == unicode::GeneralCategory::Mc || gc == unicode::GeneralCategory::Me;
}

// ---- Regular expressions to DFA ----

struct Nfa {
    struct State {
        std::uint64_t set = 0; // categories leading to `to`
        int to = -1;
        std::vector<int> eps;
        int accept = -1;
    };
    std::vector<State> states;

    int add() {
        states.emplace_back();
        return static_cast<int>(states.size()) - 1;
    }
    // Thompson's construction: a fragment from `start` to a fresh end state.
    int build(const Re &r, int start) {
        switch (r.kind) {
        case Re::Kind::Set: {
            const int end = add();
            states[static_cast<std::size_t>(start)].set = r.set;
            states[static_cast<std::size_t>(start)].to = end;
            return end;
        }
        case Re::Kind::Seq: {
            int at = start;
            for (const Re &p : r.parts) {
                const int next = add();
                states[static_cast<std::size_t>(at)].eps.push_back(next);
                at = build(p, next);
            }
            return at;
        }
        case Re::Kind::Alt: {
            const int end = add();
            for (const Re &p : r.parts) {
                const int s = add();
                states[static_cast<std::size_t>(start)].eps.push_back(s);
                const int e = build(p, s);
                states[static_cast<std::size_t>(e)].eps.push_back(end);
            }
            return end;
        }
        case Re::Kind::Star: {
            const int end = add();
            const int s = add();
            states[static_cast<std::size_t>(start)].eps.push_back(s);
            states[static_cast<std::size_t>(start)].eps.push_back(end);
            const int e = build(r.parts[0], s);
            states[static_cast<std::size_t>(e)].eps.push_back(s);
            states[static_cast<std::size_t>(e)].eps.push_back(end);
            return end;
        }
        }
        return start;
    }
    void closure(std::vector<int> &set) const {
        std::vector<int> stack = set;
        std::vector<bool> seen(states.size());
        for (const int s : set) {
            seen[static_cast<std::size_t>(s)] = true;
        }
        while (!stack.empty()) {
            const int s = stack.back();
            stack.pop_back();
            for (const int t : states[static_cast<std::size_t>(s)].eps) {
                if (!seen[static_cast<std::size_t>(t)]) {
                    seen[static_cast<std::size_t>(t)] = true;
                    set.push_back(t);
                    stack.push_back(t);
                }
            }
        }
        std::sort(set.begin(), set.end());
    }
};

} // namespace

UseCat useCategory(char32_t u) noexcept { return static_cast<UseCat>(record(u)[0]); }
IndicCat indicCategory(char32_t u) noexcept { return static_cast<IndicCat>(record(u)[1]); }
IndicPos indicPosition(char32_t u) noexcept { return static_cast<IndicPos>(record(u)[2]); }

Scanner::Scanner(const std::vector<Re> &patterns) {
    Nfa nfa;
    const int start = nfa.add();
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        const int s = nfa.add();
        nfa.states[static_cast<std::size_t>(start)].eps.push_back(s);
        const int e = nfa.build(patterns[i], s);
        nfa.states[static_cast<std::size_t>(e)].accept = static_cast<int>(i);
    }
    std::map<std::vector<int>, int> ids;
    std::vector<std::vector<int>> sets;
    std::vector<int> first{start};
    nfa.closure(first);
    ids.emplace(first, 0);
    sets.push_back(first);
    for (std::size_t d = 0; d < sets.size(); ++d) {
        int accept = -1;
        for (const int s : sets[d]) {
            const int a = nfa.states[static_cast<std::size_t>(s)].accept;
            if (a >= 0 && (accept < 0 || a < accept)) {
                accept = a;
            }
        }
        // The start state accepts nothing: matches are never empty.
        m_accept.push_back(static_cast<std::int16_t>(d == 0 ? -1 : accept));
        for (unsigned c = 0; c < 64; ++c) {
            std::vector<int> next;
            for (const int s : sets[d]) {
                const Nfa::State &st = nfa.states[static_cast<std::size_t>(s)];
                if ((st.set >> c) & 1) {
                    next.push_back(st.to);
                }
            }
            std::int16_t target = -1;
            if (!next.empty()) {
                nfa.closure(next);
                const auto [it, inserted] = ids.emplace(next, static_cast<int>(sets.size()));
                if (inserted) {
                    sets.push_back(next);
                }
                target = static_cast<std::int16_t>(it->second);
            }
            m_next.push_back(target);
        }
    }
}

int Scanner::match(const std::uint8_t *cats, std::size_t n, std::size_t &length) const noexcept {
    int best = -1;
    length = 0;
    std::size_t state = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int16_t next = m_next[state * 64 + (cats[i] & 63u)];
        if (next < 0) {
            break;
        }
        state = static_cast<std::size_t>(next);
        if (m_accept[state] >= 0) {
            best = m_accept[state];
            length = i + 1;
        }
    }
    return best;
}

std::size_t nextSyllable(const ot::Buffer &b, std::size_t start) noexcept {
    const std::size_t count = std::min(b.len(), start + 64);
    const std::uint8_t syllable = b.info[start].syllable;
    while (++start < count && b.info[start].syllable == syllable) {
    }
    return start;
}

bool insertDottedCircles(const FontFace &face, ot::Buffer &b, std::uint8_t brokenType, std::uint8_t circleCategory,
                         int rephaCategory, int circlePosition) {
    const GlyphId glyph = face.glyphIndex(0x25CC);
    if (glyph == 0) {
        return false;
    }
    ot::GlyphInfo circle;
    circle.codepoint = 0x25CC;
    circle.glyph = glyph;
    circle.category = circleCategory;
    if (circlePosition >= 0) {
        circle.position = static_cast<std::uint8_t>(circlePosition);
    }
    b.clearOutput();
    b.idx = 0;
    unsigned last = 0;
    while (b.idx < b.len()) {
        const std::uint8_t syllable = b.cur().syllable;
        if (last != syllable && (syllable & 0x0F) == brokenType) {
            last = syllable;
            ot::GlyphInfo g = circle;
            g.cluster = b.cur().cluster;
            g.mask = b.cur().mask;
            g.syllable = syllable;
            if (rephaCategory >= 0) {
                while (b.idx < b.len() && b.cur().syllable == last && b.cur().category == rephaCategory) {
                    b.nextGlyph();
                }
            }
            b.out.push_back(g);
        } else {
            b.nextGlyph();
        }
    }
    b.swapBuffers();
    return true;
}

void insertVowelConstraintCircles(ot::Buffer &b, unicode::Script script) {
    const char *code = unicode::iso15924(script);
    const data::VowelConstraint *first = nullptr;
    const data::VowelConstraint *last = nullptr;
    for (const data::VowelConstraint &c : data::kVowelConstraints) {
        if (std::strcmp(c.script, code) == 0) {
            if (!first) {
                first = &c;
            }
            last = &c + 1;
        }
    }
    if (!first) {
        return;
    }
    const std::size_t count = b.len();
    b.clearOutput();
    b.idx = 0;
    while (b.idx + 1 < count) {
        const data::VowelConstraint *hit = nullptr;
        for (const data::VowelConstraint *c = first; c != last && !hit; ++c) {
            if (b.idx + c->length > count) {
                continue;
            }
            bool ok = true;
            for (std::size_t k = 0; k < c->length && ok; ++k) {
                ok = b.info[b.idx + k].codepoint == c->sequence[k];
            }
            if (ok) {
                hit = c;
            }
        }
        if (!hit) {
            b.nextGlyph();
            continue;
        }
        for (unsigned k = 0; k <= hit->before; ++k) {
            b.nextGlyph();
        }
        ot::GlyphInfo circle = b.cur(); // takes the next character's properties
        circle.codepoint = 0x25CC;
        circle.flags &= static_cast<std::uint16_t>(~ot::kContinuation);
        b.out.push_back(circle);
        b.nextGlyph();
    }
    b.swapBuffers();
}

// ---- Universal Shaping Engine ----

namespace {

constexpr std::uint64_t bit(UseCat c) { return std::uint64_t{1} << static_cast<unsigned>(c); }

const Scanner &useScanner() {
    static const Scanner scanner = [] {
        using enum UseCat;
        const Re h = cat(H, HVM, IS, Sk);
        const Re consonantModifiers = seq(star(cat(CMAbv)), star(cat(CMBlw)),
                                          star(seq(alt(seq(h, cat(B)), cat(SUB)), star(cat(CMAbv)), star(cat(CMBlw)))));
        const Re medialConsonants = seq(opt(cat(MPre)), opt(cat(MAbv)), opt(cat(MBlw)), opt(cat(MPst)));
        const Re dependentVowels = alt(seq(star(cat(VPre)), star(cat(VAbv)), star(cat(VBlw)), star(cat(VPst))), cat(H));
        const Re vowelModifiers =
            seq(opt(cat(HVM)), star(cat(VMPre)), star(cat(VMAbv)), star(cat(VMBlw)), star(cat(VMPst)));
        const Re finalConsonants = seq(star(cat(FAbv)), star(cat(FBlw)), star(cat(FPst)));
        const Re finalModifiers = alt(seq(star(cat(FMAbv)), star(cat(FMBlw))), opt(cat(FMPst)));
        const Re start = seq(opt(cat(R, CS)), cat(B, GB));
        const Re middle =
            seq(consonantModifiers, medialConsonants, dependentVowels, vowelModifiers, star(seq(cat(Sk), cat(B))));
        const Re tail = seq(middle, finalConsonants, finalModifiers);
        const Re numberJoinerTail = seq(star(seq(cat(HN), cat(N))), cat(HN));
        const Re numeralTail = plus(seq(cat(HN), cat(N)));
        const Re symbolTail = alt(seq(plus(cat(SMAbv)), star(cat(SMBlw))), plus(cat(SMBlw)));
        const Re viramaTail = seq(consonantModifiers, cat(IS, RK));
        const Re sakotTail = seq(middle, cat(Sk));
        const Re anyTail = alt(tail, sakotTail, symbolTail, viramaTail);
        const Re zwnj = opt(cat(ZWNJ));
        const Re hieroglyph =
            seq(star(cat(SB)), cat(G), opt(cat(HR)), opt(cat(HM)), star(cat(SE)),
                star(seq(cat(J), star(cat(SB)), opt(seq(cat(G), opt(cat(HR)), opt(cat(HM)), star(cat(SE)))))));
        return Scanner({
            seq(start, viramaTail, zwnj),            // UseViramaTerminated
            seq(start, sakotTail, zwnj),             // UseSakotTerminated
            seq(start, tail, zwnj),                  // UseStandard
            seq(cat(N), numberJoinerTail, zwnj),     // UseNumberJoinerTerminated
            seq(cat(N), opt(numeralTail), zwnj),     // UseNumeral
            seq(cat(O, GB, SB), opt(anyTail), zwnj), // UseSymbol
            seq(hieroglyph, zwnj),                   // UseHieroglyph
            cat(FMPst),                              // UseNonCluster
            seq(opt(cat(R)), alt(anyTail, numberJoinerTail, numeralTail),
                zwnj), // UseBroken
            any(),     // UseNonCluster
        });
    }();
    return scanner;
}

constexpr UseSyllable kUsePatternTypes[] = {
    UseViramaTerminated, UseSakotTerminated, UseStandard,   UseNumberJoinerTerminated,
    UseNumeral,          UseSymbol,          UseHieroglyph, UseNonCluster,
    UseBroken,           UseNonCluster};

bool isHalantUse(const ot::GlyphInfo &g) {
    const auto c = static_cast<UseCat>(g.category);
    return (c == UseCat::H || c == UseCat::HVM || c == UseCat::IS) && !(g.glyphProps & ot::kLigated);
}

void reorderSyllableUse(ot::Buffer &b, std::size_t start, std::size_t end) {
    const unsigned type = b.info[start].syllable & 0x0Fu;
    if (type != UseViramaTerminated && type != UseSakotTerminated && type != UseStandard && type != UseSymbol &&
        type != UseBroken) {
        return;
    }
    std::vector<ot::GlyphInfo> &info = b.info;
    using enum UseCat;
    constexpr std::uint64_t postBase = bit(FAbv) | bit(FBlw) | bit(FPst) | bit(FMAbv) | bit(FMBlw) | bit(FMPst) |
                                       bit(MAbv) | bit(MBlw) | bit(MPst) | bit(MPre) | bit(VAbv) | bit(VBlw) |
                                       bit(VPst) | bit(VPre) | bit(VMAbv) | bit(VMBlw) | bit(VMPst) | bit(VMPre);
    // A repha moves to the end of the base cluster: before the first
    // post-base glyph or halant.
    if (static_cast<UseCat>(info[start].category) == R && end - start > 1) {
        for (std::size_t i = start + 1; i < end; ++i) {
            const bool isPostBase = ((postBase >> info[i].category) & 1) || isHalantUse(info[i]);
            if (isPostBase || i == end - 1) {
                if (isPostBase) {
                    --i;
                }
                b.mergeClusters(start, i + 1);
                const ot::GlyphInfo t = info[start];
                std::memmove(&info[start], &info[start + 1], (i - start) * sizeof(ot::GlyphInfo));
                info[i] = t;
                break;
            }
        }
    }
    // Pre-base vowels move before the base: after the last halant.
    std::size_t j = start;
    for (std::size_t i = start; i < end; ++i) {
        const auto c = static_cast<UseCat>(info[i].category);
        if (isHalantUse(info[i])) {
            j = i + 1;
        } else if ((c == VPre || c == VMPre) && ot::ligComp(info[i]) == 0 && j < i) {
            b.mergeClusters(j, i + 1);
            const ot::GlyphInfo t = info[i];
            std::memmove(&info[j + 1], &info[j], (i - j) * sizeof(ot::GlyphInfo));
            info[j] = t;
        }
    }
}

} // namespace

void setUseCategories(ot::Buffer &b) {
    for (ot::GlyphInfo &g : b.info) {
        g.category = static_cast<std::uint8_t>(useCategory(g.codepoint));
    }
}

bool findUseSyllables(ot::Buffer &b) {
    // The grammar sees neither CGJ-like ignorables nor a ZWNJ before a mark.
    std::vector<std::uint32_t> at;
    std::vector<std::uint8_t> cats;
    const std::size_t n = b.len();
    for (std::size_t i = 0; i < n; ++i) {
        const ot::GlyphInfo &g = b.info[i];
        if (static_cast<UseCat>(g.category) == UseCat::CGJ) {
            continue;
        }
        if (static_cast<UseCat>(g.category) == UseCat::ZWNJ) {
            std::size_t k = i + 1;
            while (k < n && static_cast<UseCat>(b.info[k].category) == UseCat::CGJ) {
                ++k;
            }
            if (k < n && isUnicodeMark(b.info[k])) {
                continue;
            }
        }
        at.push_back(static_cast<std::uint32_t>(i));
        cats.push_back(g.category);
    }
    for (ot::GlyphInfo &g : b.info) {
        g.syllable = 0;
    }
    const Scanner &scanner = useScanner();
    bool broken = false;
    unsigned serial = 1;
    for (std::size_t p = 0; p < at.size();) {
        std::size_t length = 0;
        const int pattern = scanner.match(cats.data() + p, cats.size() - p, length);
        const UseSyllable type = pattern < 0 ? UseNonCluster : kUsePatternTypes[pattern];
        length = std::max<std::size_t>(length, 1);
        broken = broken || type == UseBroken;
        const std::size_t from = at[p];
        const std::size_t to = p + length < at.size() ? at[p + length] : n;
        for (std::size_t i = from; i < to; ++i) {
            b.info[i].syllable = static_cast<std::uint8_t>(serial << 4 | type);
        }
        serial = serial == 15 ? 1 : serial + 1;
        p += length;
    }
    return broken;
}

void reorderUse(ot::Buffer &b) {
    for (std::size_t start = 0; start < b.len();) {
        const std::size_t end = nextSyllable(b, start);
        reorderSyllableUse(b, start, end);
        start = end;
    }
}

} // namespace cfw::syllabic
