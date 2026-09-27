// JPEG decoder. The IDCT, upsampling and colour conversion reproduce
// libjpeg-turbo's defaults (jidctint.c, jdsample.c, jdcolor.c) arithmetic for
// arithmetic, so the pixels match it exactly; see Jpeg.h.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "JpegTables.h"
#include "cfw/image/Jpeg.h"

namespace cfw {

namespace {

using jpeg::kZigzag;

Error corrupt(const char *what) { return Error(ErrorCode::Corrupt, String("jpeg: ") + what); }
Error unsupported(const char *what) { return Error(ErrorCode::Unsupported, String("jpeg: ") + what); }

// --- Huffman tables ---------------------------------------------------------

struct HuffmanTable {
    static constexpr unsigned kFastBits = 9;
    bool defined = false;
    std::array<std::uint8_t, 256> values{};
    // For code lengths 1..16: the largest code of that length (-1 if none),
    // and the offset from a code to its index in `values`.
    std::array<std::int32_t, 18> maxCode{};
    std::array<std::int32_t, 17> valueOffset{};
    // Fast path: (length << 8 | value), or 0 when the code is longer.
    std::array<std::uint16_t, 1u << kFastBits> fast{};
    // AC fast path (as in stb_image): when a code and its magnitude bits fit
    // in kFastBits, one lookup gives the run, the extended value and the
    // total length. Length 0 means "use the slow path".
    struct FastAc {
        std::int16_t value;
        std::uint8_t run;
        std::uint8_t length;
    };
    std::array<FastAc, 1u << kFastBits> fastAc{};

    // `counts[i]` codes of length i + 1, then the values. Fails on a table
    // whose codes overflow their length (libjpeg's "bogus Huffman table").
    bool build(const std::uint8_t *counts, const std::uint8_t *symbols, std::size_t symbolCount) {
        std::copy(symbols, symbols + symbolCount, values.begin());
        fast.fill(0);
        std::int32_t code = 0;
        std::size_t k = 0;
        for (unsigned len = 1; len <= 16; ++len) {
            const unsigned n = counts[len - 1];
            valueOffset[len] = static_cast<std::int32_t>(k) - code;
            for (unsigned i = 0; i < n; ++i, ++k, ++code) {
                // A code that does not fit its length is a bogus table; check
                // before it indexes the fast table (found by FuzzJpeg).
                if (code >= (1 << len)) {
                    return false;
                }
                if (len <= kFastBits) {
                    const unsigned shift = kFastBits - len;
                    const auto first = static_cast<unsigned>(code) << shift;
                    for (unsigned fill = 0; fill < (1u << shift); ++fill) {
                        fast[first + fill] = static_cast<std::uint16_t>(len << 8 | values[k]);
                    }
                }
            }
            maxCode[len] = n != 0 ? code - 1 : -1;
            code <<= 1;
        }
        maxCode[17] = 0x7FFFFFFF; // sentinel
        for (std::uint32_t look = 0; look < fastAc.size(); ++look) {
            fastAc[look] = {0, 0, 0};
            const std::uint16_t entry = fast[look];
            if (entry == 0) {
                continue;
            }
            const unsigned len = entry >> 8;
            const unsigned rs = entry & 0xFFu;
            const unsigned size = rs & 15u;
            if (size == 0 || len + size > kFastBits) {
                continue;
            }
            const auto bits = static_cast<int>((look >> (kFastBits - len - size)) & ((1u << size) - 1u));
            const int value = bits < (1 << (size - 1)) ? bits - (1 << size) + 1 : bits;
            fastAc[look] = {static_cast<std::int16_t>(value), static_cast<std::uint8_t>(rs >> 4),
                            static_cast<std::uint8_t>(len + size)};
        }
        defined = true;
        return true;
    }
};

// --- Entropy-coded data -----------------------------------------------------

// MSB-first reader over entropy-coded bytes: unstuffs FF 00, stops at a
// marker, and feeds zero bits past it while recording that it did, so a
// truncated scan is detected rather than read out of bounds.
class EntropyReader {
public:
    EntropyReader(const std::uint8_t *data, std::size_t size, std::size_t pos) noexcept
        : m_data(data), m_size(size), m_pos(pos) {}

    [[nodiscard]] std::uint32_t peek16() {
        fill();
        return static_cast<std::uint32_t>(m_bits >> 48);
    }
    void consume(unsigned n) noexcept {
        m_bits <<= n;
        m_count -= n;
        if (m_count < m_fake) {
            m_fake = m_count;
            m_overrun = true;
        }
    }
    [[nodiscard]] std::uint32_t bits(unsigned n) {
        if (n == 0) {
            return 0;
        }
        fill();
        const auto v = static_cast<std::uint32_t>(m_bits >> (64 - n));
        consume(n);
        return v;
    }
    [[nodiscard]] int bit() { return static_cast<int>(bits(1)); }

    // RECEIVE and EXTEND (T.81 F.2.2.1).
    [[nodiscard]] int receiveExtend(unsigned s) {
        if (s == 0) {
            return 0;
        }
        const auto v = static_cast<int>(bits(s));
        return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
    }

    // The AC fast-path entry for the next bits (length 0 if none).
    [[nodiscard]] HuffmanTable::FastAc peekFastAc(const HuffmanTable &t) {
        return t.fastAc[peek16() >> (16 - HuffmanTable::kFastBits)];
    }

    [[nodiscard]] int decode(const HuffmanTable &t) {
        const std::uint32_t look = peek16();
        const std::uint16_t entry = t.fast[look >> (16 - HuffmanTable::kFastBits)];
        if (entry != 0) {
            consume(entry >> 8);
            return entry & 0xFF;
        }
        for (unsigned len = HuffmanTable::kFastBits + 1; len <= 16; ++len) {
            const auto code = static_cast<std::int32_t>(look >> (16 - len));
            if (code <= t.maxCode[len]) {
                consume(len);
                return t.values[static_cast<std::size_t>(code + t.valueOffset[len]) & 0xFFu];
            }
        }
        // No such code: libjpeg warns and uses 0; so do we (it cannot crash).
        consume(16);
        return 0;
    }

