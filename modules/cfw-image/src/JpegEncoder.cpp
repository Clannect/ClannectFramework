// Baseline JPEG encoder: JFIF, YCbCr with optional 4:2:0 subsampling, the
// IJG quality scale, and the standard Huffman tables.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <new>
#include <numbers>

#include "JpegTables.h"
#include "cfw/image/Jpeg.h"

namespace cfw {

namespace {

using jpeg::kZigzag;

struct HuffmanCode {
    std::array<std::uint16_t, 256> code{};
    std::array<std::uint8_t, 256> size{};

    explicit HuffmanCode(const jpeg::StandardHuffman &table) {
        std::uint16_t c = 0;
        std::size_t k = 0;
        for (unsigned len = 1; len <= 16; ++len) {
            for (unsigned i = 0; i < table.counts[len - 1]; ++i, ++k, ++c) {
                code[table.values[k]] = c;
                size[table.values[k]] = static_cast<std::uint8_t>(len);
            }
            c = static_cast<std::uint16_t>(c << 1);
        }
    }
};

class BitWriter {
public:
    explicit BitWriter(std::vector<std::byte> &out) noexcept : m_out(out) {}

    void put(std::uint32_t bits, unsigned n) {
        m_bits = (m_bits << n) | (bits & ((1u << n) - 1u));
        m_count += n;
        while (m_count >= 8) {
            const auto byte = static_cast<std::uint8_t>(m_bits >> (m_count - 8));
            m_out.push_back(static_cast<std::byte>(byte));
            if (byte == 0xFF) {
                m_out.push_back(std::byte{0}); // byte stuffing
            }
            m_count -= 8;
        }
    }
    // Pads the last byte with 1 bits (T.81 F.1.2.3).
    void flush() {
        if (m_count > 0) {
            put(0x7Fu, 8 - m_count);
        }
    }

private:
    std::vector<std::byte> &m_out;
    std::uint64_t m_bits = 0;
    unsigned m_count = 0;
};

void putByte(std::vector<std::byte> &out, unsigned b) { out.push_back(static_cast<std::byte>(b & 0xFFu)); }
void put16(std::vector<std::byte> &out, unsigned v) {
    putByte(out, v >> 8);
    putByte(out, v);
}

std::array<std::uint8_t, 64> scaledQuant(const std::array<std::uint8_t, 64> &base, int quality) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    std::array<std::uint8_t, 64> out{};
    for (std::size_t i = 0; i < 64; ++i) {
        out[i] = static_cast<std::uint8_t>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
    }
    return out;
}

struct DctTable {
    std::array<double, 64> c{}; // c[u * 8 + x] = C(u) / 2 * cos((2x + 1) u pi / 16)
    DctTable() {
        for (std::size_t u = 0; u < 8; ++u) {
            const double cu = u == 0 ? std::sqrt(0.5) : 1.0;
            for (std::size_t x = 0; x < 8; ++x) {
                c[u * 8 + x] = cu / 2.0 * std::cos((2.0 * static_cast<double>(x) + 1.0) * static_cast<double>(u) * std::numbers::pi / 16.0);
            }
        }
    }
};

// Forward DCT of an 8x8 block of level-shifted samples, then quantisation.
void fdctQuantize(const std::array<float, 64> &in, const std::array<std::uint8_t, 64> &q, std::array<int, 64> &out) {
    static const DctTable table;
    std::array<double, 64> tmp{};
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t u = 0; u < 8; ++u) {
            double s = 0;
            for (std::size_t x = 0; x < 8; ++x) {
                s += table.c[u * 8 + x] * in[y * 8 + x];
            }
            tmp[y * 8 + u] = s;
        }
    }
    for (std::size_t u = 0; u < 8; ++u) {
        for (std::size_t v = 0; v < 8; ++v) {
            double s = 0;
            for (std::size_t y = 0; y < 8; ++y) {
                s += table.c[v * 8 + y] * tmp[y * 8 + u];
            }
            // Baseline allows 11-bit DC and 10-bit AC magnitudes.
            const int limit = u == 0 && v == 0 ? 2047 : 1023;
            out[v * 8 + u] = std::clamp(static_cast<int>(std::lround(s / q[v * 8 + u])), -limit, limit);
        }
    }
}

unsigned bitLength(unsigned v) noexcept {
    unsigned n = 0;
    while (v != 0) {
        ++n;
        v >>= 1;
    }
    return n;
}

