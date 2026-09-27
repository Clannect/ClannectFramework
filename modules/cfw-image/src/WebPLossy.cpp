// WebP lossy (VP8 key frame, RFC 6386) decoder. The boolean decoder,
// transforms, predictors, loop filter and "fancy" YUV 4:2:0 -> RGB
// upsampling follow libwebp's integer arithmetic, so the output matches
// dwebp's exactly.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "Vp8Tables.h"
#include "WebPInternal.h"
#include "cfw/image/Image.h"

namespace cfw::webp {

namespace {

Error corrupt(const char *what) { return Error(ErrorCode::Corrupt, String("webp lossy: ") + what); }

// --- Boolean entropy decoder (RFC 6386 §7) ---------------------------------

class BoolDecoder {
public:
    BoolDecoder() = default;
    BoolDecoder(const std::uint8_t *data, std::size_t size) noexcept : m_buf(data), m_end(data + size) { load(); }

    [[nodiscard]] int bit(int prob) noexcept {
        if (m_bits < 0) {
            load();
        }
        std::uint32_t range = m_range;
        const std::uint32_t split = (range * static_cast<std::uint32_t>(prob)) >> 8;
        const auto value = static_cast<std::uint32_t>(m_value >> m_bits);
        const int b = value > split ? 1 : 0;
        if (b != 0) {
            range -= split;
            m_value -= static_cast<std::uint64_t>(split + 1) << m_bits;
        } else {
            range = split + 1;
        }
        const int shift = 7 ^ floorLog2(range);
        range <<= shift;
        m_bits -= shift;
        m_range = range - 1;
        return b;
    }
    [[nodiscard]] int signedValue(int v) noexcept {
        if (m_bits < 0) {
            load();
        }
        const int pos = m_bits;
        const std::uint32_t split = m_range >> 1;
        const auto value = static_cast<std::uint32_t>(m_value >> pos);
        const std::int32_t mask = static_cast<std::int32_t>(split - value) >> 31; // -1 or 0
        m_bits -= 1;
        m_range += static_cast<std::uint32_t>(mask);
        m_range |= 1;
        m_value -= static_cast<std::uint64_t>((split + 1) & static_cast<std::uint32_t>(mask)) << pos;
        return (v ^ mask) - mask;
    }
    [[nodiscard]] std::uint32_t literal(int bits) noexcept {
        std::uint32_t v = 0;
        while (bits-- > 0) {
            v |= static_cast<std::uint32_t>(bit(0x80)) << bits;
        }
        return v;
    }
    [[nodiscard]] int signedLiteral(int bits) noexcept {
        const auto v = static_cast<int>(literal(bits));
        return bit(0x80) != 0 ? -v : v;
    }
    [[nodiscard]] bool eof() const noexcept { return m_eof; }

private:
    static int floorLog2(std::uint32_t v) noexcept { return static_cast<int>(std::bit_width(v)) - 1; }
    // One byte at a time: the same bits, and the same end-of-data point, as
    // libwebp's wider loads.
    void load() noexcept {
        if (m_buf < m_end) {
            m_bits += 8;
            m_value = (m_value << 8) | *m_buf++;
        } else if (!m_eof) {
            m_value <<= 8;
            m_bits += 8;
            m_eof = true;
        } else {
            m_bits = 0;
        }
    }