    // Restart: drop buffered bits and step over the next RSTn marker.
    void restart() {
        m_bits = 0;
        m_count = 0;
        m_fake = 0;
        m_atMarker = false;
        while (m_pos + 1 < m_size) {
            if (m_data[m_pos] == 0xFF && m_data[m_pos + 1] >= 0xD0 && m_data[m_pos + 1] <= 0xD7) {
                m_pos += 2;
                return;
            }
            if (m_data[m_pos] == 0xFF && m_data[m_pos + 1] != 0x00 && m_data[m_pos + 1] != 0xFF) {
                return; // some other marker: leave it for the segment parser
            }
            ++m_pos;
        }
    }

    // The position of the marker that ends this scan.
    [[nodiscard]] std::size_t endOfScan() const noexcept {
        std::size_t p = m_pos;
        while (p + 1 < m_size) {
            if (m_data[p] == 0xFF && m_data[p + 1] != 0x00 && m_data[p + 1] != 0xFF &&
                !(m_data[p + 1] >= 0xD0 && m_data[p + 1] <= 0xD7)) {
                return p;
            }
            ++p;
        }
        return m_size;
    }

    [[nodiscard]] bool overrun() const noexcept { return m_overrun; }

private:
    void fill() {
        while (m_count <= 56) {
            std::uint64_t byte = 0;
            if (!m_atMarker && m_pos < m_size) {
                const std::uint8_t b = m_data[m_pos];
                if (b == 0xFF) {
                    // Fill bytes (FF FF ...) may precede a marker.
                    std::size_t q = m_pos + 1;
                    while (q < m_size && m_data[q] == 0xFF) {
                        ++q;
                    }
                    if (q < m_size && m_data[q] == 0x00) {
                        byte = 0xFF;
                        m_pos = q + 1;
                    } else {
                        m_atMarker = true;
                    }
                } else {
                    byte = b;
                    ++m_pos;
                }
            } else {
                m_atMarker = true;
            }
            if (m_atMarker) {
                m_fake += 8;
            }
            m_bits |= byte << (56 - m_count);
            m_count += 8;
        }
    }

    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_pos;
    std::uint64_t m_bits = 0;
    unsigned m_count = 0;
    unsigned m_fake = 0; // zero bits fed past the end, at the tail of m_bits
    bool m_atMarker = false;
    bool m_overrun = false;
};

// --- Frame state ------------------------------------------------------------

struct Component {
    int id = 0;
    int h = 1;
    int v = 1;
    int quantTable = 0;
    int dcTable = 0;
    int acTable = 0;
    // Blocks allocated (padded to whole MCUs) and blocks that hold image data.
    std::size_t blocksW = 0;
    std::size_t blocksH = 0;
    std::size_t usedBlocksW = 0;
    std::size_t usedBlocksH = 0;
    // Samples of this component that belong to the image (libjpeg's
    // downsampled_width/height).
    std::size_t width = 0;
    std::size_t height = 0;
    // Progressive files keep every coefficient until the last scan; baseline
    // blocks go through the IDCT as they are decoded.
    std::vector<std::int16_t> coefficients;
    std::vector<std::uint8_t> samples; // blocksW * 8 wide, blocksH * 8 high
    int dcPredictor = 0;

    [[nodiscard]] std::int16_t *block(std::size_t bx, std::size_t by) noexcept {
        return coefficients.data() + (by * blocksW + bx) * 64;
    }
};

void idct(const std::int16_t *in, const std::uint16_t *q, std::uint8_t *out, std::size_t stride) noexcept;

enum class ColorSpace : std::uint8_t { Gray, YCbCr, Rgb, Cmyk, Ycck };

struct Decoder {
    const std::uint8_t *data;
    std::size_t size;
    const ImageLimits &limits;

    std::array<std::array<std::uint16_t, 64>, 4> quant{};
    std::array<bool, 4> quantDefined{};
    std::array<HuffmanTable, 4> dcTables;
    std::array<HuffmanTable, 4> acTables;
    std::vector<Component> components;
    bool haveFrame = false;
    bool progressive = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int hMax = 1;
    int vMax = 1;
    std::size_t mcusX = 0;
    std::size_t mcusY = 0;
    unsigned restartInterval = 0;
    int scans = 0;
    bool sawJfif = false;
    bool sawAdobe = false;
    int adobeTransform = -1;
    int eobRun = 0;
    bool sawEnd = false;

    Result<Image> run();
    Result<void> readFrame(const std::uint8_t *p, std::size_t length, unsigned marker);
    Result<void> readHuffman(const std::uint8_t *p, std::size_t length);
    Result<void> readQuant(const std::uint8_t *p, std::size_t length);
    Result<std::size_t> readScan(const std::uint8_t *p, std::size_t length, std::size_t dataStart);
    Result<Image> finish();

