#include "cfw/text/Bidi.h"

#include <algorithm>

#include "cfw/text/Unicode.h"

namespace cfw {

using BC = unicode::BidiClass;

namespace {

constexpr std::uint8_t kMaxDepth = 125;

bool isIsolateInitiator(BC c) { return c == BC::LRI || c == BC::RLI || c == BC::FSI; }
bool isRemoved(BC c) {
    return c == BC::RLE || c == BC::LRE || c == BC::RLO || c == BC::LRO || c == BC::PDF || c == BC::BN;
}
bool isNeutralOrIsolate(BC c) {
    return c == BC::B || c == BC::S || c == BC::WS || c == BC::ON || isIsolateInitiator(c) || c == BC::PDI;
}

// Brackets that are canonically equivalent pair with each other (BD16).
char32_t canonicalBracket(char32_t c) {
    switch (c) {
    case 0x2329: return 0x3008;
    case 0x232A: return 0x3009;
    default: return c;
    }
}

// Scratch storage, reused across calls on a thread (the algorithm runs per
// paragraph, often; steady state allocates nothing).
struct Scratch {
    std::vector<BC> types;
    std::vector<std::uint8_t> explicitLevel;
    std::vector<std::size_t> matchingPdi;
    std::vector<std::size_t> matchingInitiator;
    std::vector<std::size_t> stack;
    std::vector<std::size_t> runStart; // level runs: characters runChars[runStart[r] .. runStart[r + 1])
    std::vector<std::size_t> runChars;
    std::vector<std::size_t> runOf;
    std::vector<std::size_t> sequence; // the isolating run sequence being resolved
    std::vector<std::pair<char32_t, std::size_t>> openers;
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
};

struct Resolver {
    Span<const BC> original;
    Span<const char32_t> text;
    Scratch &scratch;
    std::vector<BC> &types;
    std::vector<std::uint8_t> &levels;
    std::vector<std::uint8_t> &explicitLevel; // after X1-X8, before sequences resolve (sos/eos use these)
    std::vector<std::size_t> &matchingPdi;       // for isolate initiators; npos if none
    std::vector<std::size_t> &matchingInitiator; // for PDIs; npos if none
    std::uint8_t paragraphLevel = 0;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    Resolver(Span<const BC> classes, Span<const char32_t> chars, std::vector<std::uint8_t> &out, Scratch &s)
        : original(classes), text(chars), scratch(s), types(s.types), levels(out), explicitLevel(s.explicitLevel),
          matchingPdi(s.matchingPdi), matchingInitiator(s.matchingInitiator) {
        types.assign(classes.begin(), classes.end());
    }

    void matchIsolates() {
        const std::size_t n = original.size();
        matchingPdi.assign(n, npos);
        matchingInitiator.assign(n, npos);
        std::vector<std::size_t> &stack = scratch.stack;
        stack.clear();
        for (std::size_t i = 0; i < n; ++i) {
            if (isIsolateInitiator(original[i])) {
                stack.push_back(i);
            } else if (original[i] == BC::PDI && !stack.empty()) {
                matchingPdi[stack.back()] = i;
                matchingInitiator[i] = stack.back();
                stack.pop_back();
            } else if (original[i] == BC::B) {
                stack.clear();
            }
        }
    }

    // P2/P3 over [begin, end): 0, 1, or nothing if there is no strong character.
    std::optional<std::uint8_t> firstStrong(std::size_t begin, std::size_t end) const {
        for (std::size_t i = begin; i < end; ++i) {
            const BC c = original[i];
            if (c == BC::L) {
                return 0;
            }
            if (c == BC::R || c == BC::AL) {
                return 1;
            }
            if (isIsolateInitiator(c)) {
                if (matchingPdi[i] == npos) {
                    return std::nullopt; // the isolate runs to the paragraph's end
                }
                i = matchingPdi[i];
            } else if (c == BC::B) {
                break;
            }
        }
        return std::nullopt;
    }

