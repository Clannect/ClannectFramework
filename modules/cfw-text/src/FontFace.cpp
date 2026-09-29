#include "cfw/text/FontFace.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "cfw/core/Utf8.h"

namespace cfw {

namespace {

// Big-endian reads with bounds checks: out of range reads return 0 and
// clear `ok`, which callers test once at the end.
struct Reader {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    bool ok = true;

    Reader() = default;
    explicit Reader(Span<const std::byte> bytes)
        : data(reinterpret_cast<const std::uint8_t *>(bytes.data())), size(bytes.size()) {}

    bool has(std::size_t offset, std::size_t count) const noexcept {
        return offset <= size && count <= size - offset;
    }
    std::uint8_t u8(std::size_t o) noexcept {
        if (!has(o, 1)) {
            ok = false;
            return 0;
        }
        return data[o];
    }
    std::uint16_t u16(std::size_t o) noexcept {
        if (!has(o, 2)) {
            ok = false;
            return 0;
        }
        return static_cast<std::uint16_t>(data[o] << 8 | data[o + 1]);
    }
    std::int16_t i16(std::size_t o) noexcept { return static_cast<std::int16_t>(u16(o)); }
    std::uint32_t u24(std::size_t o) noexcept {
        if (!has(o, 3)) {
            ok = false;
            return 0;
        }
        return static_cast<std::uint32_t>(data[o]) << 16 | static_cast<std::uint32_t>(data[o + 1]) << 8 | data[o + 2];
    }
    std::uint32_t u32(std::size_t o) noexcept {
        if (!has(o, 4)) {
            ok = false;
            return 0;
        }
        return static_cast<std::uint32_t>(data[o]) << 24 | static_cast<std::uint32_t>(data[o + 1]) << 16 |
               static_cast<std::uint32_t>(data[o + 2]) << 8 | data[o + 3];
    }
};

Error malformed(const char *what) { return Error(ErrorCode::Corrupt, String("font: ") + what); }

// ---- CFF: INDEX and DICT ----

struct Index {
    std::uint32_t count = 0;
    std::uint32_t offsets = 0; // position of the offset array
    std::uint8_t offSize = 0;
    std::uint32_t dataBase = 0; // offsets are relative to dataBase + 1
    std::uint32_t end = 0;      // first byte after the INDEX
};

bool readIndex(Reader &r, std::uint32_t at, Index &out) {
    out = {};
    out.count = r.u16(at);
    if (!r.ok) {
        return false;
    }
    if (out.count == 0) {
        out.end = at + 2;
        return true;
    }
    out.offSize = r.u8(at + 2);
    if (out.offSize < 1 || out.offSize > 4) {
        return false;
    }
    out.offsets = at + 3;
    const std::uint64_t arrayEnd = out.offsets + static_cast<std::uint64_t>(out.count + 1) * out.offSize;
    if (arrayEnd > r.size) {
        return false;
    }
    out.dataBase = static_cast<std::uint32_t>(arrayEnd) - 1;
    std::uint32_t last = 0;
    for (std::uint32_t k = 0; k < out.offSize; ++k) {
        last = last << 8 | r.u8(out.offsets + out.count * out.offSize + k);
    }
    const std::uint64_t end = static_cast<std::uint64_t>(out.dataBase) + last;
    if (last < 1 || end > r.size) {
        return false;
    }
    out.end = static_cast<std::uint32_t>(end);
    return r.ok;
}

// Element `i` of an INDEX as [begin, end); false if out of range or broken.
bool indexItem(Reader &r, const Index &index, std::uint32_t i, std::uint32_t &begin, std::uint32_t &end) {
    if (i >= index.count) {
        return false;
    }
    const auto offset = [&](std::uint32_t k) {
        std::uint32_t v = 0;
        for (std::uint32_t b = 0; b < index.offSize; ++b) {
            v = v << 8 | r.u8(index.offsets + k * index.offSize + b);
        }
        return v;
    };
    const std::uint32_t a = offset(i);
    const std::uint32_t b = offset(i + 1);
    if (!r.ok || a < 1 || b < a || static_cast<std::uint64_t>(index.dataBase) + b > index.end) {
        return false;
    }
    begin = index.dataBase + a;
    end = index.dataBase + b;
    return true;
}

// A DICT's operators with their operands (at most 48 each).
struct DictEntry {
    std::uint16_t op; // 0-21, or 1200 + escape
    double args[4];
    int argc;
};

bool readDict(Reader &r, std::uint32_t begin, std::uint32_t end, std::vector<DictEntry> &out) {
    out.clear();
    double stack[48];
    int n = 0;
    std::uint32_t p = begin;
    while (p < end) {
        const std::uint8_t b = r.u8(p);
        if (!r.ok) {
            return false;
        }
        if (b <= 21) {
            std::uint16_t op = b;
            ++p;
            if (b == 12) {
                op = static_cast<std::uint16_t>(1200 + r.u8(p++));
            }
            DictEntry e{op, {0, 0, 0, 0}, std::min(n, 4)};
            for (int k = 0; k < e.argc; ++k) {
                e.args[k] = stack[n - e.argc + k];
            }
            out.push_back(e);
            n = 0;
            continue;
        }
        double v = 0;
        if (b == 28) {
            v = static_cast<std::int16_t>(r.u16(p + 1));
            p += 3;
        } else if (b == 29) {
            v = static_cast<std::int32_t>(r.u32(p + 1));
            p += 5;
        } else if (b == 30) {
            // Real number: nibbles until 0xf. Only its presence matters here.
            ++p;
            while (p < end) {
                const std::uint8_t x = r.u8(p++);
                if ((x & 0x0f) == 0x0f || (x >> 4) == 0x0f) {
                    break;
                }
            }
        } else if (b >= 32 && b <= 246) {
            v = b - 139;
            ++p;
        } else if (b >= 247 && b <= 250) {
            v = (b - 247) * 256 + r.u8(p + 1) + 108;
            p += 2;
        } else if (b >= 251 && b <= 254) {
            v = -(b - 251) * 256 - r.u8(p + 1) - 108;
            p += 2;
        } else {
            return false;
        }
        if (n < 48) {
            stack[n++] = v;
        } else {
            return false;
        }
    }
    return r.ok;
}

const DictEntry *find(const std::vector<DictEntry> &dict, std::uint16_t op) {
    for (const DictEntry &e : dict) {
        if (e.op == op) {
            return &e;
        }
    }
    return nullptr;
}

std::int32_t subrBias(std::uint32_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

} // namespace

// ---- CFF structures ----

struct FontFace::Cff {
    struct Private {
        Index subrs;
        double defaultWidth = 0;
        double nominalWidth = 0;
    };
    std::uint32_t base = 0; // offset of the CFF table in the file
    Index charStrings;
    Index globalSubrs;
    std::vector<Private> privates; // one, or one per FD for CID fonts
    // CID: FDSelect (format 0 or 3), as an offset; 0 for name-keyed fonts.
    std::uint32_t fdSelect = 0;
    std::uint8_t fdSelectFormat = 0;
};

namespace {

bool parsePrivate(Reader &r, std::uint32_t cffBase, const DictEntry *privateOp, FontFace::Cff::Private &out) {
    out = {};
    if (privateOp == nullptr || privateOp->argc < 2) {
        return true; // no private dict: no local subrs, widths 0
    }
    const double size = privateOp->args[0];
    const double offset = privateOp->args[1];
    if (size < 0 || offset < 0 || size > 1e7 || offset > 1e9) {
        return false;
    }
    const auto begin = static_cast<std::uint32_t>(cffBase + static_cast<std::uint32_t>(offset));
    const auto end = begin + static_cast<std::uint32_t>(size);
    if (!r.has(begin, static_cast<std::size_t>(size))) {
        return false;
    }
    std::vector<DictEntry> dict;
    if (!readDict(r, begin, end, dict)) {
        return false;
    }
    if (const DictEntry *e = find(dict, 20); e != nullptr && e->argc > 0) {
        out.defaultWidth = e->args[e->argc - 1];
    }
    if (const DictEntry *e = find(dict, 21); e != nullptr && e->argc > 0) {
        out.nominalWidth = e->args[e->argc - 1];
    }
    if (const DictEntry *e = find(dict, 19); e != nullptr && e->argc > 0) {
        const double subrs = e->args[e->argc - 1];
        if (subrs < 0 || subrs > 1e9 || !readIndex(r, begin + static_cast<std::uint32_t>(subrs), out.subrs)) {
            return false;
        }
    }
    return true;
}

// The Type 2 charstring interpreter.
struct CharString {
    Reader &r;
    const FontFace::Cff &cff;
    const FontFace::Cff::Private &priv;
    PainterPath &path;
    double stack[48];
    int n = 0;
    double transient[32] = {};
    double x = 0;
    double y = 0;
    int stems = 0;
    bool haveWidth = false;
    bool open = false;
    bool ended = false;
    int budget = 200000; // operators, across subroutines
    int depth = 0;

