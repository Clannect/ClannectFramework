// WebP lossless (VP8L) decoder, from the WebP Lossless Bitstream
// specification (RFC 9649 §3-7).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "WebPInternal.h"
#include "cfw/image/Image.h"

namespace cfw::webp {

namespace {

Error corrupt(const char *what) { return Error(ErrorCode::Corrupt, String("webp lossless: ") + what); }

// LSB-first bit reader. Reads past the end yield zeros and are recorded.
class BitReader {
public:
    explicit BitReader(Span<const std::byte> data) noexcept
        : m_data(reinterpret_cast<const std::uint8_t *>(data.data())), m_size(data.size()) {}

    [[nodiscard]] std::uint32_t bits(unsigned n) noexcept {
        if (n == 0) {
            return 0;
        }
        fill();
        const auto v = static_cast<std::uint32_t>(m_bits & ((std::uint64_t{1} << n) - 1u));
        consume(n);
        return v;
    }
    [[nodiscard]] std::uint32_t peek(unsigned n) noexcept {
        fill();
        return static_cast<std::uint32_t>(m_bits & ((std::uint64_t{1} << n) - 1u));
    }
    void consume(unsigned n) noexcept {
        m_bits >>= n;
        m_count -= n;
        m_consumed += n;
    }
    [[nodiscard]] std::uint64_t buffered() noexcept {
        fill();
        return m_bits;
    }
    [[nodiscard]] bool overrun() const noexcept { return m_consumed > std::uint64_t{m_size} * 8; }

private:
    void fill() noexcept {
        while (m_count <= 56) {
            const std::uint64_t byte = m_pos < m_size ? m_data[m_pos] : 0u;
            ++m_pos;
            m_bits |= byte << m_count;
            m_count += 8;
        }
    }

    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_pos = 0;
    std::uint64_t m_bits = 0;
    unsigned m_count = 0;
    std::uint64_t m_consumed = 0;
};

// A canonical prefix code (the same construction as DEFLATE's). A code with
// a single used symbol decodes it with no bits, as the format requires.
class PrefixCode {
public:
    static constexpr unsigned kFastBits = 8;
    static constexpr unsigned kMaxLength = 15;

    // Fails on an all-zero code or one that is not complete.
    [[nodiscard]] bool build(const std::uint8_t *lengths, std::size_t n) {
        std::array<std::uint32_t, kMaxLength + 1> counts{};
        std::size_t used = 0;
        std::size_t last = 0;
        for (std::size_t i = 0; i < n; ++i) {
            ++counts[lengths[i]];
            if (lengths[i] != 0) {
                ++used;
                last = i;
            }
        }
        if (used == 0) {
            return false;
        }
        if (used == 1) {
            m_single = static_cast<int>(last);
            return true;
        }
        m_single = -1;
        int left = 1;
        for (unsigned len = 1; len <= kMaxLength; ++len) {
            left <<= 1;
            left -= static_cast<int>(counts[len]);
            if (left < 0) {
                return false;
            }
        }
        if (left != 0) {
            return false;
        }
        for (unsigned len = 0; len <= kMaxLength; ++len) {
            m_counts[len] = static_cast<std::uint16_t>(counts[len]);
        }
        std::array<std::uint32_t, kMaxLength + 2> offsets{};
        for (unsigned len = 1; len <= kMaxLength; ++len) {
            offsets[len + 1] = offsets[len] + counts[len];
        }
        m_symbols.assign(used, 0);
        for (std::size_t sym = 0; sym < n; ++sym) {
            if (lengths[sym] != 0) {
                m_symbols[offsets[lengths[sym]]++] = static_cast<std::uint16_t>(sym);
            }
        }
        m_fast.fill(0);
        std::array<std::uint32_t, kMaxLength + 1> nextCode{};
        std::uint32_t code = 0;
        for (unsigned len = 1; len <= kMaxLength; ++len) {
            nextCode[len] = code;
            code = (code + counts[len]) << 1;
        }
        for (std::size_t sym = 0; sym < n; ++sym) {
            const unsigned len = lengths[sym];
            if (len == 0) {
                continue;
            }
            const std::uint32_t c = nextCode[len]++;
            if (len > kFastBits) {
                continue;
            }
            std::uint32_t reversed = 0;
            for (unsigned i = 0; i < len; ++i) {
                reversed |= ((c >> i) & 1u) << (len - 1 - i);
            }
            for (std::uint32_t fill = reversed; fill < (1u << kFastBits); fill += 1u << len) {
                m_fast[fill] = static_cast<std::uint16_t>(sym << 4 | len);
            }
        }
        return true;
    }