    // X1-X8.
    void explicitLevels() {
        struct Status {
            std::uint8_t level;
            std::uint8_t override; // 0 neutral, 1 L, 2 R
            bool isolate;
        };
        thread_local std::vector<Status> stack;
        stack.clear();
        stack.reserve(kMaxDepth + 2);
        stack.push_back({paragraphLevel, 0, false});
        int overflowIsolates = 0;
        int overflowEmbeddings = 0;
        int validIsolates = 0;
        const auto nextOdd = [](std::uint8_t l) { return static_cast<std::uint8_t>((l + 1) | 1); };
        const auto nextEven = [](std::uint8_t l) { return static_cast<std::uint8_t>((l + 2) & ~1); };
        const auto applyOverride = [&](std::size_t i) {
            if (stack.back().override == 1) {
                types[i] = BC::L;
            } else if (stack.back().override == 2) {
                types[i] = BC::R;
            }
        };
        for (std::size_t i = 0; i < original.size(); ++i) {
            BC c = original[i];
            switch (c) {
            case BC::RLE:
            case BC::LRE:
            case BC::RLO:
            case BC::LRO: {
                const bool rtl = c == BC::RLE || c == BC::RLO;
                const std::uint8_t level = rtl ? nextOdd(stack.back().level) : nextEven(stack.back().level);
                levels[i] = stack.back().level;
                if (level <= kMaxDepth && overflowIsolates == 0 && overflowEmbeddings == 0) {
                    const std::uint8_t override = c == BC::RLO ? 2 : c == BC::LRO ? 1 : 0;
                    stack.push_back({level, override, false});
                } else if (overflowIsolates == 0) {
                    ++overflowEmbeddings;
                }
                break;
            }
            case BC::RLI:
            case BC::LRI:
            case BC::FSI: {
                levels[i] = stack.back().level;
                applyOverride(i);
                bool rtl = c == BC::RLI;
                if (c == BC::FSI) {
                    const std::size_t end = matchingPdi[i] == npos ? original.size() : matchingPdi[i];
                    rtl = firstStrong(i + 1, end).value_or(0) == 1;
                }
                const std::uint8_t level = rtl ? nextOdd(stack.back().level) : nextEven(stack.back().level);
                if (level <= kMaxDepth && overflowIsolates == 0 && overflowEmbeddings == 0) {
                    ++validIsolates;
                    stack.push_back({level, 0, true});
                } else {
                    ++overflowIsolates;
                }
                break;
            }
            case BC::PDI:
                if (overflowIsolates > 0) {
                    --overflowIsolates;
                } else if (validIsolates > 0) {
                    overflowEmbeddings = 0;
                    while (!stack.back().isolate) {
                        stack.pop_back();
                    }
                    stack.pop_back();
                    --validIsolates;
                }
                levels[i] = stack.back().level;
                applyOverride(i);
                break;
            case BC::PDF:
                levels[i] = stack.back().level;
                if (overflowIsolates > 0) {
                } else if (overflowEmbeddings > 0) {
                    --overflowEmbeddings;
                } else if (!stack.back().isolate && stack.size() >= 2) {
                    stack.pop_back();
                }
                break;
            case BC::B:
                levels[i] = paragraphLevel; // X8
                break;
            case BC::BN: levels[i] = stack.back().level; break;
            default:
                levels[i] = stack.back().level;
                applyOverride(i);
                break;
            }
        }
    }