    void close() {
        if (open) {
            path.close();
            open = false;
        }
    }
    void moveTo(double dx, double dy) {
        close();
        x += dx;
        y += dy;
        path.moveTo({static_cast<float>(x), static_cast<float>(y)});
        open = true;
    }
    void lineTo(double dx, double dy) {
        x += dx;
        y += dy;
        path.lineTo({static_cast<float>(x), static_cast<float>(y)});
    }
    void curveTo(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
        const double x1 = x + dx1;
        const double y1 = y + dy1;
        const double x2 = x1 + dx2;
        const double y2 = y1 + dy2;
        x = x2 + dx3;
        y = y2 + dy3;
        path.cubicTo({static_cast<float>(x1), static_cast<float>(y1)}, {static_cast<float>(x2), static_cast<float>(y2)},
                     {static_cast<float>(x), static_cast<float>(y)});
    }
    // The first stack-clearing operator may carry the advance width first.
    void width(bool odd) {
        if (!haveWidth) {
            haveWidth = true;
            if (odd && n > 0) {
                std::memmove(stack, stack + 1, static_cast<std::size_t>(n - 1) * sizeof(double));
                --n;
            }
        }
    }

    bool run(std::uint32_t begin, std::uint32_t end) {
        if (++depth > 10) {
            return false;
        }
        std::uint32_t p = begin;
        while (p < end && !ended) {
            if (--budget < 0) {
                return false;
            }
            const std::uint8_t b = r.u8(p++);
            if (!r.ok) {
                return false;
            }
            if (b >= 32 || b == 28) {
                double v = 0;
                if (b == 28) {
                    v = static_cast<std::int16_t>(r.u16(p));
                    p += 2;
                } else if (b <= 246) {
                    v = b - 139;
                } else if (b <= 250) {
                    v = (b - 247) * 256 + r.u8(p++) + 108;
                } else if (b <= 254) {
                    v = -(b - 251) * 256 - r.u8(p++) - 108;
                } else {
                    v = static_cast<std::int32_t>(r.u32(p)) / 65536.0;
                    p += 4;
                }
                if (n >= 48) {
                    return false;
                }
                stack[n++] = v;
                continue;
            }
            if (!op(b, p, end)) {
                return false;
            }
        }
        --depth;
        return r.ok;
    }

    bool callSubr(const Index &subrs) {
        if (n < 1) {
            return false;
        }
        const auto index = static_cast<std::int64_t>(stack[--n]) + subrBias(subrs.count);
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        if (index < 0 || !indexItem(r, subrs, static_cast<std::uint32_t>(index), begin, end)) {
            return false;
        }
        return run(begin, end);
    }