    [[nodiscard]] int decode(BitReader &in) const noexcept {
        if (m_single >= 0) {
            return m_single;
        }
        const std::uint16_t entry = m_fast[in.peek(kFastBits)];
        if (entry != 0) {
            in.consume(entry & 15u);
            return entry >> 4;
        }
        const std::uint64_t bits = in.buffered();
        int code = 0;
        int first = 0;
        int index = 0;
        for (unsigned len = 1; len <= kMaxLength; ++len) {
            code |= static_cast<int>((bits >> (len - 1)) & 1u);
            const int count = m_counts[len];
            if (code - count < first) {
                in.consume(len);
                return m_symbols[static_cast<std::size_t>(index + (code - first))];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return 0; // unreachable for a complete code
    }

    [[nodiscard]] std::size_t memoryBytes() const noexcept { return sizeof(*this) + m_symbols.size() * 2; }

private:
    int m_single = -1;
    std::array<std::uint16_t, kMaxLength + 1> m_counts{};
    std::vector<std::uint16_t> m_symbols;
    std::array<std::uint16_t, 1u << kFastBits> m_fast{};
};

constexpr std::array<std::uint8_t, 19> kCodeLengthOrder = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// Distance codes 1..120: (xi, yi) offsets of the close neighbourhood.
constexpr std::array<std::array<std::int8_t, 2>, 120> kDistanceMap = {{
    {0, 1},  {1, 0},  {1, 1},  {-1, 1}, {0, 2},  {2, 0},  {1, 2},  {-1, 2}, {2, 1},  {-2, 1}, {2, 2},  {-2, 2},
    {0, 3},  {3, 0},  {1, 3},  {-1, 3}, {3, 1},  {-3, 1}, {2, 3},  {-2, 3}, {3, 2},  {-3, 2}, {0, 4},  {4, 0},
    {1, 4},  {-1, 4}, {4, 1},  {-4, 1}, {3, 3},  {-3, 3}, {2, 4},  {-2, 4}, {4, 2},  {-4, 2}, {0, 5},  {3, 4},
    {-3, 4}, {4, 3},  {-4, 3}, {5, 0},  {1, 5},  {-1, 5}, {5, 1},  {-5, 1}, {2, 5},  {-2, 5}, {5, 2},  {-5, 2},
    {4, 4},  {-4, 4}, {3, 5},  {-3, 5}, {5, 3},  {-5, 3}, {0, 6},  {6, 0},  {1, 6},  {-1, 6}, {6, 1},  {-6, 1},
    {2, 6},  {-2, 6}, {6, 2},  {-6, 2}, {4, 5},  {-4, 5}, {5, 4},  {-5, 4}, {3, 6},  {-3, 6}, {6, 3},  {-6, 3},
    {0, 7},  {7, 0},  {1, 7},  {-1, 7}, {5, 5},  {-5, 5}, {7, 1},  {-7, 1}, {4, 6},  {-4, 6}, {6, 4},  {-6, 4},
    {2, 7},  {-2, 7}, {7, 2},  {-7, 2}, {3, 7},  {-3, 7}, {7, 3},  {-7, 3}, {5, 6},  {-5, 6}, {6, 5},  {-6, 5},
    {8, 0},  {4, 7},  {-4, 7}, {7, 4},  {-7, 4}, {8, 1},  {8, 2},  {6, 6},  {-6, 6}, {8, 3},  {5, 7},  {-5, 7},
    {7, 5},  {-7, 5}, {8, 4},  {6, 7},  {-6, 7}, {7, 6},  {-7, 6}, {8, 5},  {7, 7},  {-7, 7}, {8, 6},  {8, 7},
}};

enum class TransformType : std::uint8_t { Predictor = 0, Color = 1, SubtractGreen = 2, ColorIndexing = 3 };

struct Transform {
    TransformType type{};
    std::uint32_t xsize = 0; // width of the image this transform outputs
    std::uint32_t ysize = 0;
    unsigned bits = 0;
    std::vector<std::uint32_t> data;
};

std::uint32_t subSampleSize(std::uint32_t size, unsigned bits) noexcept { return (size + (1u << bits) - 1u) >> bits; }

struct Group {
    std::array<PrefixCode, 5> codes;
};

class Decoder {
public:
    Decoder(Span<const std::byte> data, const ImageLimits &limits) noexcept : m_in(data), m_limits(limits) {}

    BitReader &in() noexcept { return m_in; }

    // DecodeImageStream: level 0 is the main image (transforms, meta codes).
    Result<std::vector<std::uint32_t>> imageStream(std::uint32_t xsize, std::uint32_t ysize, bool level0) {
        if (++m_depth > 8) {
            return corrupt("image streams nested too deeply");
        }
        std::uint32_t transformXsize = xsize;
        if (level0) {
            while (m_in.bits(1) != 0) {
                if (Result<void> ok = readTransform(transformXsize, ysize); !ok) {
                    return std::move(ok).error();
                }
            }
        }
        unsigned cacheBits = 0;
        if (m_in.bits(1) != 0) {
            cacheBits = m_in.bits(4);
            if (cacheBits < 1 || cacheBits > 11) {
                return corrupt("invalid colour cache size");
            }
        }
        // Meta prefix codes.
        unsigned prefixBits = 0;
        std::vector<std::uint32_t> entropyImage;
        std::uint32_t entropyWidth = 0;
        std::size_t groupCount = 1;
        if (level0 && m_in.bits(1) != 0) {
            prefixBits = m_in.bits(3) + 2;
            entropyWidth = subSampleSize(transformXsize, prefixBits);
            Result<std::vector<std::uint32_t>> image =
                imageStream(entropyWidth, subSampleSize(ysize, prefixBits), false);
            if (!image) {
                return image;
            }
            entropyImage = std::move(image).value();
            for (std::uint32_t &p : entropyImage) {
                p = (p >> 8) & 0xFFFFu;
                groupCount = std::max<std::size_t>(groupCount, std::size_t{p} + 1);
            }
        }
        // Only groups the entropy image uses are kept; the others are read
        // and dropped, so memory is bounded by the image, not by the largest
        // index a hostile file names.
        std::vector<std::int32_t> slot(groupCount, -1);
        std::size_t usedGroups = 0;
        if (entropyImage.empty()) {
            slot[0] = 0;
            usedGroups = 1;
        } else {
            for (const std::uint32_t p : entropyImage) {
                if (slot[p] < 0) {
                    slot[p] = static_cast<std::int32_t>(usedGroups++);
                }
            }
        }
        std::vector<Group> groups(usedGroups);
        Group scratch;
        const std::size_t cacheSize = cacheBits != 0 ? std::size_t{1} << cacheBits : 0;
        const std::array<std::size_t, 5> alphabet = {256 + 24 + cacheSize, 256, 256, 256, 40};
        std::size_t tableBytes = 0;
        for (std::size_t g = 0; g < groupCount; ++g) {
            Group &group = slot[g] >= 0 ? groups[static_cast<std::size_t>(slot[g])] : scratch;
            for (std::size_t k = 0; k < 5; ++k) {
                if (Result<void> ok = readCode(alphabet[k], group.codes[k]); !ok) {
                    return std::move(ok).error();
                }
                tableBytes += slot[g] >= 0 ? group.codes[k].memoryBytes() : 0;
            }
            if (tableBytes > m_limits.maxDecodedBytes / 4) {
                return Error(ErrorCode::LimitExceeded, "webp lossless: prefix codes exceed the memory limit");
            }
        }

        Result<std::vector<std::uint32_t>> pixels =
            decodePixels(transformXsize, ysize, groups, slot, entropyImage, entropyWidth, prefixBits, cacheBits);
        if (!pixels) {
            return pixels;
        }
        std::vector<std::uint32_t> image = std::move(pixels).value();
        if (level0) {
            for (std::size_t i = m_transforms.size(); i-- > 0;) {
                image = applyInverse(m_transforms[i], image);
            }
        }
        --m_depth;
        return image;
    }

private:
    Result<void> readCode(std::size_t alphabetSize, PrefixCode &code) {
        std::vector<std::uint8_t> lengths(std::max<std::size_t>(alphabetSize, 256), 0);
        if (m_in.bits(1) != 0) {
            // Simple code: one or two symbols of length 1.
            const unsigned symbols = m_in.bits(1) + 1;
            const unsigned firstBits = m_in.bits(1) != 0 ? 8u : 1u;
            lengths[m_in.bits(firstBits)] = 1;
            if (symbols == 2) {
                lengths[m_in.bits(8)] = 1;
            }
        } else {
            std::array<std::uint8_t, 19> clLengths{};
            const unsigned count = m_in.bits(4) + 4;
            for (unsigned i = 0; i < count; ++i) {
                clLengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(m_in.bits(3));
            }
            PrefixCode clCode;
            if (!clCode.build(clLengths.data(), clLengths.size())) {
                return corrupt("invalid code-length code");
            }
            std::size_t maxSymbol = alphabetSize;
            if (m_in.bits(1) != 0) {
                const unsigned lengthBits = 2 + 2 * m_in.bits(3);
                maxSymbol = 2 + m_in.bits(lengthBits);
                if (maxSymbol > alphabetSize) {
                    return corrupt("max_symbol exceeds the alphabet");
                }
            }
            std::size_t symbol = 0;
            std::uint8_t previous = 8;
            while (symbol < alphabetSize) {
                if (maxSymbol-- == 0) {
                    break;
                }
                const int c = clCode.decode(m_in);
                if (c < 16) {
                    lengths[symbol++] = static_cast<std::uint8_t>(c);
                    if (c != 0) {
                        previous = static_cast<std::uint8_t>(c);
                    }
                    continue;
                }
                const unsigned extra = c == 16 ? 2u : c == 17 ? 3u : 7u;
                const std::size_t repeat = m_in.bits(extra) + (c == 18 ? 11u : 3u);
                if (symbol + repeat > alphabetSize) {
                    return corrupt("code lengths overflow the alphabet");
                }
                std::memset(lengths.data() + symbol, c == 16 ? previous : 0, repeat);
                symbol += repeat;
            }
        }
        if (m_in.overrun()) {
            return corrupt("truncated prefix code");
        }
        // Symbols of a simple code beyond the alphabet are ignored, as libwebp
        // does; an alphabet left empty is then an error.
        if (!code.build(lengths.data(), alphabetSize)) {
            return corrupt("invalid prefix code");
        }
        return success();
    }

    Result<void> readTransform(std::uint32_t &xsize, std::uint32_t ysize) {
        const auto type = static_cast<TransformType>(m_in.bits(2));
        const unsigned bit = 1u << static_cast<unsigned>(type);
        if ((m_seenTransforms & bit) != 0) {
            return corrupt("repeated transform");
        }
        m_seenTransforms |= bit;
        Transform t;
        t.type = type;
        t.xsize = xsize;
        t.ysize = ysize;
        switch (type) {
        case TransformType::Predictor:
        case TransformType::Color: {
            t.bits = m_in.bits(3) + 2;
            Result<std::vector<std::uint32_t>> data =
                imageStream(subSampleSize(xsize, t.bits), subSampleSize(ysize, t.bits), false);
            if (!data) {
                return std::move(data).error();
            }
            t.data = std::move(data).value();
            break;
        }
        case TransformType::ColorIndexing: {
            const std::uint32_t colors = m_in.bits(8) + 1;
            t.bits = colors > 16 ? 0 : colors > 4 ? 1 : colors > 2 ? 2 : 3;
            xsize = subSampleSize(xsize, t.bits);
            Result<std::vector<std::uint32_t>> data = imageStream(colors, 1, false);
            if (!data) {
                return std::move(data).error();
            }
            // Delta-coded palette, expanded to a full table of zeros beyond it.
            std::vector<std::uint32_t> palette(std::size_t{1} << (8 >> t.bits), 0);
            const std::vector<std::uint32_t> &deltas = data.value();
            std::uint32_t previous = 0;
            for (std::size_t i = 0; i < colors && i < palette.size(); ++i) {
                const std::uint32_t d = deltas[i];
                std::uint32_t sum = 0;
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    sum |= (((previous >> shift) + (d >> shift)) & 0xFFu) << shift;
                }
                palette[i] = sum;
                previous = sum;
            }
            if (colors > palette.size()) {
                palette.resize(256, 0); // 17..256 colours: bits is 0, table is 256
            }
            t.data = std::move(palette);
            break;
        }
        case TransformType::SubtractGreen: break;
        }
        m_transforms.push_back(std::move(t));
        return success();
    }

    Result<std::vector<std::uint32_t>> decodePixels(std::uint32_t width, std::uint32_t height,
                                                    const std::vector<Group> &groups,
                                                    const std::vector<std::int32_t> &slot,
                                                    const std::vector<std::uint32_t> &entropyImage,
                                                    std::uint32_t entropyWidth, unsigned prefixBits, unsigned cacheBits) {
        const std::size_t total = std::size_t{width} * height;
        if (total > m_limits.maxDecodedBytes / 4) {
            return Error(ErrorCode::LimitExceeded, "webp lossless: image exceeds the byte limit");
        }
        std::vector<std::uint32_t> out(total);
        std::vector<std::uint32_t> cache(cacheBits != 0 ? std::size_t{1} << cacheBits : 0, 0);
        const unsigned cacheShift = 32 - cacheBits;
        std::size_t lastCached = 0;
        const auto updateCache = [&](std::size_t upTo) {
            if (cacheBits == 0) {
                return;
            }
            for (; lastCached < upTo; ++lastCached) {
                const std::uint32_t c = out[lastCached];
                cache[(0x1e35a7bdu * c) >> cacheShift] = c;
            }
        };
        const auto readValue = [&](int prefix) -> std::size_t {
            if (prefix < 4) {
                return static_cast<std::size_t>(prefix) + 1;
            }
            const auto extra = static_cast<unsigned>((prefix - 2) >> 1);
            const std::size_t offset = static_cast<std::size_t>(2 + (prefix & 1)) << extra;
            return offset + m_in.bits(extra) + 1;
        };
        std::size_t pos = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        const Group *group = &groups[0];
        const auto groupAt = [&]() -> const Group * {
            if (entropyImage.empty()) {
                return &groups[0];
            }
            const std::uint32_t meta = entropyImage[std::size_t{y >> prefixBits} * entropyWidth + (x >> prefixBits)];
            return &groups[static_cast<std::size_t>(slot[meta])];
        };
        const std::uint32_t groupMask = entropyImage.empty() ? 0xFFFFFFFFu : (1u << prefixBits) - 1u;
        while (pos < total) {
            if ((x & groupMask) == 0) {
                group = groupAt();
            }
            const int s = group->codes[0].decode(m_in);
            if (s < 256) {
                const auto r = static_cast<std::uint32_t>(group->codes[1].decode(m_in));
                const auto b = static_cast<std::uint32_t>(group->codes[2].decode(m_in));
                const auto a = static_cast<std::uint32_t>(group->codes[3].decode(m_in));
                out[pos++] = a << 24 | r << 16 | static_cast<std::uint32_t>(s) << 8 | b;
                if (++x == width) {
                    x = 0;
                    ++y;
                }
            } else if (s < 256 + 24) {
                const std::size_t length = readValue(s - 256);
                const std::size_t code = readValue(group->codes[4].decode(m_in));
                std::size_t distance = 0;
                if (code > 120) {
                    distance = code - 120;
                } else {
                    const auto &d = kDistanceMap[code - 1];
                    const std::int64_t dist = std::int64_t{d[1]} * width + d[0];
                    distance = dist >= 1 ? static_cast<std::size_t>(dist) : 1;
                }
                if (m_in.overrun()) {
                    return corrupt("truncated image data");
                }
                if (distance > pos || length > total - pos) {
                    return corrupt("backward reference out of range");
                }
                for (std::size_t i = 0; i < length; ++i) {
                    out[pos + i] = out[pos + i - distance];
                }
                pos += length;
                x += static_cast<std::uint32_t>(length % width);
                y += static_cast<std::uint32_t>(length / width);
                if (x >= width) {
                    x -= width;
                    ++y;
                }
                if (pos < total) {
                    group = groupAt();
                }
            } else {
                const auto index = static_cast<std::size_t>(s - 256 - 24);
                if (index >= cache.size()) {
                    return corrupt("colour cache index out of range");
                }
                updateCache(pos);
                out[pos++] = cache[index];
                if (++x == width) {
                    x = 0;
                    ++y;
                }
            }
            if (m_in.overrun()) {
                return corrupt("truncated image data");
            }
            updateCache(pos);
        }
        return out;
    }

    static std::uint32_t average2(std::uint32_t a, std::uint32_t b) noexcept {
        return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b);
    }
    static std::uint32_t channel(std::uint32_t p, unsigned shift) noexcept { return (p >> shift) & 0xFFu; }
    static std::uint32_t clampedAddSubtractFull(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
        std::uint32_t out = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            const int v = static_cast<int>(channel(a, shift) + channel(b, shift)) - static_cast<int>(channel(c, shift));
            out |= static_cast<std::uint32_t>(std::clamp(v, 0, 255)) << shift;
        }
        return out;
    }
    static std::uint32_t clampedAddSubtractHalf(std::uint32_t a, std::uint32_t b) noexcept {
        std::uint32_t out = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            const int ca = static_cast<int>(channel(a, shift));
            const int cb = static_cast<int>(channel(b, shift));
            out |= static_cast<std::uint32_t>(std::clamp(ca + (ca - cb) / 2, 0, 255)) << shift;
        }
        return out;
    }
    static std::uint32_t select(std::uint32_t l, std::uint32_t t, std::uint32_t tl) noexcept {
        int pL = 0;
        int pT = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            pL += std::abs(static_cast<int>(channel(t, shift)) - static_cast<int>(channel(tl, shift)));
            pT += std::abs(static_cast<int>(channel(l, shift)) - static_cast<int>(channel(tl, shift)));
        }
        return pL < pT ? l : t;
    }
    static std::uint32_t addPixels(std::uint32_t a, std::uint32_t b) noexcept {
        const std::uint32_t ag = (a & 0xFF00FF00u) + (b & 0xFF00FF00u);
        const std::uint32_t rb = (a & 0x00FF00FFu) + (b & 0x00FF00FFu);
        return (ag & 0xFF00FF00u) | (rb & 0x00FF00FFu);
    }

    static std::uint32_t predict(unsigned mode, const std::uint32_t *out, std::size_t pos, std::uint32_t width) noexcept {
        const std::uint32_t l = out[pos - 1];
        const std::uint32_t t = out[pos - width];
        const std::uint32_t tl = out[pos - width - 1];
        const std::uint32_t tr = out[pos - width + 1]; // the row's first pixel for the last column
        switch (mode) {
        case 1: return l;
        case 2: return t;
        case 3: return tr;
        case 4: return tl;
        case 5: return average2(average2(l, tr), t);
        case 6: return average2(l, tl);
        case 7: return average2(l, t);
        case 8: return average2(tl, t);
        case 9: return average2(t, tr);
        case 10: return average2(average2(l, tl), average2(t, tr));
        case 11: return select(l, t, tl);
        case 12: return clampedAddSubtractFull(l, t, tl);
        case 13: return clampedAddSubtractHalf(average2(l, t), tl);
        default: return 0xFF000000u; // 0, and 14-15 as libwebp treats them
        }
    }

    static std::int32_t colorDelta(std::uint32_t t, std::uint32_t c) noexcept {
        return (static_cast<std::int32_t>(static_cast<std::int8_t>(t)) * static_cast<std::int8_t>(c)) >> 5;
    }

    static std::vector<std::uint32_t> applyInverse(const Transform &t, std::vector<std::uint32_t> &in) {
        const std::uint32_t width = t.xsize;
        const std::uint32_t height = t.ysize;
        switch (t.type) {
        case TransformType::SubtractGreen:
            for (std::uint32_t &p : in) {
                const std::uint32_t g = (p >> 8) & 0xFFu;
                const std::uint32_t rb = ((p & 0x00FF00FFu) + (g << 16 | g)) & 0x00FF00FFu;
                p = (p & 0xFF00FF00u) | rb;
            }
            return std::move(in);
        case TransformType::Color: {
            const std::uint32_t blocksW = subSampleSize(width, t.bits);
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint32_t e = t.data[std::size_t{y >> t.bits} * blocksW + (x >> t.bits)];
                    std::uint32_t &p = in[std::size_t{y} * width + x];
                    const std::uint32_t green = (p >> 8) & 0xFFu;
                    std::int32_t red = static_cast<std::int32_t>((p >> 16) & 0xFFu);
                    std::int32_t blue = static_cast<std::int32_t>(p & 0xFFu);
                    red += colorDelta(e & 0xFFu, green);                // green_to_red
                    blue += colorDelta((e >> 8) & 0xFFu, green);        // green_to_blue
                    blue += colorDelta((e >> 16) & 0xFFu, static_cast<std::uint32_t>(red) & 0xFFu); // red_to_blue
                    p = (p & 0xFF00FF00u) | (static_cast<std::uint32_t>(red) & 0xFFu) << 16 |
                        (static_cast<std::uint32_t>(blue) & 0xFFu);
                }
            }
            return std::move(in);
        }
        case TransformType::Predictor: {
            const std::uint32_t blocksW = subSampleSize(width, t.bits);
            std::uint32_t *out = in.data();
            // Top-left: black; the rest of the top row: L; the left column: T.
            out[0] = addPixels(out[0], 0xFF000000u);
            for (std::uint32_t x = 1; x < width; ++x) {
                out[x] = addPixels(out[x], out[x - 1]);
            }
            for (std::uint32_t y = 1; y < height; ++y) {
                const std::size_t row = std::size_t{y} * width;
                out[row] = addPixels(out[row], out[row - width]);
                for (std::uint32_t x = 1; x < width; ++x) {
                    const unsigned mode = (t.data[std::size_t{y >> t.bits} * blocksW + (x >> t.bits)] >> 8) & 0xFu;
                    out[row + x] = addPixels(out[row + x], predict(mode, out, row + x, width));
                }
            }
            return std::move(in);
        }
        case TransformType::ColorIndexing: {
            std::vector<std::uint32_t> out(std::size_t{width} * height);
            const std::uint32_t packedWidth = subSampleSize(width, t.bits);
            const unsigned perByte = 1u << t.bits;
            const unsigned bitsPer = 8u >> t.bits;
            const std::uint32_t mask = (1u << bitsPer) - 1u;
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint32_t packed = in[std::size_t{y} * packedWidth + x / perByte];
                    const std::uint32_t index = ((packed >> 8) >> ((x % perByte) * bitsPer)) & mask;
                    out[std::size_t{y} * width + x] = index < t.data.size() ? t.data[index] : 0u;
                }
            }
            return out;
        }
        }
        return std::move(in);
    }

    BitReader m_in;
    const ImageLimits &m_limits;
    std::vector<Transform> m_transforms;
    unsigned m_seenTransforms = 0;
    int m_depth = 0;
};