    // X10: level runs of the characters X9 keeps, then each isolating run
    // sequence in turn (a run, continued through matching isolates).
    void resolveSequences() {
        const std::size_t n = original.size();
        std::vector<std::size_t> &runStart = scratch.runStart;
        std::vector<std::size_t> &runChars = scratch.runChars;
        std::vector<std::size_t> &runOf = scratch.runOf;
        runStart.clear();
        runChars.clear();
        runOf.assign(n, npos);
        std::uint8_t level = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (isRemoved(original[i])) {
                continue;
            }
            if (runStart.empty() || explicitLevel[i] != level) {
                runStart.push_back(runChars.size());
                level = explicitLevel[i];
            }
            runChars.push_back(i);
            runOf[i] = runStart.size() - 1;
        }
        const std::size_t runs = runStart.size();
        runStart.push_back(runChars.size());
        std::vector<std::size_t> &seq = scratch.sequence;
        for (std::size_t r = 0; r < runs; ++r) {
            const std::size_t first = runChars[runStart[r]];
            if (original[first] == BC::PDI && matchingInitiator[first] != npos) {
                continue; // continues a sequence started earlier
            }
            seq.assign(runChars.begin() + static_cast<std::ptrdiff_t>(runStart[r]),
                       runChars.begin() + static_cast<std::ptrdiff_t>(runStart[r + 1]));
            while (true) {
                const std::size_t last = seq.back();
                if (!isIsolateInitiator(original[last]) || matchingPdi[last] == npos) {
                    break;
                }
                const std::size_t next = runOf[matchingPdi[last]];
                if (next == npos) {
                    break;
                }
                seq.insert(seq.end(), runChars.begin() + static_cast<std::ptrdiff_t>(runStart[next]),
                           runChars.begin() + static_cast<std::ptrdiff_t>(runStart[next + 1]));
            }
            resolveSequence(seq);
        }
    }