    bool op(std::uint8_t b, std::uint32_t &p, std::uint32_t end) {
        switch (b) {
        case 1:  // hstem
        case 3:  // vstem
        case 18: // hstemhm
        case 23: // vstemhm
            width(n % 2 == 1);
            stems += n / 2;
            n = 0;
            return true;
        case 19: // hintmask
        case 20: // cntrmask
            width(n % 2 == 1);
            stems += n / 2; // implicit vstem
            n = 0;
            p += static_cast<std::uint32_t>((stems + 7) / 8);
            return p <= end;
        case 21: // rmoveto
            width(n > 2);
            if (n < 2) {
                return false;
            }
            moveTo(stack[0], stack[1]);
            n = 0;
            return true;
        case 22: // hmoveto
            width(n > 1);
            if (n < 1) {
                return false;
            }
            moveTo(stack[0], 0);
            n = 0;
            return true;
        case 4: // vmoveto
            width(n > 1);
            if (n < 1) {
                return false;
            }
            moveTo(0, stack[0]);
            n = 0;
            return true;
        case 5: // rlineto
            if (!open) {
                return false;
            }
            for (int i = 0; i + 1 < n; i += 2) {
                lineTo(stack[i], stack[i + 1]);
            }
            n = 0;
            return true;
        case 6: // hlineto
        case 7: // vlineto
            if (!open) {
                return false;
            }
            for (int i = 0; i < n; ++i) {
                const bool horizontal = (i % 2 == 0) == (b == 6);
                lineTo(horizontal ? stack[i] : 0, horizontal ? 0 : stack[i]);
            }
            n = 0;
            return true;
        case 8: // rrcurveto
            if (!open) {
                return false;
            }
            for (int i = 0; i + 5 < n; i += 6) {
                curveTo(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
            }
            n = 0;
            return true;
        case 27: { // hhcurveto
            if (!open) {
                return false;
            }
            int i = 0;
            double dy1 = 0;
            if (n % 2 == 1) {
                dy1 = stack[0];
                i = 1;
            }
            for (; i + 3 < n; i += 4) {
                curveTo(stack[i], dy1, stack[i + 1], stack[i + 2], stack[i + 3], 0);
                dy1 = 0;
            }
            n = 0;
            return true;
        }
        case 26: { // vvcurveto
            if (!open) {
                return false;
            }
            int i = 0;
            double dx1 = 0;
            if (n % 2 == 1) {
                dx1 = stack[0];
                i = 1;
            }
            for (; i + 3 < n; i += 4) {
                curveTo(dx1, stack[i], stack[i + 1], stack[i + 2], 0, stack[i + 3]);
                dx1 = 0;
            }
            n = 0;
            return true;
        }
        case 30: // vhcurveto
        case 31: { // hvcurveto
            if (!open) {
                return false;
            }
            bool horizontal = b == 31;
            for (int i = 0; i + 3 < n; i += 4) {
                const bool last = i + 8 > n; // the final curve may take a 5th operand
                const double extra = (last && n - i == 5) ? stack[i + 4] : 0;
                if (horizontal) {
                    curveTo(stack[i], 0, stack[i + 1], stack[i + 2], extra, stack[i + 3]);
                } else {
                    curveTo(0, stack[i], stack[i + 1], stack[i + 2], stack[i + 3], extra);
                }
                horizontal = !horizontal;
            }
            n = 0;
            return true;
        }
        case 24: { // rcurveline
            if (!open || n < 8) {
                return false;
            }
            int i = 0;
            for (; i + 7 < n; i += 6) {
                curveTo(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
            }
            lineTo(stack[i], stack[i + 1]);
            n = 0;
            return true;
        }
        case 25: { // rlinecurve
            if (!open || n < 8) {
                return false;
            }
            int i = 0;
            for (; i + 7 < n; i += 2) {
                lineTo(stack[i], stack[i + 1]);
            }
            curveTo(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
            n = 0;
            return true;
        }
        case 10: return callSubr(currentLocalSubrs());
        case 29: return callSubr(cff.globalSubrs);
        case 11: // return
            p = end;
            return true;
        case 14: // endchar (the deprecated accent form, seac, is not supported)
            width(n == 1 || n == 5);
            close();
            ended = true;
            n = 0;
            return true;
        case 12: return escape(r.u8(p++));
        default: return false; // reserved
        }
    }

    const Index &currentLocalSubrs() const { return priv.subrs; }

    bool escape(std::uint8_t e) {
        const auto need = [&](int k) { return n >= k; };
        switch (e) {
        case 35: // flex: two curves, and a depth we ignore
            if (!open || !need(13)) {
                return false;
            }
            curveTo(stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
            curveTo(stack[6], stack[7], stack[8], stack[9], stack[10], stack[11]);
            n = 0;
            return true;
        case 34: // hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6, back to the starting height
            if (!open || !need(7)) {
                return false;
            }
            curveTo(stack[0], 0, stack[1], stack[2], stack[3], 0);
            curveTo(stack[4], 0, stack[5], -stack[2], stack[6], 0);
            n = 0;
            return true;
        case 36: // hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6, back to the starting height
            if (!open || !need(9)) {
                return false;
            }
            curveTo(stack[0], stack[1], stack[2], stack[3], stack[4], 0);
            curveTo(stack[5], 0, stack[6], stack[7], stack[8], -(stack[1] + stack[3] + stack[7]));
            n = 0;
            return true;
        case 37: { // flex1: five points, and d6 along the larger of the total dx or dy
            if (!open || !need(11)) {
                return false;
            }
            double dx = 0;
            double dy = 0;
            for (int i = 0; i < 10; i += 2) {
                dx += stack[i];
                dy += stack[i + 1];
            }
            curveTo(stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
            if (std::abs(dx) > std::abs(dy)) {
                curveTo(stack[6], stack[7], stack[8], stack[9], stack[10], -dy);
            } else {
                curveTo(stack[6], stack[7], stack[8], stack[9], -dx, stack[10]);
            }
            n = 0;
            return true;
        }
        // Arithmetic and storage (rare in fonts, but defined).
        case 3: return binary([](double a, double b) { return (a != 0 && b != 0) ? 1.0 : 0.0; });
        case 4: return binary([](double a, double b) { return (a != 0 || b != 0) ? 1.0 : 0.0; });
        case 5: return unary([](double a) { return a == 0 ? 1.0 : 0.0; });
        case 9: return unary([](double a) { return std::abs(a); });
        case 10: return binary([](double a, double b) { return a + b; });
        case 11: return binary([](double a, double b) { return a - b; });
        case 12: return binary([](double a, double b) { return b == 0 ? 0.0 : a / b; });
        case 14: return unary([](double a) { return -a; });
        case 15: return binary([](double a, double b) { return a == b ? 1.0 : 0.0; });
        case 18: // drop
            if (!need(1)) {
                return false;
            }
            --n;
            return true;
        case 20: { // put
            if (!need(2)) {
                return false;
            }
            const auto i = static_cast<int>(stack[n - 1]);
            if (i < 0 || i >= 32) {
                return false;
            }
            transient[i] = stack[n - 2];
            n -= 2;
            return true;
        }
        case 21: { // get
            if (!need(1)) {
                return false;
            }
            const auto i = static_cast<int>(stack[n - 1]);
            if (i < 0 || i >= 32) {
                return false;
            }
            stack[n - 1] = transient[i];
            return true;
        }
        case 22: { // ifelse
            if (!need(4)) {
                return false;
            }
            const double v = stack[n - 2] <= stack[n - 1] ? stack[n - 4] : stack[n - 3];
            n -= 3;
            stack[n - 1] = v;
            return true;
        }
        case 23: // random: deterministic here
            if (n >= 48) {
                return false;
            }
            stack[n++] = 0.5;
            return true;
        case 24: return binary([](double a, double b) { return a * b; });
        case 26: return unary([](double a) { return a >= 0 ? std::sqrt(a) : 0.0; });
        case 27: // dup
            if (!need(1) || n >= 48) {
                return false;
            }
            stack[n] = stack[n - 1];
            ++n;
            return true;
        case 28: // exch
            if (!need(2)) {
                return false;
            }
            std::swap(stack[n - 1], stack[n - 2]);
            return true;
        case 29: { // index
            if (!need(1)) {
                return false;
            }
            auto i = static_cast<int>(stack[n - 1]);
            if (i < 0) {
                i = 0;
            }
            if (i >= n - 1) {
                return false;
            }
            stack[n - 1] = stack[n - 2 - i];
            return true;
        }
        case 30: { // roll
            if (!need(2)) {
                return false;
            }
            const auto count = static_cast<int>(stack[n - 2]);
            const auto shift = static_cast<int>(stack[n - 1]);
            n -= 2;
            if (count <= 0 || count > n) {
                return count == 0;
            }
            const int s = ((shift % count) + count) % count;
            std::rotate(stack + (n - count), stack + (n - s), stack + n);
            return true;
        }
        default: return false;
        }
    }

    template <class F>
    bool unary(F f) {
        if (n < 1) {
            return false;
        }
        stack[n - 1] = f(stack[n - 1]);
        return true;
    }
    template <class F>
    bool binary(F f) {
        if (n < 2) {
            return false;
        }
        stack[n - 2] = f(stack[n - 2], stack[n - 1]);
        --n;
        return true;
    }
};

} // namespace

// ---- Loading ----

FontFace::~FontFace() = default;

Result<std::uint32_t> FontFace::faceCount(Span<const std::byte> data) {
    Reader r(data);
    const std::uint32_t magic = r.u32(0);
    if (!r.ok) {
        return malformed("too short");
    }
    if (magic == tag("ttcf")) {
        const std::uint32_t count = r.u32(8);
        if (!r.ok || count == 0 || count > 10000 || !r.has(12, static_cast<std::size_t>(count) * 4)) {
            return malformed("bad collection header");
        }
        return count;
    }
    if (magic == 0x00010000 || magic == tag("OTTO") || magic == tag("true")) {
        return 1u;
    }
    return Error(ErrorCode::Unsupported, "font: not an OpenType font (or a WOFF/Type 1 font)");
}

Result<std::shared_ptr<const FontFace>> FontFace::load(Data data, std::uint32_t index) {
    if (!data) {
        return malformed("no data");
    }
    std::shared_ptr<FontFace> face(new FontFace());
    static std::atomic<std::uint64_t> nextId{1};
    face->m_data = std::move(data);
    face->m_uniqueId = nextId.fetch_add(1, std::memory_order_relaxed);
    if (Result<void> parsed = face->parse(index); !parsed) {
        return parsed.error();
    }
    return std::shared_ptr<const FontFace>(std::move(face));
}

Span<const std::byte> FontFace::table(std::uint32_t t) const noexcept {
    for (const TableEntry &e : m_tables) {
        if (e.tag == t) {
            return Span<const std::byte>(m_data->data() + e.offset, e.length);
        }
    }
    return {};
}

namespace {

String decodeName(Reader &r, std::uint32_t at, std::uint32_t length, bool utf16) {
    String out;
    if (!r.has(at, length)) {
        return out;
    }
    if (utf16) {
        for (std::uint32_t i = 0; i + 1 < length; i += 2) {
            char32_t c = r.u16(at + i);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < length) {
                const char32_t low = r.u16(at + i + 2);
                if (low >= 0xDC00 && low < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            appendUtf8(out, c);
        }
    } else {
        for (std::uint32_t i = 0; i < length; ++i) {
            const std::uint8_t c = r.u8(at + i);
            appendUtf8(out, c < 128 ? c : U'�'); // Mac Roman beyond ASCII is rare in names
        }
    }
    return out;
}

// name: family and style, preferring Windows Unicode English names, and the
// typographic names over the legacy ones.
void readNames(Reader &r, std::uint32_t o, std::uint32_t length, String &family, String &style) {
    if (length < 6) {
        return;
    }
    const std::uint16_t records = r.u16(o + 2);
    const std::uint32_t strings = o + r.u16(o + 4);
    int bestFamily = -1;
    int bestStyle = -1;
    for (std::uint32_t i = 0; i < records && r.has(o + 6 + i * 12, 12); ++i) {
        const std::uint32_t rec = o + 6 + i * 12;
        const std::uint16_t platform = r.u16(rec);
        const std::uint16_t encoding = r.u16(rec + 2);
        const std::uint16_t language = r.u16(rec + 4);
        const std::uint16_t id = r.u16(rec + 6);
        int score = 0;
        if (platform == 3 && (encoding == 1 || encoding == 10)) {
            score = language == 0x409 ? 40 : 30;
        } else if (platform == 0) {
            score = 20;
        } else if (platform == 1 && encoding == 0) {
            score = 10;
        } else {
            continue;
        }
        const bool utf16 = platform != 1;
        const int typographic = (id == 16 || id == 17) ? 5 : 0;
        if ((id == 1 || id == 16) && score + typographic > bestFamily) {
            bestFamily = score + typographic;
            family = decodeName(r, strings + r.u16(rec + 10), r.u16(rec + 8), utf16);
        } else if ((id == 2 || id == 17) && score + typographic > bestStyle) {
            bestStyle = score + typographic;
            style = decodeName(r, strings + r.u16(rec + 10), r.u16(rec + 8), utf16);
        }
    }
}

} // namespace

Result<FontFace::Description> FontFace::describe(Span<const std::byte> data, std::uint32_t index) {
    Reader r(data);
    const Result<std::uint32_t> count = faceCount(data);
    if (!count) {
        return count.error();
    }
    if (index >= count.value()) {
        return Error(ErrorCode::NotFound, "font: no face with that index in the file");
    }
    const std::uint32_t dir = r.u32(0) == tag("ttcf") ? r.u32(12 + 4 * static_cast<std::size_t>(index)) : 0;
    const std::uint16_t tableCount = r.u16(dir + 4);
    if (!r.ok || !r.has(dir + 12, static_cast<std::size_t>(tableCount) * 16)) {
        return malformed("bad table directory");
    }
    TableEntry head{}, maxp{}, hhea{}, hmtx{}, os2{}, name{};
    for (std::uint32_t i = 0; i < tableCount; ++i) {
        const std::uint32_t rec = dir + 12 + i * 16;
        const TableEntry e{r.u32(rec), r.u32(rec + 8), r.u32(rec + 12)};
        if (!r.has(e.offset, e.length)) {
            continue; // as parse() does
        }
        TableEntry *slot = e.tag == tag("head")   ? &head
                           : e.tag == tag("maxp") ? &maxp
                           : e.tag == tag("hhea") ? &hhea
                           : e.tag == tag("hmtx") ? &hmtx
                           : e.tag == tag("OS/2") ? &os2
                           : e.tag == tag("name") ? &name
                                                  : nullptr;
        if (slot && slot->tag == 0) {
            *slot = e; // the first entry wins, as in parse()
        }
    }
    if (head.tag == 0 || maxp.tag == 0 || hhea.tag == 0 || hmtx.tag == 0 || head.length < 54 || maxp.length < 6 ||
        hhea.length < 36) {
        return malformed("missing or short head/maxp/hhea/hmtx");
    }
    const std::uint16_t unitsPerEm = r.u16(head.offset + 18);
    const std::uint16_t hMetricCount = r.u16(hhea.offset + 34);
    if (unitsPerEm < 16 || unitsPerEm > 16384 || hMetricCount == 0 ||
        static_cast<std::uint64_t>(hMetricCount) * 4 > hmtx.length) {
        return malformed("bad head or hmtx");
    }
    Description out;
    if (os2.tag != 0 && os2.length >= 78) {
        out.weight = r.u16(os2.offset + 4);
        out.italic = (r.u16(os2.offset + 62) & 1) != 0;
    }
    if (name.tag != 0) {
        readNames(r, name.offset, name.length, out.family, out.style);
    }
    return out;
}

Result<void> FontFace::parse(std::uint32_t index) {
    Reader r(*m_data);
    const Result<std::uint32_t> count = faceCount(*m_data);
    if (!count) {
        return count.error();
    }
    if (index >= count.value()) {
        return Error(ErrorCode::NotFound, "font: no face with that index in the file");
    }
    std::uint32_t dir = 0;
    if (r.u32(0) == tag("ttcf")) {
        dir = r.u32(12 + 4 * static_cast<std::size_t>(index));
    }
    const std::uint16_t tableCount = r.u16(dir + 4);
    if (!r.ok || !r.has(dir + 12, static_cast<std::size_t>(tableCount) * 16)) {
        return malformed("bad table directory");
    }
    for (std::uint32_t i = 0; i < tableCount; ++i) {
        const std::uint32_t rec = dir + 12 + i * 16;
        const TableEntry e{r.u32(rec), r.u32(rec + 8), r.u32(rec + 12)};
        if (r.has(e.offset, e.length)) {
            m_tables.push_back(e); // tables that point outside the file are ignored
        }
    }
    const auto entry = [&](const char(&t)[5]) -> const TableEntry * {
        for (const TableEntry &e : m_tables) {
            if (e.tag == tag(t)) {
                return &e;
            }
        }
        return nullptr;
    };

    // head, maxp, hhea, hmtx
    const TableEntry *head = entry("head");
    const TableEntry *maxp = entry("maxp");
    const TableEntry *hhea = entry("hhea");
    const TableEntry *hmtx = entry("hmtx");
    if (!head || !maxp || !hhea || !hmtx || head->length < 54 || maxp->length < 6 || hhea->length < 36) {
        return malformed("missing or short head/maxp/hhea/hmtx");
    }
    m_unitsPerEm = r.u16(head->offset + 18);
    if (m_unitsPerEm < 16 || m_unitsPerEm > 16384) {
        return malformed("unitsPerEm out of range");
    }
    m_longLoca = r.i16(head->offset + 50) != 0;
    m_glyphCount = r.u16(maxp->offset + 4);
    m_line = {r.i16(hhea->offset + 4), r.i16(hhea->offset + 6), r.i16(hhea->offset + 8)};
    m_hMetricCount = r.u16(hhea->offset + 34);
    m_hmtx = hmtx->offset;
    m_hmtxEnd = hmtx->offset + hmtx->length;
    if (m_hMetricCount == 0 || static_cast<std::uint64_t>(m_hMetricCount) * 4 > hmtx->length) {
        return malformed("bad hmtx");
    }

    // OS/2 and post
    if (const TableEntry *os2 = entry("OS/2"); os2 && os2->length >= 78) {
        const std::uint32_t o = os2->offset;
        m_weight = r.u16(o + 4);
        const std::uint16_t selection = r.u16(o + 62);
        m_italic = (selection & 1) != 0;
        m_strikeoutThickness = r.i16(o + 26);
        m_strikeoutPosition = r.i16(o + 28);
        m_winAscent = r.u16(o + 74);
        m_winDescent = r.u16(o + 76);
        if (selection & 0x80) { // USE_TYPO_METRICS
            m_line = {r.i16(o + 68), r.i16(o + 70), r.i16(o + 72)};
        }
        if (r.u16(o) >= 2 && os2->length >= 90) {
            m_xHeight = r.i16(o + 86);
            m_capHeight = r.i16(o + 88);
        }
    }
    if (const TableEntry *post = entry("post"); post && post->length >= 16) {
        m_underlinePosition = r.i16(post->offset + 8);
        m_underlineThickness = r.i16(post->offset + 10);
        m_fixedPitch = r.u32(post->offset + 12) != 0;
    }

    if (const TableEntry *name = entry("name")) {
        readNames(r, name->offset, name->length, m_family, m_style);
    }

    // cmap: the best Unicode subtable, and variation sequences.
    if (const TableEntry *cmap = entry("cmap"); cmap && cmap->length >= 4) {
        const std::uint32_t o = cmap->offset;
        const std::uint16_t subtables = r.u16(o + 2);
        int best = -1;
        for (std::uint32_t i = 0; i < subtables && r.has(o + 4 + i * 8, 8); ++i) {
            const std::uint32_t rec = o + 4 + i * 8;
            const std::uint16_t platform = r.u16(rec);
            const std::uint16_t encoding = r.u16(rec + 2);
            const std::uint32_t at = o + r.u32(rec + 4);
            if (at >= o + cmap->length) {
                continue;
            }
            const std::uint16_t format = r.u16(at);
            if (platform == 0 && encoding == 5 && format == 14) {
                m_cmapVariations = at;
                m_cmapVariationsEnd = o + cmap->length;
                continue;
            }
            int score = -1;
            if ((platform == 3 && encoding == 10) || (platform == 0 && (encoding == 4 || encoding == 6))) {
                score = 5;
            } else if ((platform == 3 && encoding == 1) || (platform == 0 && encoding <= 3)) {
                score = 4;
            } else if (platform == 3 && encoding == 0) {
                score = 2;
            }
            const bool supported = format == 0 || format == 4 || format == 6 || format == 10 || format == 12 || format == 13;
            if (supported && score > best) {
                best = score;
                m_cmap = at;
                m_cmapEnd = o + cmap->length;
                m_cmapFormat = format;
                m_cmapSymbol = platform == 3 && encoding == 0;
            }
        }
    }

    // Outlines: glyf/loca or CFF.
    const TableEntry *glyf = entry("glyf");
    const TableEntry *loca = entry("loca");
    if (glyf && loca) {
        m_glyf = glyf->offset;
        m_glyfEnd = glyf->offset + glyf->length;
        m_loca = loca->offset;
        m_locaEnd = loca->offset + loca->length;
    } else if (const TableEntry *cffTable = entry("CFF ")) {
        auto cff = std::make_unique<Cff>();
        Reader c(Span<const std::byte>(m_data->data(), cffTable->offset + cffTable->length));
        const std::uint32_t b = cffTable->offset;
        cff->base = b;
        const std::uint8_t headerSize = c.u8(b + 2);
        Index names;
        Index topDicts;
        Index strings;
        if (!c.ok || !readIndex(c, b + headerSize, names) || !readIndex(c, names.end, topDicts) ||
            !readIndex(c, topDicts.end, strings) || !readIndex(c, strings.end, cff->globalSubrs)) {
            return malformed("bad CFF header or INDEX");
        }
        std::uint32_t tb = 0;
        std::uint32_t te = 0;
        std::vector<DictEntry> top;
        if (!indexItem(c, topDicts, 0, tb, te) || !readDict(c, tb, te, top)) {
            return malformed("bad CFF top DICT");
        }
        const DictEntry *charStrings = find(top, 17);
        if (!charStrings || charStrings->argc < 1 || charStrings->args[0] < 0 ||
            !readIndex(c, b + static_cast<std::uint32_t>(charStrings->args[0]), cff->charStrings)) {
            return malformed("bad CFF CharStrings");
        }
        if (const DictEntry *type = find(top, 1206); type && type->argc > 0 && type->args[0] != 2) {
            return Error(ErrorCode::Unsupported, "font: CFF charstring type other than 2");
        }
        const DictEntry *fdArray = find(top, 1236);
        const DictEntry *fdSelect = find(top, 1237);
        if (find(top, 1230) && fdArray && fdSelect && fdArray->argc > 0 && fdSelect->argc > 0) {
            // CID-keyed: a Private DICT per font DICT, chosen by FDSelect.
            Index fds;
            if (fdArray->args[0] < 0 || !readIndex(c, b + static_cast<std::uint32_t>(fdArray->args[0]), fds) ||
                fds.count == 0 || fds.count > 256) {
                return malformed("bad CFF FDArray");
            }
            for (std::uint32_t i = 0; i < fds.count; ++i) {
                std::uint32_t fb = 0;
                std::uint32_t fe = 0;
                std::vector<DictEntry> fd;
                Cff::Private priv;
                if (!indexItem(c, fds, i, fb, fe) || !readDict(c, fb, fe, fd) ||
                    !parsePrivate(c, b, find(fd, 18), priv)) {
                    return malformed("bad CFF font DICT");
                }
                cff->privates.push_back(priv);
            }
            if (fdSelect->args[0] < 0) {
                return malformed("bad CFF FDSelect");
            }
            cff->fdSelect = b + static_cast<std::uint32_t>(fdSelect->args[0]);
            cff->fdSelectFormat = c.u8(cff->fdSelect);
            if (!c.ok || (cff->fdSelectFormat != 0 && cff->fdSelectFormat != 3)) {
                return malformed("unsupported CFF FDSelect format");
            }
        } else {
            Cff::Private priv;
            if (!parsePrivate(c, b, find(top, 18), priv)) {
                return malformed("bad CFF Private DICT");
            }
            cff->privates.push_back(priv);
        }
        if (cff->charStrings.count < m_glyphCount) {
            m_glyphCount = cff->charStrings.count;
        }
        m_cff = std::move(cff);
    }
    if (!r.ok) {
        return malformed("truncated tables");
    }
    return success();
}

// ---- Character mapping ----

GlyphId FontFace::glyphIndex(char32_t c) const noexcept {
    if (m_cmap == 0) {
        return 0;
    }
    Reader r(Span<const std::byte>(m_data->data(), m_cmapEnd));
    const std::uint32_t o = m_cmap;
    std::uint32_t glyph = 0;
    const auto lookup = [&](char32_t ch) -> std::uint32_t {
        switch (m_cmapFormat) {
        case 0: return ch < 256 ? r.u8(o + 6 + ch) : 0;
        case 6: {
            const std::uint16_t first = r.u16(o + 6);
            const std::uint16_t count = r.u16(o + 8);
            return ch >= first && ch - first < count ? r.u16(o + 10 + 2 * (ch - first)) : 0;
        }
        case 10: {
            const std::uint32_t first = r.u32(o + 12);
            const std::uint32_t count = r.u32(o + 16);
            return ch >= first && ch - first < count ? r.u16(o + 20 + 2 * (ch - first)) : 0;
        }
        case 4: {
            if (ch > 0xFFFF) {
                return 0;
            }
            const std::uint32_t segments = r.u16(o + 6) / 2u;
            const std::uint32_t ends = o + 14;
            const std::uint32_t starts = ends + segments * 2 + 2;
            const std::uint32_t deltas = starts + segments * 2;
            const std::uint32_t ranges = deltas + segments * 2;
            std::uint32_t lo = 0;
            std::uint32_t hi = segments;
            while (lo < hi) { // first segment with end >= ch
                const std::uint32_t mid = (lo + hi) / 2;
                if (r.u16(ends + mid * 2) < ch) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo >= segments || !r.ok) {
                return 0;
            }
            const std::uint16_t start = r.u16(starts + lo * 2);
            if (ch < start) {
                return 0;
            }
            const std::uint16_t delta = r.u16(deltas + lo * 2);
            const std::uint16_t range = r.u16(ranges + lo * 2);
            if (range == 0) {
                return (ch + delta) & 0xFFFFu;
            }
            const std::uint32_t at = ranges + lo * 2 + range + 2 * (ch - start);
            const std::uint16_t g = r.u16(at);
            return g == 0 ? 0 : (g + delta) & 0xFFFFu;
        }
        case 12:
        case 13: {
            const std::uint32_t groups = r.u32(o + 12);
            if (!r.has(o + 16, static_cast<std::size_t>(groups) * 12)) {
                return 0;
            }
            std::uint32_t lo = 0;
            std::uint32_t hi = groups;
            while (lo < hi) {
                const std::uint32_t mid = lo + (hi - lo) / 2;
                const std::uint32_t g = o + 16 + mid * 12;
                if (r.u32(g + 4) < ch) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo >= groups) {
                return 0;
            }
            const std::uint32_t g = o + 16 + lo * 12;
            const std::uint32_t start = r.u32(g);
            if (ch < start) {
                return 0;
            }
            const std::uint32_t first = r.u32(g + 8);
            return m_cmapFormat == 12 ? first + (ch - start) : first;
        }
        default: return 0;
        }
    };
    glyph = lookup(c);
    if (glyph == 0 && m_cmapSymbol && c < 0x100) {
        glyph = lookup(0xF000 + c); // symbol fonts map their characters at U+F0xx
    }
    if (!r.ok || glyph >= m_glyphCount) {
        return 0;
    }
    return static_cast<GlyphId>(glyph);
}

GlyphId FontFace::glyphIndex(char32_t c, char32_t selector) const noexcept {
    return variationGlyph(c, selector).value_or(glyphIndex(c));
}

std::optional<GlyphId> FontFace::variationGlyph(char32_t c, char32_t selector) const noexcept {
    if (m_cmapVariations == 0) {
        return std::nullopt;
    }
    Reader r(Span<const std::byte>(m_data->data(), m_cmapVariationsEnd));
    const std::uint32_t o = m_cmapVariations;
    const std::uint32_t records = r.u32(o + 6);
    for (std::uint32_t i = 0; i < records && r.ok && r.has(o + 10 + i * 11, 11); ++i) {
        const std::uint32_t rec = o + 10 + i * 11;
        if (r.u24(rec) != selector) {
            continue;
        }
        if (const std::uint32_t def = r.u32(rec + 3); def != 0) {
            const std::uint32_t at = o + def;
            const std::uint32_t ranges = r.u32(at);
            for (std::uint32_t k = 0; k < ranges && r.has(at + 4 + k * 4, 4); ++k) {
                const std::uint32_t start = r.u24(at + 4 + k * 4);
                const std::uint32_t count = r.u8(at + 4 + k * 4 + 3);
                if (c >= start && c <= start + count) {
                    const GlyphId g = glyphIndex(c);
                    return g != 0 ? std::optional<GlyphId>(g) : std::nullopt;
                }
            }
        }
        if (const std::uint32_t nonDefault = r.u32(rec + 7); nonDefault != 0) {
            const std::uint32_t at = o + nonDefault;
            const std::uint32_t mappings = r.u32(at);
            for (std::uint32_t k = 0; k < mappings && r.has(at + 4 + k * 5, 5); ++k) {
                if (r.u24(at + 4 + k * 5) == c) {
                    const std::uint16_t g = r.u16(at + 4 + k * 5 + 3);
                    return g < m_glyphCount && g != 0 ? std::optional<GlyphId>(g) : std::nullopt;
                }
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// ---- Metrics ----

int FontFace::advanceWidth(GlyphId glyph) const noexcept {
    Reader r(Span<const std::byte>(m_data->data(), m_hmtxEnd));
    const std::uint32_t i = std::min<std::uint32_t>(glyph, m_hMetricCount - 1);
    return r.u16(m_hmtx + i * 4);
}

int FontFace::leftSideBearing(GlyphId glyph) const noexcept {
    Reader r(Span<const std::byte>(m_data->data(), m_hmtxEnd));
    if (glyph < m_hMetricCount) {
        return r.i16(m_hmtx + glyph * 4u + 2);
    }
    return r.i16(m_hmtx + m_hMetricCount * 4u + 2u * (glyph - m_hMetricCount));
}

// ---- Outlines ----

bool FontFace::glyphExtents(GlyphId glyph, GlyphExtents &out) const {
    out = {};
    if (glyph >= m_glyphCount) {
        return false;
    }
    if (m_cff) {
        thread_local PainterPath path;
        if (!glyphOutline(glyph, path)) {
            return false;
        }
        // The bounds of every point that is part of a segment, control
        // points included (a move that starts nothing does not count).
        float minX = 0;
        float minY = 0;
        float maxX = 0;
        float maxY = 0;
        bool any = false;
        const auto add = [&](Vec2 p) {
            if (!any) {
                minX = maxX = p.x;
                minY = maxY = p.y;
                any = true;
            }
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        };
        const Span<const Vec2> pts = path.points();
        std::size_t k = 0;
        Vec2 start{};
        bool open = false;
        for (const PainterPath::Verb verb : path.verbs()) {
            switch (verb) {
            case PainterPath::Verb::Move:
                start = pts[k++];
                open = false;
                break;
            case PainterPath::Verb::Line:
            case PainterPath::Verb::Quad:
            case PainterPath::Verb::Cubic: {
                if (!open) {
                    add(start);
                    open = true;
                }
                const std::size_t n = verb == PainterPath::Verb::Line ? 1 : verb == PainterPath::Verb::Quad ? 2 : 3;
                for (std::size_t i = 0; i < n; ++i) {
                    add(pts[k++]);
                }
                break;
            }
            case PainterPath::Verb::Close: open = false; break;
            }
        }
        if (any && minX < maxX) {
            out.xBearing = static_cast<int>(std::lround(minX));
            out.width = static_cast<int>(std::lround(maxX)) - out.xBearing;
        }
        if (any && minY < maxY) {
            out.yBearing = static_cast<int>(std::lround(maxY));
            out.height = static_cast<int>(std::lround(minY)) - out.yBearing;
        }
        return true;
    }
    if (m_glyf == 0) {
        return false;
    }
    Reader r(*m_data);
    const std::uint32_t start = m_longLoca ? r.u32(m_loca + glyph * 4u) : r.u16(m_loca + glyph * 2u) * 2u;
    const std::uint32_t next = m_longLoca ? r.u32(m_loca + glyph * 4u + 4) : r.u16(m_loca + glyph * 2u + 2) * 2u;
    if (!r.ok || m_loca + (glyph + 2u) * (m_longLoca ? 4u : 2u) > m_locaEnd) {
        return false;
    }
    if (next <= start) {
        return next == start;
    }
    if (static_cast<std::uint64_t>(m_glyf) + start + 10 > m_glyfEnd) {
        return false;
    }
    const int xMin = r.i16(m_glyf + start + 2);
    const int yMin = r.i16(m_glyf + start + 4);
    const int xMax = r.i16(m_glyf + start + 6);
    const int yMax = r.i16(m_glyf + start + 8);
    out.xBearing = leftSideBearing(glyph);
    out.yBearing = std::max(yMin, yMax);
    out.width = std::max(xMin, xMax) - std::min(xMin, xMax);
    out.height = std::min(yMin, yMax) - std::max(yMin, yMax);
    return r.ok;
}

bool FontFace::glyphOutline(GlyphId glyph, PainterPath &out) const {
    out.clear();
    if (glyph >= m_glyphCount) {
        return false;
    }
    if (m_cff) {
        Reader r(*m_data);
        const Cff &cff = *m_cff;
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        if (!indexItem(r, cff.charStrings, glyph, begin, end)) {
            return false;
        }
        std::size_t fd = 0;
        if (cff.fdSelect != 0) {
            if (cff.fdSelectFormat == 0) {
                fd = r.u8(cff.fdSelect + 1 + glyph);
            } else {
                const std::uint16_t ranges = r.u16(cff.fdSelect + 1);
                for (std::uint32_t i = 0; i < ranges; ++i) {
                    const std::uint32_t rec = cff.fdSelect + 3 + i * 3;
                    const std::uint16_t first = r.u16(rec);
                    const std::uint16_t next = r.u16(rec + 3); // the next range's first, or the sentinel
                    if (glyph >= first && glyph < next) {
                        fd = r.u8(rec + 2);
                        break;
                    }
                }
            }
            if (!r.ok || fd >= cff.privates.size()) {
                return false;
            }
        }
        CharString cs{r, cff, cff.privates[fd], out};
        if (!cs.run(begin, end)) {
            out.clear();
            return false;
        }
        cs.close();
        return true;
    }
    if (m_glyf == 0) {
        return false;
    }
    if (!trueTypeOutline(glyph, out, 0, nullptr)) {
        out.clear();
        return false;
    }
    // As the TrueType rasteriser's phantom points do (and FreeType and
    // fontTools with them): the outline sits so that its left edge is at the
    // hmtx left side bearing, even where the glyf header's xMin disagrees.
    if (!out.empty()) {
        Reader r(*m_data);
        const std::uint32_t start = m_longLoca ? r.u32(m_loca + glyph * 4u) : r.u16(m_loca + glyph * 2u) * 2u;
        const std::int16_t xMin = r.i16(m_glyf + start + 2);
        const int shift = leftSideBearing(glyph) - xMin;
        if (r.ok && shift != 0) {
            out.transform(Transform2D::translation(shift, 0));
        }
    }
    return true;
}

bool FontFace::trueTypeOutline(GlyphId glyph, PainterPath &out, int depth, std::vector<Vec2> *points) const {
    if (depth > 16 || glyph >= m_glyphCount) {
        return false;
    }
    Reader r(*m_data);
    std::uint32_t start = 0;
    std::uint32_t next = 0;
    if (m_longLoca) {
        start = r.u32(m_loca + glyph * 4u);
        next = r.u32(m_loca + glyph * 4u + 4);
    } else {
        start = r.u16(m_loca + glyph * 2u) * 2u;
        next = r.u16(m_loca + glyph * 2u + 2) * 2u;
    }
    if (!r.ok || m_loca + (glyph + 1u) * (m_longLoca ? 4u : 2u) + (m_longLoca ? 4u : 2u) > m_locaEnd) {
        return false;
    }
    if (next <= start) {
        return next == start; // empty glyph (a space)
    }
    const std::uint32_t g = m_glyf + start;
    if (static_cast<std::uint64_t>(m_glyf) + next > m_glyfEnd) {
        return false;
    }
    Reader gr(Span<const std::byte>(m_data->data(), m_glyf + next));
    const std::int16_t contours = gr.i16(g);
    if (!gr.ok) {
        return false;
    }

    if (contours >= 0) {
        // Simple glyph.
        const std::uint32_t endPts = g + 10;
        const std::uint32_t instructionLength = gr.u16(endPts + 2u * static_cast<std::uint32_t>(contours));
        std::uint32_t p = endPts + 2u * static_cast<std::uint32_t>(contours) + 2 + instructionLength;
        if (contours == 0) {
            return gr.ok;
        }
        const std::uint32_t pointCount = gr.u16(endPts + 2u * static_cast<std::uint32_t>(contours - 1)) + 1u;
        if (!gr.ok) {
            return false;
        }
        thread_local std::vector<std::uint8_t> flags;
        thread_local std::vector<Vec2> pts;
        flags.clear();
        while (flags.size() < pointCount) {
            const std::uint8_t f = gr.u8(p++);
            flags.push_back(f);
            if (f & 8) {
                const std::uint8_t repeat = gr.u8(p++);
                for (int k = 0; k < repeat && flags.size() < pointCount; ++k) {
                    flags.push_back(f);
                }
            }
            if (!gr.ok) {
                return false;
            }
        }
        pts.assign(pointCount, Vec2{});
        std::int32_t v = 0;
        for (std::uint32_t i = 0; i < pointCount; ++i) {
            const std::uint8_t f = flags[i];
            if (f & 2) {
                const std::uint8_t d = gr.u8(p++);
                v += (f & 16) ? d : -d;
            } else if (!(f & 16)) {
                v += gr.i16(p);
                p += 2;
            }
            pts[i].x = static_cast<float>(v);
        }
        v = 0;
        for (std::uint32_t i = 0; i < pointCount; ++i) {
            const std::uint8_t f = flags[i];
            if (f & 4) {
                const std::uint8_t d = gr.u8(p++);
                v += (f & 32) ? d : -d;
            } else if (!(f & 32)) {
                v += gr.i16(p);
                p += 2;
            }
            pts[i].y = static_cast<float>(v);
        }
        if (!gr.ok) {
            return false;
        }
        std::uint32_t first = 0;
        for (std::int32_t c = 0; c < contours; ++c) {
            const std::uint32_t last = gr.u16(endPts + 2u * static_cast<std::uint32_t>(c));
            if (last < first || last >= pointCount) {
                return false;
            }
            const std::uint32_t m = last - first + 1;
            const auto at = [&](std::uint32_t k) { return pts[first + k % m]; };
            const auto on = [&](std::uint32_t k) { return (flags[first + k % m] & 1) != 0; };
            const auto mid = [](Vec2 a, Vec2 b) { return Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}; };
            // Start on an on-curve point: the first, else the last, else
            // the midpoint of the two (as FreeType does).
            Vec2 startPoint;
            std::uint32_t begin = 0;
            std::uint32_t count = m;
            if (on(0)) {
                startPoint = at(0);
                begin = 1;
                count = m - 1;
            } else if (on(m - 1)) {
                startPoint = at(m - 1);
                begin = 0;
                count = m - 1;
            } else {
                startPoint = mid(at(0), at(m - 1));
                begin = 0;
                count = m;
            }
            out.moveTo(startPoint);
            bool pending = false;
            Vec2 control{};
            for (std::uint32_t k = 0; k < count; ++k) {
                const Vec2 q = at(begin + k);
                if (on(begin + k)) {
                    if (pending) {
                        out.quadTo(control, q);
                    } else {
                        out.lineTo(q);
                    }
                    pending = false;
                } else {
                    if (pending) {
                        out.quadTo(control, mid(control, q));
                    }
                    control = q;
                    pending = true;
                }
            }
            if (pending) {
                out.quadTo(control, startPoint);
            }
            out.close();
            first = last + 1;
        }
        if (points != nullptr) {
            points->insert(points->end(), pts.begin(), pts.end());
        }
        return true;
    }

    // Composite glyph.
    std::uint32_t p = g + 10;
    std::vector<Vec2> allPoints; // this glyph's points so far (for point matching)
    PainterPath component;
    std::vector<Vec2> componentPoints;
    for (int components = 0; components < 1024; ++components) {
        const std::uint16_t flags = gr.u16(p);
        const GlyphId child = gr.u16(p + 2);
        p += 4;
        std::int32_t arg1 = 0;
        std::int32_t arg2 = 0;
        if (flags & 1) { // ARG_1_AND_2_ARE_WORDS
            arg1 = (flags & 2) ? gr.i16(p) : gr.u16(p);
            arg2 = (flags & 2) ? gr.i16(p + 2) : gr.u16(p + 2);
            p += 4;
        } else {
            arg1 = (flags & 2) ? static_cast<std::int8_t>(gr.u8(p)) : gr.u8(p);
            arg2 = (flags & 2) ? static_cast<std::int8_t>(gr.u8(p + 1)) : gr.u8(p + 1);
            p += 2;
        }
        float xx = 1;
        float xy = 0;
        float yx = 0;
        float yy = 1;
        const auto f2dot14 = [&](std::uint32_t at) { return static_cast<float>(gr.i16(at)) / 16384.0f; };
        if (flags & 8) { // WE_HAVE_A_SCALE
            xx = yy = f2dot14(p);
            p += 2;
        } else if (flags & 0x40) { // WE_HAVE_AN_X_AND_Y_SCALE
            xx = f2dot14(p);
            yy = f2dot14(p + 2);
            p += 4;
        } else if (flags & 0x80) { // WE_HAVE_A_TWO_BY_TWO
            xx = f2dot14(p);
            yx = f2dot14(p + 2);
            xy = f2dot14(p + 4);
            yy = f2dot14(p + 6);
            p += 8;
        }
        if (!gr.ok) {
            return false;
        }
        component.clear();
        componentPoints.clear();
        if (!trueTypeOutline(child, component, depth + 1, &componentPoints)) {
            return false;
        }
        // x' = xx x + xy y, y' = yx x + yy y (the component's 2x2).
        const Transform2D linear = Transform2D::fromRows(xx, xy, 0, yx, yy, 0);
        for (Vec2 &q : componentPoints) {
            q = linear.map(q);
        }
        Vec2 offset{};
        if (flags & 2) { // ARGS_ARE_XY_VALUES
            // Offsets are not scaled, whatever SCALED_COMPONENT_OFFSET says:
            // FreeType (what Qt renders with on Linux) and fontTools ignore it.
            offset = {static_cast<float>(arg1), static_cast<float>(arg2)};
        } else {
            // Point matching: the component's point arg2 lands on our point arg1.
            if (static_cast<std::size_t>(arg1) >= allPoints.size() ||
                static_cast<std::size_t>(arg2) >= componentPoints.size()) {
                return false;
            }
            const Vec2 a = allPoints[static_cast<std::size_t>(arg1)];
            const Vec2 b = componentPoints[static_cast<std::size_t>(arg2)];
            offset = {a.x - b.x, a.y - b.y};
        }
        component.transform(Transform2D::translation(offset.x, offset.y) * linear);
        out.addPath(component);
        for (const Vec2 q : componentPoints) {
            allPoints.push_back({q.x + offset.x, q.y + offset.y});
        }
        if (!(flags & 0x20)) { // MORE_COMPONENTS
            break;
        }
    }
    if (points != nullptr) {
        points->insert(points->end(), allPoints.begin(), allPoints.end());
    }
    return gr.ok;
}

} // namespace cfw