    void decodeBaselineBlock(EntropyReader &in, Component &c, std::int16_t *block);
    void decodeDcFirst(EntropyReader &in, Component &c, std::int16_t *block, int al);
    static void decodeDcRefine(EntropyReader &in, std::int16_t *block, int al);
    void decodeAcFirst(EntropyReader &in, const Component &c, std::int16_t *block, int ss, int se, int al);
    void decodeAcRefine(EntropyReader &in, const Component &c, std::int16_t *block, int ss, int se, int al);
};

std::uint16_t be16(const std::uint8_t *p) noexcept { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

Result<void> Decoder::readQuant(const std::uint8_t *p, std::size_t length) {
    std::size_t i = 0;
    while (i < length) {
        const unsigned precision = p[i] >> 4;
        const unsigned id = p[i] & 15u;
        ++i;
        if (id > 3 || precision > 1) {
            return corrupt("invalid quantisation table");
        }
        const std::size_t bytes = precision == 0 ? 64 : 128;
        if (length - i < bytes) {
            return corrupt("truncated quantisation table");
        }
        for (std::size_t k = 0; k < 64; ++k) {
            quant[id][kZigzag[k]] = precision == 0 ? p[i + k] : be16(p + i + 2 * k);
        }
        quantDefined[id] = true;
        i += bytes;
    }
    return success();
}

Result<void> Decoder::readHuffman(const std::uint8_t *p, std::size_t length) {
    std::size_t i = 0;
    while (i < length) {
        if (length - i < 17) {
            return corrupt("truncated Huffman table");
        }
        const unsigned tableClass = p[i] >> 4;
        const unsigned id = p[i] & 15u;
        if (tableClass > 1 || id > 3) {
            return corrupt("invalid Huffman table");
        }
        const std::uint8_t *counts = p + i + 1;
        std::size_t total = 0;
        for (int k = 0; k < 16; ++k) {
            total += counts[k];
        }
        i += 17;
        if (total > 256 || length - i < total) {
            return corrupt("invalid Huffman table");
        }
        HuffmanTable &table = tableClass == 0 ? dcTables[id] : acTables[id];
        if (!table.build(counts, p + i, total)) {
            return corrupt("invalid Huffman table");
        }
        i += total;
    }
    return success();
}

Result<void> Decoder::readFrame(const std::uint8_t *p, std::size_t length, unsigned marker) {
    if (haveFrame) {
        return corrupt("more than one frame");
    }
    if (marker != 0xC0 && marker != 0xC1 && marker != 0xC2) {
        return unsupported("only Huffman-coded baseline, extended and progressive JPEGs are supported");
    }
    if (length < 6) {
        return corrupt("truncated frame header");
    }
    if (p[0] != 8) {
        return unsupported("only 8-bit precision is supported");
    }
    height = be16(p + 1);
    width = be16(p + 3);
    const unsigned count = p[5];
    if (height == 0) {
        return unsupported("height defined by a DNL marker");
    }
    if (count != 1 && count != 3 && count != 4) {
        return unsupported("component count must be 1, 3 or 4");
    }
    if (length < 6 + 3 * std::size_t{count}) {
        return corrupt("truncated frame header");
    }
    if (Result<void> ok = checkImageSize(width, height, limits); !ok) {
        return ok;
    }
    progressive = marker == 0xC2;
    components.resize(count);
    int blocksPerMcu = 0;
    for (unsigned i = 0; i < count; ++i) {
        Component &c = components[i];
        c.id = p[6 + 3 * i];
        c.h = p[7 + 3 * i] >> 4;
        c.v = p[7 + 3 * i] & 15;
        c.quantTable = p[8 + 3 * i];
        if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.quantTable > 3) {
            return corrupt("invalid component sampling or table");
        }
        for (unsigned j = 0; j < i; ++j) {
            if (components[j].id == c.id) {
                return corrupt("duplicate component id");
            }
        }
        hMax = std::max(hMax, c.h);
        vMax = std::max(vMax, c.v);
        blocksPerMcu += c.h * c.v;
    }
    if (count > 1 && blocksPerMcu > 10) {
        return corrupt("too many blocks per MCU");
    }
    const auto uh = static_cast<std::size_t>(hMax);
    const auto uv = static_cast<std::size_t>(vMax);
    mcusX = (width + 8 * uh - 1) / (8 * uh);
    mcusY = (height + 8 * uv - 1) / (8 * uv);
    std::size_t coefficientBytes = 0;
    for (Component &c : components) {
        if (hMax % c.h != 0 || vMax % c.v != 0) {
            return unsupported("fractional chroma subsampling");
        }
        c.width = (std::size_t{width} * static_cast<std::size_t>(c.h) + uh - 1) / uh;
        c.height = (std::size_t{height} * static_cast<std::size_t>(c.v) + uv - 1) / uv;
        c.usedBlocksW = (c.width + 7) / 8;
        c.usedBlocksH = (c.height + 7) / 8;
        c.blocksW = mcusX * static_cast<std::size_t>(c.h);
        c.blocksH = mcusY * static_cast<std::size_t>(c.v);
        coefficientBytes += c.blocksW * c.blocksH * 64 * 2;
    }
    if (coefficientBytes / 2 > limits.maxDecodedBytes) {
        return Error(ErrorCode::LimitExceeded, "jpeg: coefficient storage would exceed the limit");
    }
    for (Component &c : components) {
        if (progressive) {
            c.coefficients.assign(c.blocksW * c.blocksH * 64, 0);
        }
        c.samples.assign(c.blocksW * 8 * c.blocksH * 8, 0);
    }
    haveFrame = true;
    return success();
}

void Decoder::decodeBaselineBlock(EntropyReader &in, Component &c, std::int16_t *block) {
    const int s = in.decode(dcTables[static_cast<std::size_t>(c.dcTable)]);
    // Kept to 16 bits so a hostile file cannot overflow it.
    c.dcPredictor = static_cast<std::int16_t>(c.dcPredictor + in.receiveExtend(static_cast<unsigned>(std::min(s, 16))));
    block[0] = static_cast<std::int16_t>(c.dcPredictor);
    const HuffmanTable &ac = acTables[static_cast<std::size_t>(c.acTable)];
    for (int k = 1; k < 64; ++k) {
        const HuffmanTable::FastAc fast = in.peekFastAc(ac);
        if (fast.length != 0) {
            in.consume(fast.length);
            k += fast.run;
            // Past 63 the padded zigzag table lands on 63, as libjpeg's does.
            block[kZigzag[static_cast<std::size_t>(k)]] = fast.value;
            continue;
        }
        const int rs = in.decode(ac);
        const int r = rs >> 4;
        const int sz = rs & 15;
        if (sz != 0) {
            k += r;
            block[kZigzag[static_cast<std::size_t>(k)]] = static_cast<std::int16_t>(in.receiveExtend(static_cast<unsigned>(sz)));
        } else {
            if (r != 15) {
                break;
            }
            k += 15;
        }
    }
}

void Decoder::decodeDcFirst(EntropyReader &in, Component &c, std::int16_t *block, int al) {
    const int s = in.decode(dcTables[static_cast<std::size_t>(c.dcTable)]);
    c.dcPredictor = static_cast<std::int16_t>(c.dcPredictor + in.receiveExtend(static_cast<unsigned>(std::min(s, 16))));
    block[0] = static_cast<std::int16_t>(static_cast<unsigned>(c.dcPredictor) << al);
}

void Decoder::decodeDcRefine(EntropyReader &in, std::int16_t *block, int al) {
    if (in.bit() != 0) {
        block[0] = static_cast<std::int16_t>(block[0] | (1 << al));
    }
}

void Decoder::decodeAcFirst(EntropyReader &in, const Component &c, std::int16_t *block, int ss, int se, int al) {
    if (eobRun > 0) {
        --eobRun;
        return;
    }
    const HuffmanTable &ac = acTables[static_cast<std::size_t>(c.acTable)];
    for (int k = ss; k <= se; ++k) {
        const int rs = in.decode(ac);
        const int r = rs >> 4;
        const int s = rs & 15;
        if (s != 0) {
            k += r;
            const int value = in.receiveExtend(static_cast<unsigned>(s));
            block[kZigzag[static_cast<std::size_t>(k)]] = static_cast<std::int16_t>(static_cast<unsigned>(value) << al);
        } else if (r < 15) {
            eobRun = (1 << r) - 1;
            if (r != 0) {
                eobRun += static_cast<int>(in.bits(static_cast<unsigned>(r)));
            }
            break;
        } else {
            k += 15;
        }
    }
}

void Decoder::decodeAcRefine(EntropyReader &in, const Component &c, std::int16_t *block, int ss, int se, int al) {
    const int p1 = 1 << al;
    const int m1 = -p1;
    const auto refine = [&](std::int16_t &coef) {
        if (in.bit() != 0 && (coef & p1) == 0) {
            coef = static_cast<std::int16_t>(coef >= 0 ? coef + p1 : coef + m1);
        }
    };
    int k = ss;
    if (eobRun == 0) {
        const HuffmanTable &ac = acTables[static_cast<std::size_t>(c.acTable)];
        for (; k <= se; ++k) {
            const int rs = in.decode(ac);
            int r = rs >> 4;
            int s = rs & 15;
            if (s != 0) {
                s = in.bit() != 0 ? p1 : m1; // s should be 1; libjpeg only warns otherwise
            } else if (r != 15) {
                eobRun = 1 << r;
                if (r != 0) {
                    eobRun += static_cast<int>(in.bits(static_cast<unsigned>(r)));
                }
                break;
            }
            // Advance over already-nonzero coefficients (refining them) and
            // r still-zero ones.
            while (k <= se) {
                std::int16_t &coef = block[kZigzag[static_cast<std::size_t>(k)]];
                if (coef != 0) {
                    refine(coef);
                } else {
                    if (--r < 0) {
                        break;
                    }
                }
                ++k;
            }
            if (s != 0) {
                block[kZigzag[static_cast<std::size_t>(k)]] = static_cast<std::int16_t>(s);
            }
        }
    }
    if (eobRun > 0) {
        for (; k <= se; ++k) {
            std::int16_t &coef = block[kZigzag[static_cast<std::size_t>(k)]];
            if (coef != 0) {
                refine(coef);
            }
        }
        --eobRun;
    }
}

Result<std::size_t> Decoder::readScan(const std::uint8_t *p, std::size_t length, std::size_t dataStart) {
    if (!haveFrame) {
        return corrupt("scan before frame header");
    }
    if (++scans > kMaxJpegScans) {
        return Error(ErrorCode::LimitExceeded, "jpeg: too many scans");
    }
    if (length < 1) {
        return corrupt("truncated scan header");
    }
    const unsigned count = p[0];
    if (count < 1 || count > 4 || length < 4 + 2 * std::size_t{count}) {
        return corrupt("invalid scan header");
    }
    std::array<Component *, 4> scan{};
    for (unsigned i = 0; i < count; ++i) {
        const int id = p[1 + 2 * i];
        Component *found = nullptr;
        for (Component &c : components) {
            if (c.id == id) {
                found = &c;
            }
        }
        for (unsigned j = 0; j < i; ++j) {
            if (scan[j] == found) {
                found = nullptr;
            }
        }
        if (found == nullptr) {
            return corrupt("scan names an unknown or repeated component");
        }
        found->dcTable = p[2 + 2 * i] >> 4;
        found->acTable = p[2 + 2 * i] & 15;
        if (found->dcTable > 3 || found->acTable > 3) {
            return corrupt("invalid scan table");
        }
        scan[i] = found;
    }
    const std::uint8_t *params = p + 1 + 2 * count;
    const int ss = params[0];
    const int se = params[1];
    const int ah = params[2] >> 4;
    const int al = params[2] & 15;
    if (progressive) {
        if (ss > se || se > 63 || al > 13 || ah > 13 || (ss == 0 && se != 0) || (ss != 0 && count != 1)) {
            return corrupt("invalid progressive scan parameters");
        }
    }
    // Sequential scans ignore Ss, Se, Ah and Al, as libjpeg does.
    const bool dcScan = !progressive || ss == 0;
    const bool acScan = !progressive || ss != 0;
    // Motion-JPEG frames omit their Huffman tables and rely on the standard
    // ones; libjpeg-turbo loads them for tables 0 and 1, and so do we.
    const auto loadStandard = [](HuffmanTable &t, const jpeg::StandardHuffman &standard) {
        if (!t.defined) {
            (void)t.build(standard.counts.data(), standard.values.data(), standard.valueCount);
        }
    };
    for (unsigned i = 0; i < count; ++i) {
        if (scan[i]->dcTable < 2) {
            loadStandard(dcTables[static_cast<std::size_t>(scan[i]->dcTable)],
                         scan[i]->dcTable == 0 ? jpeg::kDcLuminance : jpeg::kDcChrominance);
        }
        if (scan[i]->acTable < 2) {
            loadStandard(acTables[static_cast<std::size_t>(scan[i]->acTable)],
                         scan[i]->acTable == 0 ? jpeg::kAcLuminance : jpeg::kAcChrominance);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if ((dcScan && !dcTables[static_cast<std::size_t>(scan[i]->dcTable)].defined && !(progressive && ah != 0)) ||
            (acScan && !acTables[static_cast<std::size_t>(scan[i]->acTable)].defined)) {
            return corrupt("scan uses an undefined Huffman table");
        }
        scan[i]->dcPredictor = 0;
    }
    eobRun = 0;

    if (!progressive) {
        for (unsigned i = 0; i < count; ++i) {
            if (!quantDefined[static_cast<std::size_t>(scan[i]->quantTable)]) {
                return corrupt("undefined quantisation table");
            }
        }
    }
    EntropyReader in(data, size, dataStart);
    std::array<std::int16_t, 64> scratch{};
    const auto decodeBlock = [&](Component &c, std::size_t bx, std::size_t by) {
        if (!progressive) {
            scratch.fill(0);
            decodeBaselineBlock(in, c, scratch.data());
            if (bx < c.usedBlocksW && by < c.usedBlocksH) {
                const std::size_t stride = c.blocksW * 8;
                idct(scratch.data(), quant[static_cast<std::size_t>(c.quantTable)].data(),
                     c.samples.data() + by * 8 * stride + bx * 8, stride);
            }
            return;
        }
        std::int16_t *block = c.block(bx, by);
        if (ss == 0) {
            if (ah == 0) {
                decodeDcFirst(in, c, block, al);
            } else {
                decodeDcRefine(in, block, al);
            }
        } else if (ah == 0) {
            decodeAcFirst(in, c, block, ss, se, al);
        } else {
            decodeAcRefine(in, c, block, ss, se, al);
        }
    };
    const auto restartIfDue = [&](std::size_t mcu) {
        if (restartInterval != 0 && mcu != 0 && mcu % restartInterval == 0) {
            in.restart();
            for (unsigned i = 0; i < count; ++i) {
                scan[i]->dcPredictor = 0;
            }
            eobRun = 0;
        }
    };

    if (count == 1) {
        // Non-interleaved: one block per MCU, only the blocks with image data.
        Component &c = *scan[0];
        std::size_t mcu = 0;
        for (std::size_t by = 0; by < c.usedBlocksH; ++by) {
            for (std::size_t bx = 0; bx < c.usedBlocksW; ++bx, ++mcu) {
                restartIfDue(mcu);
                decodeBlock(c, bx, by);
            }
            if (in.overrun()) {
                break;
            }
        }
    } else {
        std::size_t mcu = 0;
        for (std::size_t my = 0; my < mcusY; ++my) {
            for (std::size_t mx = 0; mx < mcusX; ++mx, ++mcu) {
                restartIfDue(mcu);
                for (unsigned i = 0; i < count; ++i) {
                    Component &c = *scan[i];
                    const auto ch = static_cast<std::size_t>(c.h);
                    const auto cv = static_cast<std::size_t>(c.v);
                    for (std::size_t v = 0; v < cv; ++v) {
                        for (std::size_t h = 0; h < ch; ++h) {
                            decodeBlock(c, mx * ch + h, my * cv + v);
                        }
                    }
                }
            }
            if (in.overrun()) {
                break;
            }
        }
    }
    if (in.overrun()) {
        return corrupt("scan data is truncated");
    }
    return in.endOfScan();
}

// --- IDCT (jidctint.c, "islow") ---------------------------------------------

constexpr std::int64_t kFix_0_298631336 = 2446;
constexpr std::int64_t kFix_0_390180644 = 3196;
constexpr std::int64_t kFix_0_541196100 = 4433;
constexpr std::int64_t kFix_0_765366865 = 6270;
constexpr std::int64_t kFix_0_899976223 = 7373;
constexpr std::int64_t kFix_1_175875602 = 9633;
constexpr std::int64_t kFix_1_501321110 = 12299;
constexpr std::int64_t kFix_1_847759065 = 15137;
constexpr std::int64_t kFix_1_961570560 = 16069;
constexpr std::int64_t kFix_2_053119869 = 16819;
constexpr std::int64_t kFix_2_562915447 = 20995;
constexpr std::int64_t kFix_3_072711026 = 25172;
constexpr int kConstBits = 13;
constexpr int kPass1Bits = 2;

constexpr std::int64_t descale(std::int64_t x, int n) noexcept { return (x + (std::int64_t{1} << (n - 1))) >> n; }

// libjpeg's post-IDCT range limit: the value is taken mod 1024, then
// [0,127] -> 128..255, [128,511] -> 255, [512,895] -> 0, [896,1023] -> 0..127.
struct RangeLimitTable {
    std::array<std::uint8_t, 1024> t{};
    constexpr RangeLimitTable() {
        for (std::uint32_t i = 0; i < 1024; ++i) {
            t[i] = static_cast<std::uint8_t>(i < 128 ? i + 128 : i < 512 ? 255 : i < 896 ? 0 : i - 896);
        }
    }
};
constexpr RangeLimitTable kRangeLimit;

std::uint8_t rangeLimit(std::int64_t x) noexcept { return kRangeLimit.t[static_cast<std::uint32_t>(x) & 1023u]; }

// One 1-D pass of the islow IDCT over `in` (stride `step`), in arithmetic
// type T. Writes the eight outputs before descaling to `out`.
template <class T>
void idct1d(T d0, T d1, T d2, T d3, T d4, T d5, T d6, T d7, std::array<T, 8> &out) noexcept {
    T z1 = (d2 + d6) * static_cast<T>(kFix_0_541196100);
    const T tmp2e = z1 + d6 * static_cast<T>(-kFix_1_847759065);
    const T tmp3e = z1 + d2 * static_cast<T>(kFix_0_765366865);
    const T tmp0e = (d0 + d4) * static_cast<T>(1 << kConstBits);
    const T tmp1e = (d0 - d4) * static_cast<T>(1 << kConstBits);
    const T tmp10 = tmp0e + tmp3e;
    const T tmp13 = tmp0e - tmp3e;
    const T tmp11 = tmp1e + tmp2e;
    const T tmp12 = tmp1e - tmp2e;

    T tmp0 = d7;
    T tmp1 = d5;
    T tmp2 = d3;
    T tmp3 = d1;
    z1 = tmp0 + tmp3;
    T z2 = tmp1 + tmp2;
    T z3 = tmp0 + tmp2;
    T z4 = tmp1 + tmp3;
    const T z5 = (z3 + z4) * static_cast<T>(kFix_1_175875602);
    tmp0 *= static_cast<T>(kFix_0_298631336);
    tmp1 *= static_cast<T>(kFix_2_053119869);
    tmp2 *= static_cast<T>(kFix_3_072711026);
    tmp3 *= static_cast<T>(kFix_1_501321110);
    z1 *= static_cast<T>(-kFix_0_899976223);
    z2 *= static_cast<T>(-kFix_2_562915447);
    z3 *= static_cast<T>(-kFix_1_961570560);
    z4 *= static_cast<T>(-kFix_0_390180644);
    z3 += z5;
    z4 += z5;
    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    out[0] = tmp10 + tmp3;
    out[7] = tmp10 - tmp3;
    out[1] = tmp11 + tmp2;
    out[6] = tmp11 - tmp2;
    out[2] = tmp12 + tmp1;
    out[5] = tmp12 - tmp1;
    out[3] = tmp13 + tmp0;
    out[4] = tmp13 - tmp0;
}

// jidctint.c's jpeg_idct_islow, in 64-bit arithmetic: identical results to
// libjpeg's for every real image, and no overflow on hostile coefficients.
void idct(const std::int16_t *in, const std::uint16_t *q, std::uint8_t *out, std::size_t stride) noexcept {
    std::array<std::int64_t, 64> ws{};
    std::array<std::int64_t, 8> o{};
    for (std::size_t col = 0; col < 8; ++col) {
        const auto deq = [&](std::size_t row) { return std::int64_t{in[row * 8 + col]} * q[row * 8 + col]; };
        if (in[8 + col] == 0 && in[16 + col] == 0 && in[24 + col] == 0 && in[32 + col] == 0 && in[40 + col] == 0 &&
            in[48 + col] == 0 && in[56 + col] == 0) {
            const std::int64_t dc = deq(0) * (1 << kPass1Bits);
            for (std::size_t r = 0; r < 8; ++r) {
                ws[r * 8 + col] = dc;
            }
            continue;
        }
        idct1d<std::int64_t>(deq(0), deq(1), deq(2), deq(3), deq(4), deq(5), deq(6), deq(7), o);
        for (std::size_t r = 0; r < 8; ++r) {
            ws[r * 8 + col] = descale(o[r], kConstBits - kPass1Bits);
        }
    }
    for (std::size_t row = 0; row < 8; ++row, out += stride) {
        const std::int64_t *w = ws.data() + row * 8;
        if (w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0 && w[5] == 0 && w[6] == 0 && w[7] == 0) {
            std::memset(out, rangeLimit(descale(w[0], kPass1Bits + 3)), 8);
            continue;
        }
        idct1d<std::int64_t>(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], o);
        for (std::size_t i = 0; i < 8; ++i) {
            out[i] = rangeLimit(descale(o[i], kConstBits + kPass1Bits + 3));
        }
    }
}

// --- Upsampling (jdsample.c, fancy upsampling on) ----------------------------

// Row y of one component at full resolution (`width` samples into `dst`).
// A full-resolution component needs no work: the caller reads its plane.
void upsampleRow(const Component &c, const std::uint8_t *plane, std::size_t y, std::size_t width, int hMax, int vMax,
                 std::uint8_t *dst) noexcept {
    const std::size_t stride = c.blocksW * 8;
    const auto rowAt = [&](std::size_t r) { return plane + std::min(r, c.height - 1) * stride; };
    const int rh = hMax / c.h;
    const int rv = vMax / c.v;
    const std::size_t dw = c.width;
    if (rh == 2 && rv == 1 && dw > 2) {
        const std::uint8_t *in = rowAt(y);
        std::uint8_t *o = dst;
        int v = in[0];
        *o++ = static_cast<std::uint8_t>(v);
        *o++ = static_cast<std::uint8_t>((v * 3 + in[1] + 2) >> 2);
        for (std::size_t x = 1; x + 1 < dw; ++x) {
            v = in[x] * 3;
            *o++ = static_cast<std::uint8_t>((v + in[x - 1] + 1) >> 2);
            *o++ = static_cast<std::uint8_t>((v + in[x + 1] + 2) >> 2);
        }
        v = in[dw - 1];
        *o++ = static_cast<std::uint8_t>((v * 3 + in[dw - 2] + 1) >> 2);
        *o = static_cast<std::uint8_t>(v);
        return;
    }
    if (rh == 1 && rv == 2) {
        const std::size_t inRow = y / 2;
        const bool lower = (y & 1) != 0;
        const std::uint8_t *in0 = rowAt(inRow);
        const std::uint8_t *in1 = lower ? rowAt(inRow + 1) : rowAt(inRow == 0 ? 0 : inRow - 1);
        const int bias = lower ? 2 : 1;
        for (std::size_t x = 0; x < width; ++x) {
            dst[x] = static_cast<std::uint8_t>((in0[x] * 3 + in1[x] + bias) >> 2);
        }
        return;
    }
    if (rh == 2 && rv == 2 && dw > 2) {
        const std::size_t inRow = y / 2;
        const bool lower = (y & 1) != 0;
        const std::uint8_t *in0 = rowAt(inRow);
        const std::uint8_t *in1 = lower ? rowAt(inRow + 1) : rowAt(inRow == 0 ? 0 : inRow - 1);
        std::uint8_t *o = dst;
        int thisSum = in0[0] * 3 + in1[0];
        int nextSum = in0[1] * 3 + in1[1];
        *o++ = static_cast<std::uint8_t>((thisSum * 4 + 8) >> 4);
        *o++ = static_cast<std::uint8_t>((thisSum * 3 + nextSum + 7) >> 4);
        int lastSum = thisSum;
        thisSum = nextSum;
        for (std::size_t x = 2; x < dw; ++x) {
            nextSum = in0[x] * 3 + in1[x];
            *o++ = static_cast<std::uint8_t>((thisSum * 3 + lastSum + 8) >> 4);
            *o++ = static_cast<std::uint8_t>((thisSum * 3 + nextSum + 7) >> 4);
            lastSum = thisSum;
            thisSum = nextSum;
        }
        *o++ = static_cast<std::uint8_t>((thisSum * 3 + lastSum + 8) >> 4);
        *o = static_cast<std::uint8_t>((thisSum * 4 + 7) >> 4);
        return;
    }
    // Any other integer ratio: replicate (libjpeg's int_upsample, and the
    // plain h2v1/h2v2 used for components one or two samples wide).
    const std::uint8_t *in = plane + (y / static_cast<std::size_t>(rv)) * stride;
    for (std::size_t x = 0; x < width; ++x) {
        dst[x] = in[x / static_cast<std::size_t>(rh)];
    }
}

// --- Colour conversion (jdcolor.c) -------------------------------------------

struct YccTables {
    std::array<int, 256> crR{};
    std::array<int, 256> cbB{};
    std::array<std::int32_t, 256> crG{};
    std::array<std::int32_t, 256> cbG{};
    YccTables() {
        constexpr int kScale = 16;
        constexpr std::int32_t kHalf = std::int32_t{1} << (kScale - 1);
        const auto fix = [](double x) { return static_cast<std::int32_t>(x * (1 << kScale) + 0.5); };
        for (int i = 0; i < 256; ++i) {
            const int x = i - 128;
            crR[static_cast<std::size_t>(i)] = (fix(1.40200) * x + kHalf) >> kScale;
            cbB[static_cast<std::size_t>(i)] = (fix(1.77200) * x + kHalf) >> kScale;
            crG[static_cast<std::size_t>(i)] = -fix(0.71414) * x;
            cbG[static_cast<std::size_t>(i)] = -fix(0.34414) * x + kHalf;
        }
    }
};

const YccTables &yccTables() {
    static const YccTables tables;
    return tables;
}

// libjpeg's sample range limit: clamps -256..511 to 0..255.
struct ClampTable {
    std::array<std::uint8_t, 768> t{};
    ClampTable() {
        for (std::size_t i = 0; i < t.size(); ++i) {
            t[i] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(i) - 256, 0, 255));
        }
    }
    [[nodiscard]] std::uint8_t operator()(int v) const noexcept { return t[static_cast<std::size_t>(v + 256)]; }
};