    // W1-W7, N0-N2, I1-I2 on one isolating run sequence.
    void resolveSequence(const std::vector<std::size_t> &seq) {
        const std::size_t n = original.size();
        const std::uint8_t level = explicitLevel[seq.front()];
        // sos/eos: the higher of this level and the neighbouring one.
        std::uint8_t before = paragraphLevel;
        for (std::size_t i = seq.front(); i-- > 0;) {
            if (!isRemoved(original[i])) {
                before = explicitLevel[i];
                break;
            }
        }
        // eos: an isolate initiator at the end (no matching PDI) compares with
        // the paragraph level, anything else with the next character X9 keeps.
        std::uint8_t after = paragraphLevel;
        const std::size_t last = seq.back();
        if (!isIsolateInitiator(original[last])) {
            for (std::size_t i = last + 1; i < n; ++i) {
                if (!isRemoved(original[i])) {
                    after = explicitLevel[i];
                    break;
                }
            }
        }
        const BC sos = (std::max(before, level) & 1) ? BC::R : BC::L;
        const BC eos = (std::max(after, level) & 1) ? BC::R : BC::L;
        const std::size_t m = seq.size();
        const auto t = [&](std::size_t k) -> BC & { return types[seq[k]]; };

        // W1: NSM takes the type before it (ON after an isolate initiator or PDI).
        for (std::size_t k = 0; k < m; ++k) {
            if (t(k) == BC::NSM) {
                if (k == 0) {
                    t(k) = sos;
                } else {
                    const BC p = t(k - 1);
                    t(k) = (isIsolateInitiator(p) || p == BC::PDI) ? BC::ON : p;
                }
            }
        }
        // W2, W3.
        BC lastStrong = sos;
        for (std::size_t k = 0; k < m; ++k) {
            BC &c = t(k);
            if (c == BC::EN && lastStrong == BC::AL) {
                c = BC::AN;
            }
            if (c == BC::L || c == BC::R || c == BC::AL) {
                lastStrong = c;
            }
        }
        for (std::size_t k = 0; k < m; ++k) {
            if (t(k) == BC::AL) {
                t(k) = BC::R;
            }
        }
        // W4.
        for (std::size_t k = 1; k + 1 < m; ++k) {
            const BC p = t(k - 1);
            const BC q = t(k + 1);
            if (t(k) == BC::ES && p == BC::EN && q == BC::EN) {
                t(k) = BC::EN;
            } else if (t(k) == BC::CS && p == q && (p == BC::EN || p == BC::AN)) {
                t(k) = p;
            }
        }
        // W5: ET next to EN becomes EN.
        for (std::size_t k = 0; k < m; ++k) {
            if (t(k) != BC::ET) {
                continue;
            }
            std::size_t e = k;
            while (e < m && t(e) == BC::ET) {
                ++e;
            }
            const bool en = (k > 0 && t(k - 1) == BC::EN) || (e < m && t(e) == BC::EN);
            for (std::size_t j = k; j < e; ++j) {
                if (en) {
                    t(j) = BC::EN;
                }
            }
            k = e - 1;
        }
        // W6.
        for (std::size_t k = 0; k < m; ++k) {
            if (t(k) == BC::ES || t(k) == BC::ET || t(k) == BC::CS) {
                t(k) = BC::ON;
            }
        }
        // W7.
        lastStrong = sos;
        for (std::size_t k = 0; k < m; ++k) {
            if (t(k) == BC::EN && lastStrong == BC::L) {
                t(k) = BC::L;
            }
            if (t(k) == BC::L || t(k) == BC::R) {
                lastStrong = t(k);
            }
        }

        const BC embedding = (level & 1) ? BC::R : BC::L;
        const auto strong = [](BC c) { return c == BC::L ? BC::L : (c == BC::R || c == BC::EN || c == BC::AN) ? BC::R : BC::ON; };

        // N0: bracket pairs.
        if (!text.empty()) {
            std::vector<std::pair<char32_t, std::size_t>> &openers = scratch.openers; // closing bracket, position
            std::vector<std::pair<std::size_t, std::size_t>> &pairs = scratch.pairs;
            openers.clear();
            pairs.clear();
            bool overflow = false;
            for (std::size_t k = 0; k < m && !overflow; ++k) {
                if (t(k) != BC::ON) {
                    continue;
                }
                const char32_t c = text[seq[k]];
                const unicode::Properties p = unicode::properties(c);
                if (p.bracketType == unicode::BracketType::Open) {
                    if (openers.size() == 63) {
                        overflow = true;
                        break;
                    }
                    openers.emplace_back(canonicalBracket(unicode::pairedBracket(c)), k);
                } else if (p.bracketType == unicode::BracketType::Close) {
                    const char32_t want = canonicalBracket(c);
                    for (std::size_t s = openers.size(); s-- > 0;) {
                        if (openers[s].first == want) {
                            pairs.emplace_back(openers[s].second, k);
                            openers.resize(s);
                            break;
                        }
                    }
                }
            }
            if (overflow) {
                pairs.clear();
            }
            std::sort(pairs.begin(), pairs.end());
            for (const auto &[open, close] : pairs) {
                BC found = BC::ON;
                for (std::size_t k = open + 1; k < close; ++k) {
                    const BC s = strong(t(k));
                    if (s == embedding) {
                        found = embedding;
                        break;
                    }
                    if (s != BC::ON) {
                        found = s;
                    }
                }
                if (found == BC::ON) {
                    continue; // no strong type inside: leave them to N1/N2
                }
                BC dir = embedding;
                if (found != embedding) {
                    BC context = sos;
                    for (std::size_t k = open; k-- > 0;) {
                        const BC s = strong(t(k));
                        if (s != BC::ON) {
                            context = s;
                            break;
                        }
                    }
                    dir = context == found ? found : embedding;
                }
                t(open) = dir;
                t(close) = dir;
                // NSMs after a bracket follow it (they were ON after W1).
                for (const std::size_t b : {open, close}) {
                    for (std::size_t k = b + 1; k < m && original[seq[k]] == BC::NSM; ++k) {
                        t(k) = dir;
                    }
                }
            }
        }

        // N1, N2: neutrals between strong types of one direction take it;
        // others take the embedding direction.
        for (std::size_t k = 0; k < m; ++k) {
            if (!isNeutralOrIsolate(t(k))) {
                continue;
            }
            std::size_t e = k;
            while (e < m && isNeutralOrIsolate(t(e))) {
                ++e;
            }
            const BC left = k == 0 ? sos : strong(t(k - 1));
            const BC right = e == m ? eos : strong(t(e));
            const BC dir = (left == right && left != BC::ON) ? left : embedding;
            for (std::size_t j = k; j < e; ++j) {
                t(j) = dir;
            }
            k = e - 1;
        }

        // I1, I2.
        for (std::size_t k = 0; k < m; ++k) {
            std::uint8_t &l = levels[seq[k]];
            const BC c = t(k);
            if ((l & 1) == 0) {
                if (c == BC::R) {
                    l = static_cast<std::uint8_t>(l + 1);
                } else if (c == BC::AN || c == BC::EN) {
                    l = static_cast<std::uint8_t>(l + 2);
                }
            } else if (c == BC::L || c == BC::EN || c == BC::AN) {
                l = static_cast<std::uint8_t>(l + 1);
            }
        }
    }