Result<ArgbImage> finishStream(Decoder &decoder, std::uint32_t width, std::uint32_t height) {
    Result<std::vector<std::uint32_t>> pixels = decoder.imageStream(width, height, true);
    if (!pixels) {
        return std::move(pixels).error();
    }
    ArgbImage image;
    image.width = width;
    image.height = height;
    image.pixels = std::move(pixels).value();
    return image;
}

} // namespace

Result<ArgbImage> decodeLossless(Span<const std::byte> data, const ImageLimits &limits) {
    try {
        if (data.size() < 5 || data[0] != std::byte{0x2F}) {
            return corrupt("missing VP8L signature");
        }
        Decoder decoder(data.subspan(1), limits);
        BitReader &in = decoder.in();
        const std::uint32_t width = in.bits(14) + 1;
        const std::uint32_t height = in.bits(14) + 1;
        (void)in.bits(1); // alpha_is_used: a hint only
        if (in.bits(3) != 0) {
            return corrupt("unknown version");
        }
        if (Result<void> ok = checkImageSize(width, height, limits); !ok) {
            return std::move(ok).error();
        }
        return finishStream(decoder, width, height);
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "webp lossless: out of memory");
    }
}

Result<ArgbImage> decodeLosslessStream(Span<const std::byte> data, std::uint32_t width, std::uint32_t height,
                                       const ImageLimits &limits) {
    try {
        Decoder decoder(data, limits);
        return finishStream(decoder, width, height);
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "webp lossless: out of memory");
    }
}

} // namespace cfw::webp