void yccToRgbRow(const YccTables &t, const ClampTable &clamp, const std::uint8_t *y, const std::uint8_t *cb,
                 const std::uint8_t *cr, std::size_t n, std::uint8_t *out, std::size_t outStep) noexcept {
    for (std::size_t i = 0; i < n; ++i, out += outStep) {
        const int yy = y[i];
        out[0] = clamp(yy + t.crR[cr[i]]);
        out[1] = clamp(yy + ((t.cbG[cb[i]] + t.crG[cr[i]]) >> 16));
        out[2] = clamp(yy + t.cbB[cb[i]]);
    }
}

Result<Image> Decoder::finish() {
    if (!haveFrame) {
        return corrupt("no frame");
    }
    if (scans == 0) {
        return corrupt("no image data");
    }
    // A baseline file is complete once every MCU has decoded. A progressive
    // file cut between scans would decode to a blurrier image, which a
    // partial download must not pass for; it has to reach its EOI.
    if (progressive && !sawEnd) {
        return corrupt("progressive file is truncated");
    }
    for (const Component &c : components) {
        if (!quantDefined[static_cast<std::size_t>(c.quantTable)]) {
            return corrupt("undefined quantisation table");
        }
    }
    ColorSpace space = ColorSpace::YCbCr;
    if (components.size() == 1) {
        space = ColorSpace::Gray;
    } else if (components.size() == 3) {
        // jdapimin.c default_decompress_parms.
        if (sawJfif) {
            space = ColorSpace::YCbCr;
        } else if (sawAdobe) {
            space = adobeTransform == 0 ? ColorSpace::Rgb : ColorSpace::YCbCr;
        } else if (components[0].id == 'R' && components[1].id == 'G' && components[2].id == 'B') {
            space = ColorSpace::Rgb;
        }
    } else {
        space = sawAdobe && adobeTransform == 2 ? ColorSpace::Ycck : ColorSpace::Cmyk;
    }

    if (progressive) {
        for (Component &c : components) {
            const std::size_t stride = c.blocksW * 8;
            const std::uint16_t *q = quant[static_cast<std::size_t>(c.quantTable)].data();
            for (std::size_t by = 0; by < c.usedBlocksH; ++by) {
                for (std::size_t bx = 0; bx < c.usedBlocksW; ++bx) {
                    idct(c.block(bx, by), q, c.samples.data() + by * 8 * stride + bx * 8, stride);
                }
            }
            c.coefficients = {}; // free as we go
        }
    }

    Result<Image> created = Image::create(width, height, AlphaMode::Straight, limits);
    if (!created) {
        return created;
    }
    Image image = std::move(created).value();
    const YccTables &tables = yccTables();
    static const ClampTable clamp;
    // Upsampled rows go through small line buffers; full-resolution
    // components are read straight from their planes.
    std::vector<std::vector<std::uint8_t>> lines(components.size(), std::vector<std::uint8_t>(std::size_t{width} + 16));
    std::array<const std::uint8_t *, 4> rows{};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::size_t i = 0; i < components.size(); ++i) {
            const Component &c = components[i];
            if (c.h == hMax && c.v == vMax) {
                rows[i] = components[i].samples.data() + std::size_t{y} * c.blocksW * 8;
            } else {
                upsampleRow(c, components[i].samples.data(), y, width, hMax, vMax, lines[i].data());
                rows[i] = lines[i].data();
            }
        }
        std::uint8_t *out = image.row(y).data();
        switch (space) {
        case ColorSpace::Gray:
            for (std::size_t x = 0; x < width; ++x, out += 4) {
                out[0] = out[1] = out[2] = rows[0][x];
                out[3] = 255;
            }
            break;
        case ColorSpace::Rgb:
            for (std::size_t x = 0; x < width; ++x, out += 4) {
                out[0] = rows[0][x];
                out[1] = rows[1][x];
                out[2] = rows[2][x];
                out[3] = 255;
            }
            break;
        case ColorSpace::YCbCr:
            yccToRgbRow(tables, clamp, rows[0], rows[1], rows[2], width, out, 4);
            for (std::size_t x = 0; x < width; ++x) {
                out[x * 4 + 3] = 255;
            }
            break;
        case ColorSpace::Cmyk:
        case ColorSpace::Ycck:
            for (std::size_t x = 0; x < width; ++x, out += 4) {
                std::array<std::uint8_t, 3> cmy{rows[0][x], rows[1][x], rows[2][x]};
                if (space == ColorSpace::Ycck) {
                    std::array<std::uint8_t, 3> rgb{};
                    yccToRgbRow(tables, clamp, &cmy[0], &cmy[1], &cmy[2], 1, rgb.data(), 3);
                    cmy = {static_cast<std::uint8_t>(255 - rgb[0]), static_cast<std::uint8_t>(255 - rgb[1]),
                           static_cast<std::uint8_t>(255 - rgb[2])};
                }
                // Adobe's inverted CMYK: each channel times K (Qt's conversion).
                const unsigned k = rows[3][x];
                out[0] = static_cast<std::uint8_t>(k * cmy[0] / 255);
                out[1] = static_cast<std::uint8_t>(k * cmy[1] / 255);
                out[2] = static_cast<std::uint8_t>(k * cmy[2] / 255);
                out[3] = 255;
            }
            break;
        }
    }
    return image;
}