    void run(std::optional<TextDirection> direction) {
        const std::size_t n = original.size();
        levels.assign(n, 0);
        matchIsolates();
        if (direction) {
            paragraphLevel = *direction == TextDirection::RightToLeft ? 1 : 0;
        } else {
            paragraphLevel = firstStrong(0, n).value_or(0);
        }
        explicitLevels();
        explicitLevel = levels;
        resolveSequences();
        // Removed characters take the level of the character before them.
        for (std::size_t i = 0; i < n; ++i) {
            if (isRemoved(original[i])) {
                levels[i] = i > 0 ? levels[i - 1] : paragraphLevel;
            }
        }
    }
};

} // namespace

void resolveBidi(Span<const BC> classes, Span<const char32_t> text, std::optional<TextDirection> direction,
                 BidiParagraph &out) {
    thread_local Scratch scratch;
    Resolver r(classes, text.size() == classes.size() ? text : Span<const char32_t>(), out.levels, scratch);
    r.run(direction);
    out.level = r.paragraphLevel;
}

void resolveBidi(Span<const char32_t> text, std::optional<TextDirection> direction, BidiParagraph &out) {
    thread_local std::vector<BC> classes;
    classes.resize(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        classes[i] = unicode::bidiClass(text[i]);
    }
    resolveBidi(classes, text, direction, out);
}

void applyBidiLineRules(Span<const BC> classes, std::uint8_t paragraphLevel, Span<std::uint8_t> levels,
                        std::size_t begin, std::size_t end) {
    end = std::min(end, levels.size());
    // Whitespace (and isolate controls and removed characters) before a
    // separator or at the end of the line resets to the paragraph level.
    bool trailing = true;
    for (std::size_t i = end; i-- > begin;) {
        const BC c = classes[i];
        if (c == BC::S || c == BC::B) {
            levels[i] = paragraphLevel;
            trailing = true;
        } else if (c == BC::WS || isIsolateInitiator(c) || c == BC::PDI || isRemoved(c)) {
            if (trailing) {
                levels[i] = paragraphLevel;
            }
        } else {
            trailing = false;
        }
    }
}

void bidiVisualOrder(Span<const std::uint8_t> levels, std::vector<std::uint32_t> &order) {
    const std::size_t n = levels.size();
    order.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    if (n == 0) {
        return;
    }
    std::uint8_t highest = 0;
    std::uint8_t lowestOdd = 255;
    for (const std::uint8_t l : levels) {
        highest = std::max(highest, l);
        if (l & 1) {
            lowestOdd = std::min(lowestOdd, l);
        }
    }
    for (std::uint8_t level = highest; level >= lowestOdd && level > 0; --level) {
        for (std::size_t i = 0; i < n;) {
            if (levels[order[i]] < level) {
                ++i;
                continue;
            }
            std::size_t e = i;
            while (e < n && levels[order[e]] >= level) {
                ++e;
            }
            std::reverse(order.begin() + static_cast<std::ptrdiff_t>(i), order.begin() + static_cast<std::ptrdiff_t>(e));
            i = e;
        }
    }
}

} // namespace cfw