void encodeBlock(BitWriter &w, const std::array<int, 64> &coef, int &predictor, const HuffmanCode &dc,
                 const HuffmanCode &ac) {
    const int diff = coef[0] - predictor;
    predictor = coef[0];
    const unsigned dcSize = bitLength(static_cast<unsigned>(std::abs(diff)));
    w.put(dc.code[dcSize], dc.size[dcSize]);
    if (dcSize != 0) {
        w.put(static_cast<std::uint32_t>(diff < 0 ? diff - 1 : diff), dcSize);
    }
    int run = 0;
    for (std::size_t k = 1; k < 64; ++k) {
        const int v = coef[kZigzag[k]];
        if (v == 0) {
            ++run;
            continue;
        }
        while (run > 15) {
            w.put(ac.code[0xF0], ac.size[0xF0]); // 16 zeros
            run -= 16;
        }
        const unsigned size = bitLength(static_cast<unsigned>(std::abs(v)));
        const unsigned symbol = static_cast<unsigned>(run) << 4 | size;
        w.put(ac.code[symbol], ac.size[symbol]);
        w.put(static_cast<std::uint32_t>(v < 0 ? v - 1 : v), size);
        run = 0;
    }
    if (run > 0) {
        w.put(ac.code[0x00], ac.size[0x00]); // end of block
    }
}

void writeHuffmanTable(std::vector<std::byte> &out, unsigned classAndId, const jpeg::StandardHuffman &t) {
    putByte(out, classAndId);
    for (const std::uint8_t c : t.counts) {
        putByte(out, c);
    }
    for (std::size_t i = 0; i < t.valueCount; ++i) {
        putByte(out, t.values[i]);
    }
}