Result<Image> Decoder::run() {
    if (size > limits.maxInputBytes) {
        return Error(ErrorCode::LimitExceeded, "jpeg: file exceeds the size limit");
    }
    if (size < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        return corrupt("not a JPEG file");
    }
    std::size_t pos = 2;
    for (;;) {
        // Find the next marker, skipping fill bytes (and, as libjpeg does,
        // any garbage before it).
        while (pos < size && data[pos] != 0xFF) {
            ++pos;
        }
        while (pos < size && data[pos] == 0xFF) {
            ++pos;
        }
        if (pos >= size) {
            break; // no EOI: accept what was decoded, as libjpeg does
        }
        const unsigned marker = data[pos++];
        if (marker == 0xD9) {
            sawEnd = true;
            break;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue; // standalone markers
        }
        if (size - pos < 2) {
            return corrupt("truncated segment");
        }
        const std::size_t length = be16(data + pos);
        if (length < 2 || size - pos < length) {
            return corrupt("segment runs past the end of the file");
        }
        const std::uint8_t *body = data + pos + 2;
        const std::size_t bodyLength = length - 2;
        pos += length;
        Result<void> ok = success();
        switch (marker) {
        case 0xC4: ok = readHuffman(body, bodyLength); break;
        case 0xDB: ok = readQuant(body, bodyLength); break;
        case 0xDD:
            if (bodyLength < 2) {
                return corrupt("truncated restart interval");
            }
            restartInterval = be16(body);
            break;
        case 0xDA: {
            Result<std::size_t> end = readScan(body, bodyLength, pos);
            if (!end) {
                return std::move(end).error();
            }
            pos = end.value();
            break;
        }
        case 0xE0:
            if (bodyLength >= 5 && std::memcmp(body, "JFIF\0", 5) == 0) {
                sawJfif = true;
            }
            break;
        case 0xEE:
            if (bodyLength >= 12 && std::memcmp(body, "Adobe", 5) == 0) {
                sawAdobe = true;
                adobeTransform = body[11];
            }
            break;
        case 0xDC: return unsupported("DNL marker");
        default:
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                ok = readFrame(body, bodyLength, marker);
            }
            // Everything else (APPn, COM, DAC...) is skipped.
            break;
        }
        if (!ok) {
            return std::move(ok).error();
        }
    }
    return finish();
}

} // namespace

bool isJpeg(Span<const std::byte> data) noexcept {
    return data.size() >= 3 && data[0] == std::byte{0xFF} && data[1] == std::byte{0xD8} && data[2] == std::byte{0xFF};
}

Result<Image> decodeJpeg(Span<const std::byte> data, const ImageLimits &limits) {
    try {
        Decoder decoder{reinterpret_cast<const std::uint8_t *>(data.data()), data.size(), limits};
        return decoder.run();
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "jpeg: out of memory");
    }
}

} // namespace cfw