    const std::uint8_t *m_buf = nullptr;
    const std::uint8_t *m_end = nullptr;
    std::uint64_t m_value = 0;
    int m_bits = -8;
    std::uint32_t m_range = 255 - 1;
    bool m_eof = false;
};

// --- Frame state ------------------------------------------------------------

enum : std::uint8_t {
    kBDc = 0,
    kBTm,
    kBVe,
    kBHe,
    kBRd,
    kBVr,
    kBLd,
    kBVl,
    kBHd,
    kBHu,
    // DC prediction without top and/or left samples (16x16 and chroma).
    kDcNoTop,
    kDcNoLeft,
    kDcNoTopLeft,
};

struct MacroblockInfo {
    std::uint8_t segment = 0;
    bool skip = false;
    bool i4x4 = false;
    std::array<std::uint8_t, 16> modes{}; // 16 sub-block modes, or modes[0] for 16x16
    std::uint8_t uvMode = 0;
    std::array<std::int16_t, 384> coeffs{}; // 16 Y, 4 U, 4 V blocks of 16
    std::uint32_t nonZeroY = 0;
    std::uint32_t nonZeroUv = 0;
    bool filterInner = false;
};

struct Quant {
    std::array<int, 2> y1{};
    std::array<int, 2> y2{};
    std::array<int, 2> uv{};
};

struct FilterStrength {
    int limit = 0;
    int innerLevel = 0;
    int hevThreshold = 0;
    bool inner = false;
};

constexpr std::array<std::uint8_t, 16> kZigzag = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr std::array<std::uint8_t, 17> kBands = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
constexpr std::array<std::uint8_t, 4> kCat3 = {173, 148, 140, 0};
constexpr std::array<std::uint8_t, 5> kCat4 = {176, 155, 140, 135, 0};
constexpr std::array<std::uint8_t, 6> kCat5 = {180, 157, 141, 134, 130, 0};
constexpr std::array<std::uint8_t, 12> kCat6 = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};

std::uint8_t clip8(int v) noexcept { return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

class Decoder {
public:
    Decoder(const std::uint8_t *data, std::size_t size, const ImageLimits &limits) noexcept
        : m_data(data), m_size(size), m_limits(limits) {}

    Result<Rgba> run();

private:
    Result<void> parseHeaders();
    void parseModes(MacroblockInfo &mb, std::size_t mbX);
    bool parseResiduals(MacroblockInfo &mb, std::size_t mbX, BoolDecoder &tokens);
    int coefficients(BoolDecoder &br, int type, int ctx, const std::array<int, 2> &dq, int n, std::int16_t *out);
    void reconstruct(const MacroblockInfo &mb, std::size_t mbX, std::size_t mbY);
    void filterFrame();
    Rgba toRgba() const;

    [[nodiscard]] const std::uint8_t *probs(int type, int band, int ctx) const noexcept {
        return m_coeffProbs.data() + ((static_cast<std::size_t>(type) * 8 + static_cast<std::size_t>(band)) * 3 +
                                      static_cast<std::size_t>(ctx)) * 11;
    }

    const std::uint8_t *m_data;
    std::size_t m_size;
    const ImageLimits &m_limits;

    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::size_t m_mbW = 0;
    std::size_t m_mbH = 0;
    BoolDecoder m_br;
    std::array<BoolDecoder, 8> m_parts;
    std::size_t m_partMask = 0;

    bool m_useSegment = false;
    bool m_updateMap = false;
    bool m_absoluteDelta = true;
    std::array<int, 4> m_segmentQuant{};
    std::array<int, 4> m_segmentFilter{};
    std::array<std::uint8_t, 3> m_segmentProbs{255, 255, 255};

    bool m_simpleFilter = false;
    int m_filterLevel = 0;
    int m_sharpness = 0;
    bool m_useLfDelta = false;
    std::array<int, 4> m_refLfDelta{};
    std::array<int, 4> m_modeLfDelta{};
    int m_filterType = 0; // 0 none, 1 simple, 2 complex

    std::array<Quant, 4> m_quant{};
    std::array<std::uint8_t, 1056> m_coeffProbs{};
    bool m_useSkipProb = false;
    int m_skipProb = 0;
    std::array<std::array<FilterStrength, 2>, 4> m_strengths{};

    // Mode contexts: above (4 per macroblock column) and left (4).
    std::vector<std::uint8_t> m_intraTop;
    std::array<std::uint8_t, 4> m_intraLeft{};
    // Non-zero contexts: above per column, and left.
    struct Nz {
        std::uint8_t nz = 0;
        std::uint8_t nzDc = 0;
    };
    std::vector<Nz> m_nzTop;
    Nz m_nzLeft;

    // The frame, macroblock-aligned; filtered in place once reconstructed.
    std::vector<std::uint8_t> m_y;
    std::vector<std::uint8_t> m_u;
    std::vector<std::uint8_t> m_v;
    std::size_t m_yStride = 0;
    std::size_t m_uvStride = 0;
    std::vector<FilterStrength> m_filterInfo; // per macroblock
};

Result<void> Decoder::parseHeaders() {
    if (m_size < 10) {
        return corrupt("truncated frame header");
    }
    const std::uint32_t bits = m_data[0] | static_cast<std::uint32_t>(m_data[1]) << 8 | static_cast<std::uint32_t>(m_data[2]) << 16;
    const bool keyFrame = (bits & 1u) == 0;
    const unsigned profile = (bits >> 1) & 7u;
    const bool show = ((bits >> 4) & 1u) != 0;
    const std::uint32_t partitionLength = bits >> 5;
    if (!keyFrame) {
        return Error(ErrorCode::Unsupported, "webp lossy: not a key frame");
    }
    if (profile > 3) {
        return corrupt("unknown profile");
    }
    if (!show) {
        return Error(ErrorCode::Unsupported, "webp lossy: frame is not displayable");
    }
    if (m_data[3] != 0x9d || m_data[4] != 0x01 || m_data[5] != 0x2a) {
        return corrupt("bad start code");
    }
    m_width = (static_cast<std::uint32_t>(m_data[7]) << 8 | m_data[6]) & 0x3FFFu;
    m_height = (static_cast<std::uint32_t>(m_data[9]) << 8 | m_data[8]) & 0x3FFFu;
    if (m_width == 0 || m_height == 0) {
        return corrupt("zero dimension");
    }
    if (Result<void> ok = checkImageSize(m_width, m_height, m_limits); !ok) {
        return ok;
    }
    m_mbW = (m_width + 15) / 16;
    m_mbH = (m_height + 15) / 16;
    const std::uint8_t *buf = m_data + 10;
    std::size_t size = m_size - 10;
    if (partitionLength > size) {
        return corrupt("first partition runs past the end");
    }
    m_br = BoolDecoder(buf, partitionLength);
    buf += partitionLength;
    size -= partitionLength;

    (void)m_br.literal(1); // colour space
    (void)m_br.literal(1); // clamping type
    // Segments (§9.3).
    m_useSegment = m_br.literal(1) != 0;
    if (m_useSegment) {
        m_updateMap = m_br.literal(1) != 0;
        if (m_br.literal(1) != 0) {
            m_absoluteDelta = m_br.literal(1) != 0;
            for (int &q : m_segmentQuant) {
                q = m_br.literal(1) != 0 ? m_br.signedLiteral(7) : 0;
            }
            for (int &f : m_segmentFilter) {
                f = m_br.literal(1) != 0 ? m_br.signedLiteral(6) : 0;
            }
        }
        if (m_updateMap) {
            for (std::uint8_t &p : m_segmentProbs) {
                p = m_br.literal(1) != 0 ? static_cast<std::uint8_t>(m_br.literal(8)) : std::uint8_t{255};
            }
        }
    }
    // Loop filter (§9.4).
    m_simpleFilter = m_br.literal(1) != 0;
    m_filterLevel = static_cast<int>(m_br.literal(6));
    m_sharpness = static_cast<int>(m_br.literal(3));
    m_useLfDelta = m_br.literal(1) != 0;
    if (m_useLfDelta && m_br.literal(1) != 0) {
        for (int &d : m_refLfDelta) {
            if (m_br.literal(1) != 0) {
                d = m_br.signedLiteral(6);
            }
        }
        for (int &d : m_modeLfDelta) {
            if (m_br.literal(1) != 0) {
                d = m_br.signedLiteral(6);
            }
        }
    }
    m_filterType = m_filterLevel == 0 ? 0 : m_simpleFilter ? 1 : 2;
    if (m_br.eof()) {
        return corrupt("truncated frame header");
    }
    // Token partitions (§9.5).
    const std::size_t parts = std::size_t{1} << m_br.literal(2);
    m_partMask = parts - 1;
    if (size < 3 * (parts - 1)) {
        return corrupt("truncated partition sizes");
    }
    const std::uint8_t *sizes = buf;
    const std::uint8_t *start = buf + 3 * (parts - 1);
    std::size_t left = size - 3 * (parts - 1);
    for (std::size_t p = 0; p + 1 < parts; ++p) {
        std::size_t psize = sizes[0] | static_cast<std::size_t>(sizes[1]) << 8 | static_cast<std::size_t>(sizes[2]) << 16;
        psize = std::min(psize, left);
        m_parts[p] = BoolDecoder(start, psize);
        start += psize;
        left -= psize;
        sizes += 3;
    }
    m_parts[parts - 1] = BoolDecoder(start, left);
    if (left == 0) {
        return corrupt("missing token partition data");
    }
    // Quantisers (§9.6).
    const int baseQ = static_cast<int>(m_br.literal(7));
    const auto delta = [&] { return m_br.literal(1) != 0 ? m_br.signedLiteral(4) : 0; };
    const int dqY1Dc = delta();
    const int dqY2Dc = delta();
    const int dqY2Ac = delta();
    const int dqUvDc = delta();
    const int dqUvAc = delta();
    for (std::size_t s = 0; s < 4; ++s) {
        int q = baseQ;
        if (m_useSegment) {
            q = m_segmentQuant[s] + (m_absoluteDelta ? 0 : baseQ);
        } else if (s > 0) {
            m_quant[s] = m_quant[0];
            continue;
        }
        const auto idx = [](int v, int max) { return static_cast<std::size_t>(std::clamp(v, 0, max)); };
        Quant &m = m_quant[s];
        m.y1 = {kDcQuant[idx(q + dqY1Dc, 127)], kAcQuant[idx(q, 127)]};
        m.y2 = {kDcQuant[idx(q + dqY2Dc, 127)] * 2, (kAcQuant[idx(q + dqY2Ac, 127)] * 101581) >> 16};
        if (m.y2[1] < 8) {
            m.y2[1] = 8;
        }
        m.uv = {kDcQuant[idx(q + dqUvDc, 117)], kAcQuant[idx(q + dqUvAc, 127)]};
    }
    (void)m_br.literal(1); // refresh_entropy_probs: ignored for a single key frame
    // Coefficient probabilities (§13.4).
    for (std::size_t i = 0; i < 1056; ++i) {
        m_coeffProbs[i] =
            m_br.bit(kCoeffUpdateProbs[i]) != 0 ? static_cast<std::uint8_t>(m_br.literal(8)) : kCoeffProbs[i];
    }
    m_useSkipProb = m_br.literal(1) != 0;
    if (m_useSkipProb) {
        m_skipProb = static_cast<int>(m_br.literal(8));
    }
    // Filter strengths per segment and 4x4/16x16 (frame_dec.c).
    if (m_filterType > 0) {
        for (std::size_t s = 0; s < 4; ++s) {
            int base = m_filterLevel;
            if (m_useSegment) {
                base = m_segmentFilter[s] + (m_absoluteDelta ? 0 : m_filterLevel);
            }
            for (std::size_t i4 = 0; i4 < 2; ++i4) {
                FilterStrength &f = m_strengths[s][i4];
                int level = base;
                if (m_useLfDelta) {
                    level += m_refLfDelta[0];
                    if (i4 != 0) {
                        level += m_modeLfDelta[0];
                    }
                }
                level = std::clamp(level, 0, 63);
                if (level > 0) {
                    int ilevel = level;
                    if (m_sharpness > 0) {
                        ilevel >>= m_sharpness > 4 ? 2 : 1;
                        ilevel = std::min(ilevel, 9 - m_sharpness);
                    }
                    ilevel = std::max(ilevel, 1);
                    f.innerLevel = ilevel;
                    f.limit = 2 * level + ilevel;
                    f.hevThreshold = level >= 40 ? 2 : level >= 15 ? 1 : 0;
                } else {
                    f.limit = 0;
                }
                f.inner = i4 != 0;
            }
        }
    }
    return success();
}

void Decoder::parseModes(MacroblockInfo &mb, std::size_t mbX) {
    std::uint8_t *top = m_intraTop.data() + 4 * mbX;
    std::uint8_t *left = m_intraLeft.data();
    mb.segment = 0;
    if (m_updateMap) {
        mb.segment = static_cast<std::uint8_t>(m_br.bit(m_segmentProbs[0]) == 0 ? m_br.bit(m_segmentProbs[1])
                                                                              : m_br.bit(m_segmentProbs[2]) + 2);
    }
    mb.skip = m_useSkipProb ? m_br.bit(m_skipProb) != 0 : false;
    mb.i4x4 = m_br.bit(145) == 0;
    if (!mb.i4x4) {
        const std::uint8_t ymode = m_br.bit(156) != 0 ? (m_br.bit(128) != 0 ? kBTm : kBHe)
                                                      : (m_br.bit(163) != 0 ? kBVe : kBDc);
        mb.modes[0] = ymode;
        std::memset(top, ymode, 4);
        std::memset(left, ymode, 4);
    } else {
        for (std::size_t y = 0; y < 4; ++y) {
            std::uint8_t ymode = left[y];
            for (std::size_t x = 0; x < 4; ++x) {
                const std::uint8_t *p = kBModeProbs.data() + (std::size_t{top[x]} * 10 + ymode) * 9;
                if (m_br.bit(p[0]) == 0) {
                    ymode = kBDc;
                } else if (m_br.bit(p[1]) == 0) {
                    ymode = kBTm;
                } else if (m_br.bit(p[2]) == 0) {
                    ymode = kBVe;
                } else if (m_br.bit(p[3]) == 0) {
                    ymode = m_br.bit(p[4]) == 0 ? kBHe : (m_br.bit(p[5]) == 0 ? kBRd : kBVr);
                } else if (m_br.bit(p[6]) == 0) {
                    ymode = kBLd;
                } else if (m_br.bit(p[7]) == 0) {
                    ymode = kBVl;
                } else {
                    ymode = m_br.bit(p[8]) == 0 ? kBHd : kBHu;
                }
                top[x] = ymode;
            }
            std::memcpy(mb.modes.data() + 4 * y, top, 4);
            left[y] = ymode;
        }
    }
    mb.uvMode = m_br.bit(142) == 0 ? kBDc : m_br.bit(114) == 0 ? kBVe : m_br.bit(183) != 0 ? kBTm : kBHe;
}

int Decoder::coefficients(BoolDecoder &br, int type, int ctx, const std::array<int, 2> &dq, int n, std::int16_t *out) {
    const std::uint8_t *p = probs(type, kBands[static_cast<std::size_t>(n)], ctx);
    for (; n < 16; ++n) {
        if (br.bit(p[0]) == 0) {
            return n;
        }
        while (br.bit(p[1]) == 0) {
            p = probs(type, kBands[static_cast<std::size_t>(++n)], 0);
            if (n == 16) {
                return 16;
            }
        }
        const std::size_t band = kBands[static_cast<std::size_t>(n + 1)];
        int v = 0;
        if (br.bit(p[2]) == 0) {
            v = 1;
            p = probs(type, static_cast<int>(band), 1);
        } else {
            if (br.bit(p[3]) == 0) {
                v = br.bit(p[4]) == 0 ? 2 : 3 + br.bit(p[5]);
            } else if (br.bit(p[6]) == 0) {
                if (br.bit(p[7]) == 0) {
                    v = 5 + br.bit(159);
                } else {
                    v = 7 + 2 * br.bit(165);
                    v += br.bit(145);
                }
            } else {
                const int bit1 = br.bit(p[8]);
                const int bit0 = br.bit(p[9 + bit1]);
                const int cat = 2 * bit1 + bit0;
                const std::uint8_t *tab = cat == 0 ? kCat3.data() : cat == 1 ? kCat4.data() : cat == 2 ? kCat5.data() : kCat6.data();
                v = 0;
                for (; *tab != 0; ++tab) {
                    v += v + br.bit(*tab);
                }
                v += 3 + (8 << cat);
            }
            p = probs(type, static_cast<int>(band), 2);
        }
        // Stored in 16 bits as libwebp stores it (hostile values wrap).
        out[kZigzag[static_cast<std::size_t>(n)]] = static_cast<std::int16_t>(br.signedValue(v) * dq[n > 0 ? 1 : 0]);
    }
    return 16;
}

std::uint32_t nzCodeBits(std::uint32_t nzCoeffs, int nz, bool dcNz) noexcept {
    nzCoeffs <<= 2;
    nzCoeffs |= nz > 3 ? 3u : nz > 1 ? 2u : (dcNz ? 1u : 0u);
    return nzCoeffs;
}

void transformWht(const std::int16_t *in, std::int16_t *out) noexcept {
    std::array<int, 16> tmp{};
    for (std::size_t i = 0; i < 4; ++i) {
        const int a0 = in[0 + i] + in[12 + i];
        const int a1 = in[4 + i] + in[8 + i];
        const int a2 = in[4 + i] - in[8 + i];
        const int a3 = in[0 + i] - in[12 + i];
        tmp[0 + i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const int dc = tmp[0 + i * 4] + 3;
        const int a0 = dc + tmp[3 + i * 4];
        const int a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        const int a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
        const int a3 = dc - tmp[3 + i * 4];
        out[0] = static_cast<std::int16_t>((a0 + a1) >> 3);
        out[16] = static_cast<std::int16_t>((a3 + a2) >> 3);
        out[32] = static_cast<std::int16_t>((a0 - a1) >> 3);
        out[48] = static_cast<std::int16_t>((a3 - a2) >> 3);
        out += 64;
    }
}

bool Decoder::parseResiduals(MacroblockInfo &mb, std::size_t mbX, BoolDecoder &tokens) {
    Nz &top = m_nzTop[mbX];
    Nz &left = m_nzLeft;
    const Quant &q = m_quant[mb.segment];
    std::int16_t *dst = mb.coeffs.data();
    mb.coeffs.fill(0);
    int first = 0;
    int acType = 3;
    if (!mb.i4x4) {
        std::array<std::int16_t, 16> dc{};
        const int ctx = top.nzDc + left.nzDc;
        const int nz = coefficients(tokens, 1, ctx, q.y2, 0, dc.data());
        top.nzDc = left.nzDc = static_cast<std::uint8_t>(nz > 0 ? 1 : 0);
        if (nz > 1) {
            transformWht(dc.data(), dst);
        } else {
            const auto dc0 = static_cast<std::int16_t>((dc[0] + 3) >> 3);
            for (std::size_t i = 0; i < 256; i += 16) {
                dst[i] = dc0;
            }
        }
        first = 1;
        acType = 0;
    }
    std::uint32_t nonZeroY = 0;
    std::uint32_t nonZeroUv = 0;
    std::uint32_t tnz = top.nz & 0x0Fu;
    std::uint32_t lnz = left.nz & 0x0Fu;
    for (int y = 0; y < 4; ++y) {
        std::uint32_t l = lnz & 1u;
        std::uint32_t nzCoeffs = 0;
        for (int x = 0; x < 4; ++x) {
            const int ctx = static_cast<int>(l + (tnz & 1u));
            const int nz = coefficients(tokens, acType, ctx, q.y1, first, dst);
            l = nz > first ? 1u : 0u;
            tnz = (tnz >> 1) | (l << 7);
            nzCoeffs = nzCodeBits(nzCoeffs, nz, dst[0] != 0);
            dst += 16;
        }
        tnz >>= 4;
        lnz = (lnz >> 1) | (l << 7);
        nonZeroY = (nonZeroY << 8) | nzCoeffs;
    }
    std::uint32_t outTnz = tnz;
    std::uint32_t outLnz = lnz >> 4;
    for (int ch = 0; ch < 4; ch += 2) {
        std::uint32_t nzCoeffs = 0;
        tnz = static_cast<std::uint32_t>(top.nz) >> (4 + ch);
        lnz = static_cast<std::uint32_t>(left.nz) >> (4 + ch);
        for (int y = 0; y < 2; ++y) {
            std::uint32_t l = lnz & 1u;
            for (int x = 0; x < 2; ++x) {
                const int ctx = static_cast<int>(l + (tnz & 1u));
                const int nz = coefficients(tokens, 2, ctx, q.uv, 0, dst);
                l = nz > 0 ? 1u : 0u;
                tnz = (tnz >> 1) | (l << 3);
                nzCoeffs = nzCodeBits(nzCoeffs, nz, dst[0] != 0);
                dst += 16;
            }
            tnz >>= 2;
            lnz = (lnz >> 1) | (l << 5);
        }
        nonZeroUv |= nzCoeffs << (4 * ch);
        outTnz |= (tnz << 4) << ch;
        outLnz |= (lnz & 0xF0u) << ch;
    }
    top.nz = static_cast<std::uint8_t>(outTnz);
    left.nz = static_cast<std::uint8_t>(outLnz);
    mb.nonZeroY = nonZeroY;
    mb.nonZeroUv = nonZeroUv;
    return (nonZeroY | nonZeroUv) == 0;
}

// --- Reconstruction (dsp/dec.c) ----------------------------------------------

constexpr std::size_t kBps = 32; // work-buffer stride

int mul1(int a) noexcept { return ((a * 20091) >> 16) + a; }
int mul2(int a) noexcept { return (a * 35468) >> 16; }

void transform(const std::int16_t *in, std::uint8_t *dst) noexcept {
    std::array<int, 16> c{};
    int *tmp = c.data();
    for (std::size_t i = 0; i < 4; ++i, ++in, tmp += 4) {
        const int a = in[0] + in[8];
        const int b = in[0] - in[8];
        const int cc = mul2(in[4]) - mul1(in[12]);
        const int d = mul1(in[4]) + mul2(in[12]);
        tmp[0] = a + d;
        tmp[1] = b + cc;
        tmp[2] = b - cc;
        tmp[3] = a - d;
    }
    tmp = c.data();
    for (std::size_t i = 0; i < 4; ++i, ++tmp, dst += kBps) {
        const int dc = tmp[0] + 4;
        const int a = dc + tmp[8];
        const int b = dc - tmp[8];
        const int cc = mul2(tmp[4]) - mul1(tmp[12]);
        const int d = mul1(tmp[4]) + mul2(tmp[12]);
        dst[0] = clip8(dst[0] + ((a + d) >> 3));
        dst[1] = clip8(dst[1] + ((b + cc) >> 3));
        dst[2] = clip8(dst[2] + ((b - cc) >> 3));
        dst[3] = clip8(dst[3] + ((a - d) >> 3));
    }
}

void transformDc(const std::int16_t *in, std::uint8_t *dst) noexcept {
    const int dc = in[0] + 4;
    for (std::size_t j = 0; j < 4; ++j) {
        for (std::size_t i = 0; i < 4; ++i) {
            dst[j * kBps + i] = clip8(dst[j * kBps + i] + (dc >> 3));
        }
    }
}

// Applies a block's residual given its 2-bit non-zero code.
void addResidual(std::uint32_t code, const std::int16_t *in, std::uint8_t *dst) noexcept {
    if (code == 1) {
        transformDc(in, dst);
    } else if (code != 0) {
        transform(in, dst);
    }
}

std::uint8_t avg3(int a, int b, int c) noexcept { return static_cast<std::uint8_t>((a + 2 * b + c + 2) >> 2); }
std::uint8_t avg2(int a, int b) noexcept { return static_cast<std::uint8_t>((a + b + 1) >> 1); }

void trueMotion(std::uint8_t *dst, std::size_t size) noexcept {
    const std::uint8_t *top = dst - kBps;
    const int topLeft = top[-1];
    for (std::size_t y = 0; y < size; ++y, dst += kBps) {
        const int left = dst[-1];
        for (std::size_t x = 0; x < size; ++x) {
            dst[x] = clip8(top[x] + left - topLeft);
        }
    }
}

void fill(std::uint8_t *dst, std::size_t size, std::uint8_t v) noexcept {
    for (std::size_t j = 0; j < size; ++j) {
        std::memset(dst + j * kBps, v, size);
    }
}

// 16x16 luma and 8x8 chroma prediction.
void predictBlock(std::uint8_t *dst, std::size_t size, int mode) noexcept {
    const int shift = size == 16 ? 5 : 4;
    switch (mode) {
    case kBTm: trueMotion(dst, size); break;
    case kBVe:
        for (std::size_t j = 0; j < size; ++j) {
            std::memcpy(dst + j * kBps, dst - kBps, size);
        }
        break;
    case kBHe:
        for (std::size_t j = 0; j < size; ++j) {
            std::memset(dst + j * kBps, dst[j * kBps - 1], size);
        }
        break;
    case kDcNoTop: {
        int dc = static_cast<int>(size / 2);
        for (std::size_t j = 0; j < size; ++j) {
            dc += dst[j * kBps - 1];
        }
        fill(dst, size, static_cast<std::uint8_t>(dc >> (shift - 1)));
        break;
    }
    case kDcNoLeft: {
        int dc = static_cast<int>(size / 2);
        for (std::size_t i = 0; i < size; ++i) {
            dc += dst[i - kBps];
        }
        fill(dst, size, static_cast<std::uint8_t>(dc >> (shift - 1)));
        break;
    }
    case kDcNoTopLeft: fill(dst, size, 0x80); break;
    default: { // DC
        int dc = static_cast<int>(size);
        for (std::size_t j = 0; j < size; ++j) {
            dc += dst[j * kBps - 1] + dst[j - kBps];
        }
        fill(dst, size, static_cast<std::uint8_t>(dc >> shift));
        break;
    }
    }
}

void predict4(std::uint8_t *dst, int mode) noexcept {
    const auto at = [&](int x, int y) -> std::uint8_t & {
        return dst[static_cast<std::ptrdiff_t>(x) + static_cast<std::ptrdiff_t>(y) * static_cast<std::ptrdiff_t>(kBps)];
    };
    const int I = at(-1, 0), J = at(-1, 1), K = at(-1, 2), L = at(-1, 3), X = at(-1, -1);
    const int A = at(0, -1), B = at(1, -1), C = at(2, -1), D = at(3, -1);
    const int E = at(4, -1), F = at(5, -1), G = at(6, -1), H = at(7, -1);
    switch (mode) {
    case kBDc: {
        unsigned dc = 4;
        for (int i = 0; i < 4; ++i) {
            dc += static_cast<unsigned>(at(i, -1) + at(-1, i));
        }
        dc >>= 3;
        for (int y = 0; y < 4; ++y) {
            std::memset(&at(0, y), static_cast<int>(dc), 4);
        }
        break;
    }
    case kBTm: trueMotion(dst, 4); break;
    case kBVe: {
        const std::uint8_t vals[4] = {avg3(X, A, B), avg3(A, B, C), avg3(B, C, D), avg3(C, D, E)};
        for (int y = 0; y < 4; ++y) {
            std::memcpy(&at(0, y), vals, 4);
        }
        break;
    }
    case kBHe: {
        std::memset(&at(0, 0), avg3(X, I, J), 4);
        std::memset(&at(0, 1), avg3(I, J, K), 4);
        std::memset(&at(0, 2), avg3(J, K, L), 4);
        std::memset(&at(0, 3), avg3(K, L, L), 4);
        break;
    }
    case kBRd:
        at(0, 3) = avg3(J, K, L);
        at(1, 3) = at(0, 2) = avg3(I, J, K);
        at(2, 3) = at(1, 2) = at(0, 1) = avg3(X, I, J);
        at(3, 3) = at(2, 2) = at(1, 1) = at(0, 0) = avg3(A, X, I);
        at(3, 2) = at(2, 1) = at(1, 0) = avg3(B, A, X);
        at(3, 1) = at(2, 0) = avg3(C, B, A);
        at(3, 0) = avg3(D, C, B);
        break;
    case kBLd:
        at(0, 0) = avg3(A, B, C);
        at(1, 0) = at(0, 1) = avg3(B, C, D);
        at(2, 0) = at(1, 1) = at(0, 2) = avg3(C, D, E);
        at(3, 0) = at(2, 1) = at(1, 2) = at(0, 3) = avg3(D, E, F);
        at(3, 1) = at(2, 2) = at(1, 3) = avg3(E, F, G);
        at(3, 2) = at(2, 3) = avg3(F, G, H);
        at(3, 3) = avg3(G, H, H);
        break;
    case kBVr:
        at(0, 0) = at(1, 2) = avg2(X, A);
        at(1, 0) = at(2, 2) = avg2(A, B);
        at(2, 0) = at(3, 2) = avg2(B, C);
        at(3, 0) = avg2(C, D);
        at(0, 3) = avg3(K, J, I);
        at(0, 2) = avg3(J, I, X);
        at(0, 1) = at(1, 3) = avg3(I, X, A);
        at(1, 1) = at(2, 3) = avg3(X, A, B);
        at(2, 1) = at(3, 3) = avg3(A, B, C);
        at(3, 1) = avg3(B, C, D);
        break;
    case kBVl:
        at(0, 0) = avg2(A, B);
        at(1, 0) = at(0, 2) = avg2(B, C);
        at(2, 0) = at(1, 2) = avg2(C, D);
        at(3, 0) = at(2, 2) = avg2(D, E);
        at(0, 1) = avg3(A, B, C);
        at(1, 1) = at(0, 3) = avg3(B, C, D);
        at(2, 1) = at(1, 3) = avg3(C, D, E);
        at(3, 1) = at(2, 3) = avg3(D, E, F);
        at(3, 2) = avg3(E, F, G);
        at(3, 3) = avg3(F, G, H);
        break;
    case kBHu:
        at(0, 0) = avg2(I, J);
        at(2, 0) = at(0, 1) = avg2(J, K);
        at(2, 1) = at(0, 2) = avg2(K, L);
        at(1, 0) = avg3(I, J, K);
        at(3, 0) = at(1, 1) = avg3(J, K, L);
        at(3, 1) = at(1, 2) = avg3(K, L, L);
        at(3, 2) = at(2, 2) = at(0, 3) = at(1, 3) = at(2, 3) = at(3, 3) = static_cast<std::uint8_t>(L);
        break;
    default: // kBHd
        at(0, 0) = at(2, 1) = avg2(I, X);
        at(0, 1) = at(2, 2) = avg2(J, I);
        at(0, 2) = at(2, 3) = avg2(K, J);
        at(0, 3) = avg2(L, K);
        at(3, 0) = avg3(A, B, C);
        at(2, 0) = avg3(X, A, B);
        at(1, 0) = at(3, 1) = avg3(I, X, A);
        at(1, 1) = at(3, 2) = avg3(J, I, X);
        at(1, 2) = at(3, 3) = avg3(K, J, I);
        at(1, 3) = avg3(L, K, J);
        break;
    }
}

int checkMode(std::size_t mbX, std::size_t mbY, int mode) noexcept {
    if (mode != kBDc) {
        return mode;
    }
    if (mbX == 0) {
        return mbY == 0 ? kDcNoTopLeft : kDcNoLeft;
    }
    return mbY == 0 ? kDcNoTop : kBDc;
}

void Decoder::reconstruct(const MacroblockInfo &mb, std::size_t mbX, std::size_t mbY) {
    // Work buffers with a one-sample border (four on the left for
    // alignment, four extra on the right for 4x4 top-right samples).
    std::array<std::uint8_t, kBps * 17> yWork{};
    std::array<std::uint8_t, kBps * 9> uWork{};
    std::array<std::uint8_t, kBps * 9> vWork{};
    std::uint8_t *yDst = yWork.data() + kBps + 8;
    std::uint8_t *uDst = uWork.data() + kBps + 8;
    std::uint8_t *vDst = vWork.data() + kBps + 8;
    const std::size_t x0 = mbX * 16;
    const std::size_t y0 = mbY * 16;
    const std::size_t cx0 = mbX * 8;
    const std::size_t cy0 = mbY * 8;

    // Left samples: 129 at the frame edge, else the reconstructed (not yet
    // filtered) column to the left.
    for (std::size_t j = 0; j < 16; ++j) {
        yDst[j * kBps - 1] = mbX == 0 ? 129 : m_y[(y0 + j) * m_yStride + x0 - 1];
    }
    for (std::size_t j = 0; j < 8; ++j) {
        uDst[j * kBps - 1] = mbX == 0 ? 129 : m_u[(cy0 + j) * m_uvStride + cx0 - 1];
        vDst[j * kBps - 1] = mbX == 0 ? 129 : m_v[(cy0 + j) * m_uvStride + cx0 - 1];
    }
    // Top samples (and top-left, and luma top-right): 127 above the frame.
    if (mbY == 0) {
        std::memset(yDst - kBps - 1, 127, 16 + 4 + 1);
        std::memset(uDst - kBps - 1, 127, 8 + 1);
        std::memset(vDst - kBps - 1, 127, 8 + 1);
    } else {
        const std::uint8_t *above = m_y.data() + (y0 - 1) * m_yStride + x0;
        std::memcpy(yDst - kBps, above, 16);
        std::memcpy(uDst - kBps, m_u.data() + (cy0 - 1) * m_uvStride + cx0, 8);
        std::memcpy(vDst - kBps, m_v.data() + (cy0 - 1) * m_uvStride + cx0, 8);
        yDst[-1 - static_cast<std::ptrdiff_t>(kBps)] = mbX == 0 ? 129 : above[-1];
        uDst[-1 - static_cast<std::ptrdiff_t>(kBps)] = mbX == 0 ? 129 : m_u[(cy0 - 1) * m_uvStride + cx0 - 1];
        vDst[-1 - static_cast<std::ptrdiff_t>(kBps)] = mbX == 0 ? 129 : m_v[(cy0 - 1) * m_uvStride + cx0 - 1];
        if (mbX + 1 >= m_mbW) {
            std::memset(yDst - kBps + 16, above[15], 4);
        } else {
            std::memcpy(yDst - kBps + 16, above + 16, 4);
        }
    }

    const std::int16_t *coeffs = mb.coeffs.data();
    std::uint32_t bits = mb.nonZeroY;
    if (mb.i4x4) {
        // Sub-blocks in the right column of rows 1-3 use the macroblock's
        // top-right samples.
        for (std::size_t r = 1; r < 4; ++r) {
            std::memcpy(yDst + (4 * r - 1) * kBps + 16, yDst - kBps + 16, 4);
        }
        for (std::size_t n = 0; n < 16; ++n, bits <<= 2) {
            std::uint8_t *dst = yDst + (n / 4) * 4 * kBps + (n % 4) * 4;
            predict4(dst, mb.modes[n]);
            addResidual(bits >> 30, coeffs + n * 16, dst);
        }
    } else {
        predictBlock(yDst, 16, checkMode(mbX, mbY, mb.modes[0]));
        for (std::size_t n = 0; n < 16 && bits != 0; ++n, bits <<= 2) {
            addResidual(bits >> 30, coeffs + n * 16, yDst + (n / 4) * 4 * kBps + (n % 4) * 4);
        }
    }
    const int uvMode = checkMode(mbX, mbY, mb.uvMode);
    predictBlock(uDst, 8, uvMode);
    predictBlock(vDst, 8, uvMode);
    const auto chroma = [&](std::uint32_t uvBits, const std::int16_t *in, std::uint8_t *dst) {
        if ((uvBits & 0xFFu) == 0) {
            return;
        }
        for (std::size_t b = 0; b < 4; ++b) {
            std::uint8_t *d = dst + (b / 2) * 4 * kBps + (b % 2) * 4;
            if ((uvBits & 0xAAu) != 0) {
                transform(in + b * 16, d);
            } else if (in[b * 16] != 0) {
                transformDc(in + b * 16, d);
            }
        }
    };
    chroma(mb.nonZeroUv, coeffs + 16 * 16, uDst);
    chroma(mb.nonZeroUv >> 8, coeffs + 20 * 16, vDst);

    for (std::size_t j = 0; j < 16; ++j) {
        std::memcpy(m_y.data() + (y0 + j) * m_yStride + x0, yDst + j * kBps, 16);
    }
    for (std::size_t j = 0; j < 8; ++j) {
        std::memcpy(m_u.data() + (cy0 + j) * m_uvStride + cx0, uDst + j * kBps, 8);
        std::memcpy(m_v.data() + (cy0 + j) * m_uvStride + cx0, vDst + j * kBps, 8);
    }
}

// --- Loop filter (RFC 6386 §15, dsp/dec.c) ------------------------------------

int sclip1(int v) noexcept { return std::clamp(v, -128, 127); }
int sclip2(int v) noexcept { return std::clamp(v, -16, 15); }

void doFilter2(std::uint8_t *p, std::ptrdiff_t step) noexcept {
    const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    const int a = 3 * (q0 - p0) + sclip1(p1 - q1);
    const int a1 = sclip2((a + 4) >> 3);
    const int a2 = sclip2((a + 3) >> 3);
    p[-step] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
}

void doFilter4(std::uint8_t *p, std::ptrdiff_t step) noexcept {
    const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    const int a = 3 * (q0 - p0);
    const int a1 = sclip2((a + 4) >> 3);
    const int a2 = sclip2((a + 3) >> 3);
    const int a3 = (a1 + 1) >> 1;
    p[-2 * step] = clip8(p1 + a3);
    p[-step] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
    p[step] = clip8(q1 - a3);
}

void doFilter6(std::uint8_t *p, std::ptrdiff_t step) noexcept {
    const int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    const int q0 = p[0], q1 = p[step], q2 = p[2 * step];
    const int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    const int a1 = (27 * a + 63) >> 7;
    const int a2 = (18 * a + 63) >> 7;
    const int a3 = (9 * a + 63) >> 7;
    p[-3 * step] = clip8(p2 + a3);
    p[-2 * step] = clip8(p1 + a2);
    p[-step] = clip8(p0 + a1);
    p[0] = clip8(q0 - a1);
    p[step] = clip8(q1 - a2);
    p[2 * step] = clip8(q2 - a3);
}

bool hev(const std::uint8_t *p, std::ptrdiff_t step, int thresh) noexcept {
    const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    return std::abs(p1 - p0) > thresh || std::abs(q1 - q0) > thresh;
}

bool needsFilter(const std::uint8_t *p, std::ptrdiff_t step, int t) noexcept {
    const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    return 4 * std::abs(p0 - q0) + std::abs(p1 - q1) <= t;
}

bool needsFilter2(const std::uint8_t *p, std::ptrdiff_t step, int t, int it) noexcept {
    const int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step];
    const int p0 = p[-step], q0 = p[0];
    const int q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
    if (4 * std::abs(p0 - q0) + std::abs(p1 - q1) > t) {
        return false;
    }
    return std::abs(p3 - p2) <= it && std::abs(p2 - p1) <= it && std::abs(p1 - p0) <= it && std::abs(q3 - q2) <= it &&
           std::abs(q2 - q1) <= it && std::abs(q1 - q0) <= it;
}

void simpleFilter(std::uint8_t *p, std::ptrdiff_t step, std::ptrdiff_t along, int thresh) noexcept {
    const int t2 = 2 * thresh + 1;
    for (int i = 0; i < 16; ++i, p += along) {
        if (needsFilter(p, step, t2)) {
            doFilter2(p, step);
        }
    }
}

void filterLoop(std::uint8_t *p, std::ptrdiff_t hstride, std::ptrdiff_t vstride, int size, int thresh, int ithresh,
                int hevThresh, bool edge) noexcept {
    const int t2 = 2 * thresh + 1;
    while (size-- > 0) {
        if (needsFilter2(p, hstride, t2, ithresh)) {
            if (hev(p, hstride, hevThresh)) {
                doFilter2(p, hstride);
            } else if (edge) {
                doFilter6(p, hstride);
            } else {
                doFilter4(p, hstride);
            }
        }
        p += vstride;
    }
}

void Decoder::filterFrame() {
    const auto ys = static_cast<std::ptrdiff_t>(m_yStride);
    const auto uvs = static_cast<std::ptrdiff_t>(m_uvStride);
    for (std::size_t mbY = 0; mbY < m_mbH; ++mbY) {
        for (std::size_t mbX = 0; mbX < m_mbW; ++mbX) {
            const FilterStrength &f = m_filterInfo[mbY * m_mbW + mbX];
            if (f.limit == 0) {
                continue;
            }
            std::uint8_t *y = m_y.data() + mbY * 16 * m_yStride + mbX * 16;
            const int limit = f.limit;
            if (m_filterType == 1) {
                if (mbX > 0) {
                    simpleFilter(y, 1, ys, limit + 4);
                }
                if (f.inner) {
                    for (int k = 1; k < 4; ++k) {
                        simpleFilter(y + 4 * k, 1, ys, limit);
                    }
                }
                if (mbY > 0) {
                    simpleFilter(y, ys, 1, limit + 4);
                }
                if (f.inner) {
                    for (int k = 1; k < 4; ++k) {
                        simpleFilter(y + 4 * k * ys, ys, 1, limit);
                    }
                }
                continue;
            }
            std::uint8_t *u = m_u.data() + mbY * 8 * m_uvStride + mbX * 8;
            std::uint8_t *v = m_v.data() + mbY * 8 * m_uvStride + mbX * 8;
            const int il = f.innerLevel;
            const int hevT = f.hevThreshold;
            if (mbX > 0) {
                filterLoop(y, 1, ys, 16, limit + 4, il, hevT, true);
                filterLoop(u, 1, uvs, 8, limit + 4, il, hevT, true);
                filterLoop(v, 1, uvs, 8, limit + 4, il, hevT, true);
            }
            if (f.inner) {
                for (int k = 1; k < 4; ++k) {
                    filterLoop(y + 4 * k, 1, ys, 16, limit, il, hevT, false);
                }
                filterLoop(u + 4, 1, uvs, 8, limit, il, hevT, false);
                filterLoop(v + 4, 1, uvs, 8, limit, il, hevT, false);
            }
            if (mbY > 0) {
                filterLoop(y, ys, 1, 16, limit + 4, il, hevT, true);
                filterLoop(u, uvs, 1, 8, limit + 4, il, hevT, true);
                filterLoop(v, uvs, 1, 8, limit + 4, il, hevT, true);
            }
            if (f.inner) {
                for (int k = 1; k < 4; ++k) {
                    filterLoop(y + 4 * k * ys, ys, 1, 16, limit, il, hevT, false);
                }
                filterLoop(u + 4 * uvs, uvs, 1, 8, limit, il, hevT, false);
                filterLoop(v + 4 * uvs, uvs, 1, 8, limit, il, hevT, false);
            }
        }
    }
}

// --- YUV 4:2:0 to RGB (dsp/yuv.h, dsp/upsampling.c, dec/io_dec.c) -----------

int multHi(int v, int coeff) noexcept { return (v * coeff) >> 8; }
std::uint8_t clipYuv(int v) noexcept {
    return static_cast<std::uint8_t>((v & ~((256 << 6) - 1)) == 0 ? (v >> 6) : v < 0 ? 0 : 255);
}
void yuvToRgba(int y, int u, int v, std::uint8_t *out) noexcept {
    out[0] = clipYuv(multHi(y, 19077) + multHi(v, 26149) - 14234);
    out[1] = clipYuv(multHi(y, 19077) - multHi(u, 6419) - multHi(v, 13320) + 8708);
    out[2] = clipYuv(multHi(y, 19077) + multHi(u, 33050) - 17685);
    out[3] = 255;
}

// One output row of libwebp's fancy upsampler. Every output sample mixes
// chroma from two rows: `n` (the nearer, weighted 3/4 vertically) and `f`
// (the farther). At the top edge, and the bottom edge of an even height,
// both are the same row. Written per component; libwebp packs U and V into
// one word, which cannot carry between them.
void upsampleRow(const std::uint8_t *yRow, const std::uint8_t *nU, const std::uint8_t *nV, const std::uint8_t *fU,
                 const std::uint8_t *fV, std::size_t width, std::uint8_t *out) noexcept {
    const auto single = [](const std::uint8_t *n, const std::uint8_t *f, std::size_t c) {
        return (3 * n[c] + f[c] + 2) >> 2;
    };
    const auto left = [](const std::uint8_t *n, const std::uint8_t *f, std::size_t x) {
        const int avg = n[x - 1] + n[x] + f[x - 1] + f[x] + 8;
        return (((avg + 2 * (n[x] + f[x - 1])) >> 3) + n[x - 1]) >> 1;
    };
    const auto right = [](const std::uint8_t *n, const std::uint8_t *f, std::size_t x) {
        const int avg = n[x - 1] + n[x] + f[x - 1] + f[x] + 8;
        return (((avg + 2 * (n[x - 1] + f[x])) >> 3) + n[x]) >> 1;
    };
    yuvToRgba(yRow[0], single(nU, fU, 0), single(nV, fV, 0), out);
    const std::size_t lastPair = (width - 1) >> 1;
    for (std::size_t x = 1; x <= lastPair; ++x) {
        yuvToRgba(yRow[2 * x - 1], left(nU, fU, x), left(nV, fV, x), out + (2 * x - 1) * 4);
        yuvToRgba(yRow[2 * x], right(nU, fU, x), right(nV, fV, x), out + 2 * x * 4);
    }
    if ((width & 1) == 0) {
        yuvToRgba(yRow[width - 1], single(nU, fU, lastPair), single(nV, fV, lastPair), out + (width - 1) * 4);
    }
}

Rgba Decoder::toRgba() const {
    Rgba out;
    out.width = m_width;
    out.height = m_height;
    out.pixels.resize(std::size_t{m_width} * m_height * 4);
    const std::size_t chromaH = (m_height + 1) / 2;
    for (std::size_t y = 0; y < m_height; ++y) {
        // Output row y weights chroma row `near` by 3/4 and `far` by 1/4.
        std::size_t nearRow = 0;
        std::size_t farRow = 0;
        if (y == 0) {
            nearRow = farRow = 0;
        } else if ((y & 1) != 0) {
            nearRow = (y - 1) / 2;
            farRow = std::min(nearRow + 1, chromaH - 1);
        } else {
            nearRow = y / 2;
            farRow = y / 2 - 1;
        }
        upsampleRow(m_y.data() + y * m_yStride, m_u.data() + nearRow * m_uvStride, m_v.data() + nearRow * m_uvStride,
                    m_u.data() + farRow * m_uvStride, m_v.data() + farRow * m_uvStride, m_width,
                    out.pixels.data() + y * m_width * 4);
    }
    return out;
}

Result<Rgba> Decoder::run() {
    if (Result<void> ok = parseHeaders(); !ok) {
        return std::move(ok).error();
    }
    m_yStride = m_mbW * 16;
    m_uvStride = m_mbW * 8;
    m_y.assign(m_yStride * m_mbH * 16, 0);
    m_u.assign(m_uvStride * m_mbH * 8, 0);
    m_v.assign(m_uvStride * m_mbH * 8, 0);
    m_intraTop.assign(m_mbW * 4, kBDc);
    m_nzTop.assign(m_mbW, Nz{});
    m_filterInfo.assign(m_mbW * m_mbH, FilterStrength{});
    std::vector<MacroblockInfo> row(m_mbW);
    for (std::size_t mbY = 0; mbY < m_mbH; ++mbY) {
        BoolDecoder &tokens = m_parts[mbY & m_partMask];
        for (std::size_t mbX = 0; mbX < m_mbW; ++mbX) {
            parseModes(row[mbX], mbX);
        }
        if (m_br.eof()) {
            return corrupt("first partition is truncated");
        }
        m_nzLeft = Nz{};
        m_intraLeft.fill(kBDc);
        for (std::size_t mbX = 0; mbX < m_mbW; ++mbX) {
            MacroblockInfo &mb = row[mbX];
            bool skip = mb.skip;
            if (!skip) {
                skip = parseResiduals(mb, mbX, tokens);
            } else {
                m_nzLeft.nz = m_nzTop[mbX].nz = 0;
                if (!mb.i4x4) {
                    m_nzLeft.nzDc = m_nzTop[mbX].nzDc = 0;
                }
                mb.nonZeroY = 0;
                mb.nonZeroUv = 0;
            }
            if (m_filterType > 0) {
                FilterStrength f = m_strengths[mb.segment][mb.i4x4 ? 1 : 0];
                f.inner = f.inner || !skip;
                m_filterInfo[mbY * m_mbW + mbX] = f;
            }
            if (tokens.eof()) {
                return corrupt("token partition is truncated");
            }
            reconstruct(mb, mbX, mbY);
        }
    }
    if (m_filterType > 0) {
        filterFrame();
    }
    return toRgba();
}

} // namespace

Result<Rgba> decodeLossy(Span<const std::byte> data, const ImageLimits &limits) {
    try {
        Decoder decoder(reinterpret_cast<const std::uint8_t *>(data.data()), data.size(), limits);
        return decoder.run();
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "webp lossy: out of memory");
    }
}

} // namespace cfw::webp