Result<std::vector<std::byte>> encode(const Image &source, const JpegEncodeOptions &options) {
    if (source.empty()) {
        return Error(ErrorCode::InvalidArgument, "jpeg: cannot encode an empty image");
    }
    if (source.width() > 65535 || source.height() > 65535) {
        return Error(ErrorCode::InvalidArgument, "jpeg: images are limited to 65535 pixels a side");
    }
    const Image *image = &source;
    Image straight;
    if (source.alphaMode() == AlphaMode::Premultiplied) {
        Result<Image> copy = source.copy();
        if (!copy) {
            return std::move(copy).error();
        }
        straight = std::move(copy).value();
        straight.unpremultiply();
        image = &straight;
    }
    const std::size_t width = image->width();
    const std::size_t height = image->height();
    const std::size_t mcu = options.subsampleChroma ? 16 : 8;
    const std::size_t paddedW = (width + mcu - 1) / mcu * mcu;
    const std::size_t paddedH = (height + mcu - 1) / mcu * mcu;

    // Full-resolution Y, Cb, Cr (jccolor.c constants), edges replicated.
    std::vector<float> planes[3];
    for (auto &p : planes) {
        p.resize(paddedW * paddedH);
    }
    for (std::size_t y = 0; y < paddedH; ++y) {
        const Span<const std::uint8_t> row = image->row(static_cast<std::uint32_t>(std::min(y, height - 1)));
        for (std::size_t x = 0; x < paddedW; ++x) {
            const std::uint8_t *px = row.data() + std::min(x, width - 1) * 4;
            const float r = px[0];
            const float g = px[1];
            const float b = px[2];
            const std::size_t i = y * paddedW + x;
            planes[0][i] = 0.29900f * r + 0.58700f * g + 0.11400f * b;
            planes[1][i] = -0.16874f * r - 0.33126f * g + 0.50000f * b + 128.0f;
            planes[2][i] = 0.50000f * r - 0.41869f * g - 0.08131f * b + 128.0f;
        }
    }

    const auto lumQ = scaledQuant(jpeg::kLuminanceQuant, options.quality);
    const auto chromQ = scaledQuant(jpeg::kChrominanceQuant, options.quality);
    const HuffmanCode dcLum(jpeg::kDcLuminance);
    const HuffmanCode acLum(jpeg::kAcLuminance);
    const HuffmanCode dcChrom(jpeg::kDcChrominance);
    const HuffmanCode acChrom(jpeg::kAcChrominance);

    std::vector<std::byte> out;
    out.reserve(width * height / 4 + 1024);
    put16(out, 0xFFD8);
    // APP0 JFIF 1.01, no density.
    put16(out, 0xFFE0);
    put16(out, 16);
    for (const char c : {'J', 'F', 'I', 'F', '\0'}) {
        putByte(out, static_cast<unsigned>(c));
    }
    putByte(out, 1);
    putByte(out, 1);
    putByte(out, 0);
    put16(out, 1);
    put16(out, 1);
    putByte(out, 0);
    putByte(out, 0);
    // DQT: both tables, zigzag order.
    put16(out, 0xFFDB);
    put16(out, 2 + 2 * 65);
    for (unsigned id = 0; id < 2; ++id) {
        putByte(out, id);
        const auto &q = id == 0 ? lumQ : chromQ;
        for (std::size_t k = 0; k < 64; ++k) {
            putByte(out, q[kZigzag[k]]);
        }
    }
    // SOF0.
    put16(out, 0xFFC0);
    put16(out, 8 + 3 * 3);
    putByte(out, 8);
    put16(out, static_cast<unsigned>(height));
    put16(out, static_cast<unsigned>(width));
    putByte(out, 3);
    putByte(out, 1);
    putByte(out, options.subsampleChroma ? 0x22u : 0x11u);
    putByte(out, 0);
    putByte(out, 2);
    putByte(out, 0x11);
    putByte(out, 1);
    putByte(out, 3);
    putByte(out, 0x11);
    putByte(out, 1);
    // DHT: the four standard tables.
    put16(out, 0xFFC4);
    put16(out, 2 + (17 + 12) * 2 + (17 + 162) * 2);
    writeHuffmanTable(out, 0x00, jpeg::kDcLuminance);
    writeHuffmanTable(out, 0x10, jpeg::kAcLuminance);
    writeHuffmanTable(out, 0x01, jpeg::kDcChrominance);
    writeHuffmanTable(out, 0x11, jpeg::kAcChrominance);
    // SOS.
    put16(out, 0xFFDA);
    put16(out, 6 + 2 * 3);
    putByte(out, 3);
    putByte(out, 1);
    putByte(out, 0x00);
    putByte(out, 2);
    putByte(out, 0x11);
    putByte(out, 3);
    putByte(out, 0x11);
    putByte(out, 0);
    putByte(out, 63);
    putByte(out, 0);

    BitWriter w(out);
    std::array<int, 3> predictors{};
    std::array<float, 64> block{};
    std::array<int, 64> coef{};
    const auto loadBlock = [&](const std::vector<float> &plane, std::size_t x0, std::size_t y0) {
        for (std::size_t y = 0; y < 8; ++y) {
            for (std::size_t x = 0; x < 8; ++x) {
                block[y * 8 + x] = plane[(y0 + y) * paddedW + x0 + x] - 128.0f;
            }
        }
    };
    // 2x2 average for subsampled chroma.
    const auto loadSubsampled = [&](const std::vector<float> &plane, std::size_t x0, std::size_t y0) {
        for (std::size_t y = 0; y < 8; ++y) {
            for (std::size_t x = 0; x < 8; ++x) {
                const std::size_t i = (y0 + 2 * y) * paddedW + x0 + 2 * x;
                block[y * 8 + x] = (plane[i] + plane[i + 1] + plane[i + paddedW] + plane[i + paddedW + 1]) * 0.25f - 128.0f;
            }
        }
    };
    for (std::size_t my = 0; my < paddedH; my += mcu) {
        for (std::size_t mx = 0; mx < paddedW; mx += mcu) {
            for (std::size_t by = 0; by < mcu; by += 8) {
                for (std::size_t bx = 0; bx < mcu; bx += 8) {
                    loadBlock(planes[0], mx + bx, my + by);
                    fdctQuantize(block, lumQ, coef);
                    encodeBlock(w, coef, predictors[0], dcLum, acLum);
                }
            }
            for (std::size_t c = 1; c < 3; ++c) {
                if (options.subsampleChroma) {
                    loadSubsampled(planes[c], mx, my);
                } else {
                    loadBlock(planes[c], mx, my);
                }
                fdctQuantize(block, chromQ, coef);
                encodeBlock(w, coef, predictors[c], dcChrom, acChrom);
            }
        }
    }
    w.flush();
    put16(out, 0xFFD9);
    return out;
}

} // namespace

Result<std::vector<std::byte>> encodeJpeg(const Image &image, const JpegEncodeOptions &options) {
    try {
        return encode(image, options);
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "jpeg: out of memory");
    }
}

} // namespace cfw
