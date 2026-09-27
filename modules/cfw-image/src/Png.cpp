#include "cfw/image/Png.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "cfw/core/Checksum.h"
#include "cfw/core/Deflate.h"

namespace cfw {

namespace {

constexpr std::array<std::uint8_t, 8> kSignature = {137, 80, 78, 71, 13, 10, 26, 10};

// Adam7: pass origins and steps.
constexpr std::array<std::uint32_t, 7> kPassX = {0, 4, 0, 2, 0, 1, 0};
constexpr std::array<std::uint32_t, 7> kPassY = {0, 0, 4, 0, 2, 0, 1};
constexpr std::array<std::uint32_t, 7> kPassDx = {8, 8, 4, 4, 2, 2, 1};
constexpr std::array<std::uint32_t, 7> kPassDy = {8, 8, 8, 4, 4, 2, 2};

Error corrupt(const char *what) { return Error(ErrorCode::Corrupt, String("png: ") + what); }

std::uint32_t be32(const std::uint8_t *p) noexcept {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

std::uint16_t be16(const std::uint8_t *p) noexcept { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

struct Header {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    unsigned depth = 0;
    unsigned colorType = 0;
    bool interlaced = false;

    [[nodiscard]] unsigned channels() const noexcept {
        switch (colorType) {
        case 2: return 3;
        case 4: return 2;
        case 6: return 4;
        default: return 1; // 0 gray, 3 palette
        }
    }
    [[nodiscard]] unsigned bitsPerPixel() const noexcept { return channels() * depth; }
    // Bytes per complete pixel for filtering (at least 1).
    [[nodiscard]] std::size_t filterStride() const noexcept { return std::max(1u, bitsPerPixel() / 8); }
    [[nodiscard]] std::size_t rowBytes(std::uint32_t pixels) const noexcept {
        return (std::size_t{pixels} * bitsPerPixel() + 7) / 8;
    }
};

bool validDepth(unsigned colorType, unsigned depth) noexcept {
    switch (colorType) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6: return depth == 8 || depth == 16;
    default: return false;
    }
}

struct Transparency {
    bool present = false;
    std::array<std::uint16_t, 3> key{}; // gray (key[0]) or RGB key colour, at the image's bit depth
};

struct PassSize {
    std::uint32_t width;
    std::uint32_t height;
};

PassSize passSize(const Header &h, int pass) noexcept {
    if (pass < 0) {
        return {h.width, h.height};
    }
    const auto p = static_cast<std::size_t>(pass);
    const std::uint32_t w = h.width > kPassX[p] ? (h.width - kPassX[p] + kPassDx[p] - 1) / kPassDx[p] : 0;
    const std::uint32_t hh = h.height > kPassY[p] ? (h.height - kPassY[p] + kPassDy[p] - 1) / kPassDy[p] : 0;
    return {w, hh};
}

std::uint8_t paeth(int a, int b, int c) noexcept {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return static_cast<std::uint8_t>(a);
    }
    return static_cast<std::uint8_t>(pb <= pc ? b : c);
}

// Reverses one row's filter in place. `prev` is the previous unfiltered row
// of the same pass, or null for the first.
bool unfilter(unsigned type, std::uint8_t *row, const std::uint8_t *prev, std::size_t n, std::size_t bpp) noexcept {
    switch (type) {
    case 0: return true;
    case 1:
        for (std::size_t i = bpp; i < n; ++i) {
            row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
        }
        return true;
    case 2:
        if (prev != nullptr) {
            for (std::size_t i = 0; i < n; ++i) {
                row[i] = static_cast<std::uint8_t>(row[i] + prev[i]);
            }
        }
        return true;
    case 3:
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned left = i >= bpp ? row[i - bpp] : 0u;
            const unsigned up = prev != nullptr ? prev[i] : 0u;
            row[i] = static_cast<std::uint8_t>(row[i] + ((left + up) >> 1));
        }
        return true;
    case 4:
        for (std::size_t i = 0; i < n; ++i) {
            const int left = i >= bpp ? row[i - bpp] : 0;
            const int up = prev != nullptr ? prev[i] : 0;
            const int upLeft = (i >= bpp && prev != nullptr) ? prev[i - bpp] : 0;
            row[i] = static_cast<std::uint8_t>(row[i] + paeth(left, up, upLeft));
        }
        return true;
    default: return false;
    }
}

// Sample `index` of a row packed at `depth` bits per sample.
unsigned sampleAt(const std::uint8_t *row, std::size_t index, unsigned depth) noexcept {
    switch (depth) {
    case 16: return static_cast<unsigned>(row[index * 2] << 8 | row[index * 2 + 1]);
    case 8: return row[index];
    default: {
        const std::size_t bit = index * depth;
        const unsigned shift = 8 - depth - static_cast<unsigned>(bit % 8);
        return (row[bit / 8] >> shift) & ((1u << depth) - 1u);
    }
    }
}

// Scales a sample to 8 bits: the high byte for 16-bit, replication for 1-4.
std::uint8_t to8(unsigned v, unsigned depth) noexcept {
    switch (depth) {
    case 16: return static_cast<std::uint8_t>(v >> 8);
    case 8: return static_cast<std::uint8_t>(v);
    case 4: return static_cast<std::uint8_t>(v * 17);
    case 2: return static_cast<std::uint8_t>(v * 85);
    default: return static_cast<std::uint8_t>(v * 255);
    }
}

void convertRow(const Header &h, const std::uint8_t *src, std::uint32_t width, std::uint8_t *out,
                const std::vector<std::array<std::uint8_t, 4>> &palette, const Transparency &trns) {
    const unsigned d = h.depth;
    for (std::uint32_t x = 0; x < width; ++x, out += 4) {
        switch (h.colorType) {
        case 0: {
            const unsigned g = sampleAt(src, x, d);
            out[0] = out[1] = out[2] = to8(g, d);
            out[3] = trns.present && g == trns.key[0] ? 0 : 255;
            break;
        }
        case 2: {
            const unsigned r = sampleAt(src, std::size_t{x} * 3, d);
            const unsigned g = sampleAt(src, std::size_t{x} * 3 + 1, d);
            const unsigned b = sampleAt(src, std::size_t{x} * 3 + 2, d);
            out[0] = to8(r, d);
            out[1] = to8(g, d);
            out[2] = to8(b, d);
            out[3] = trns.present && r == trns.key[0] && g == trns.key[1] && b == trns.key[2] ? 0 : 255;
            break;
        }
        case 3: {
            const unsigned index = sampleAt(src, x, d);
            // An index past the palette draws opaque black, as browsers do.
            static constexpr std::array<std::uint8_t, 4> kBlack = {0, 0, 0, 255};
            const auto &entry = index < palette.size() ? palette[index] : kBlack;
            std::memcpy(out, entry.data(), 4);
            break;
        }
        case 4: {
            const std::uint8_t g = to8(sampleAt(src, std::size_t{x} * 2, d), d);
            out[0] = out[1] = out[2] = g;
            out[3] = to8(sampleAt(src, std::size_t{x} * 2 + 1, d), d);
            break;
        }
        default: // 6
            for (std::size_t c = 0; c < 4; ++c) {
                out[c] = to8(sampleAt(src, std::size_t{x} * 4 + c, d), d);
            }
            break;
        }
    }
}

Result<Image> decode(Span<const std::byte> input, const ImageLimits &limits) {
    if (input.size() > limits.maxInputBytes) {
        return Error(ErrorCode::LimitExceeded, "png: file exceeds the size limit");
    }
    if (!isPng(input)) {
        return corrupt("not a PNG file");
    }
    const auto *data = reinterpret_cast<const std::uint8_t *>(input.data());
    const std::size_t size = input.size();

    Header h;
    bool haveHeader = false;
    std::vector<std::array<std::uint8_t, 4>> palette;
    Transparency trns;
    std::vector<std::uint8_t> trnsPalette;
    std::vector<std::byte> idat;
    bool sawIdat = false;

    std::size_t pos = kSignature.size();
    while (pos + 12 <= size) {
        const std::uint32_t length = be32(data + pos);
        if (length > 0x7FFFFFFFu || size - pos - 12 < length) {
            return corrupt("chunk runs past the end of the file");
        }
        const std::uint8_t *type = data + pos + 4;
        const std::uint8_t *body = type + 4;
        for (int i = 0; i < 4; ++i) {
            const auto c = type[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
                return corrupt("invalid chunk type");
            }
        }
        const bool critical = (type[0] & 0x20u) == 0;
        const std::uint32_t storedCrc = be32(body + length);
        const std::uint32_t actualCrc = crc32(Span<const std::byte>(input.data() + pos + 4, length + 4u));
        pos += 12u + length;
        if (storedCrc != actualCrc) {
            if (critical) {
                return corrupt("chunk CRC mismatch");
            }
            continue; // a damaged ancillary chunk is dropped, as libpng does
        }
        const auto is = [&](const char *name) { return std::memcmp(type, name, 4) == 0; };

        if (!haveHeader) {
            if (!is("IHDR") || length != 13) {
                return corrupt("IHDR must come first");
            }
            h.width = be32(body);
            h.height = be32(body + 4);
            h.depth = body[8];
            h.colorType = body[9];
            if (h.width == 0 || h.height == 0 || h.width > 0x7FFFFFFFu || h.height > 0x7FFFFFFFu) {
                return corrupt("invalid dimensions");
            }
            if (!validDepth(h.colorType, h.depth)) {
                return corrupt("invalid colour type and bit depth");
            }
            if (body[10] != 0 || body[11] != 0 || body[12] > 1) {
                return corrupt("unknown compression, filter or interlace method");
            }
            h.interlaced = body[12] == 1;
            if (Result<void> ok = checkImageSize(h.width, h.height, limits); !ok) {
                return std::move(ok).error();
            }
            haveHeader = true;
        } else if (is("IDAT")) {
            sawIdat = true;
            idat.insert(idat.end(), input.begin() + static_cast<std::ptrdiff_t>(body - data),
                        input.begin() + static_cast<std::ptrdiff_t>(body - data + length));
        } else if (is("IEND")) {
            break;
        } else if (is("PLTE")) {
            if (sawIdat) {
                continue;
            }
            if (length % 3 != 0 || length == 0 || length / 3 > 256 ||
                (h.colorType == 3 && length / 3 > (1u << h.depth))) {
                return corrupt("invalid palette");
            }
            palette.clear();
            for (std::uint32_t i = 0; i < length; i += 3) {
                palette.push_back({body[i], body[i + 1], body[i + 2], 255});
            }
        } else if (is("tRNS")) {
            // Keys use only the low `depth` bits, as libpng reads them.
            const auto mask = static_cast<std::uint16_t>((1u << h.depth) - 1u);
            if (h.colorType == 0 && length >= 2) {
                trns.present = true;
                trns.key[0] = static_cast<std::uint16_t>(be16(body) & mask);
            } else if (h.colorType == 2 && length >= 6) {
                trns.present = true;
                trns.key = {static_cast<std::uint16_t>(be16(body) & mask), static_cast<std::uint16_t>(be16(body + 2) & mask),
                            static_cast<std::uint16_t>(be16(body + 4) & mask)};
            } else if (h.colorType == 3) {
                trnsPalette.assign(body, body + length);
            }
            // tRNS on a type with an alpha channel is ignored (libpng warns).
        } else if (is("IHDR")) {
            return corrupt("duplicate IHDR");
        } else if (critical) {
            return Error(ErrorCode::Unsupported, "png: unknown critical chunk").with("chunk", String(reinterpret_cast<const char *>(type), 4));
        }
    }
    if (!haveHeader) {
        return corrupt("missing IHDR");
    }
    if (!sawIdat) {
        return corrupt("missing image data");
    }
    if (h.colorType == 3) {
        if (palette.empty()) {
            return corrupt("palette image without PLTE");
        }
        for (std::size_t i = 0; i < trnsPalette.size() && i < palette.size(); ++i) {
            palette[i][3] = trnsPalette[i];
        }
    }

    // The exact size of the filtered data the header implies.
    std::size_t expected = 0;
    for (int pass = h.interlaced ? 0 : -1; pass < (h.interlaced ? 7 : 0); ++pass) {
        const PassSize ps = passSize(h, pass);
        if (ps.width != 0 && ps.height != 0) {
            expected += std::size_t{ps.height} * (1 + h.rowBytes(ps.width));
        }
    }
    Result<std::vector<std::byte>> raw = zlibDecompress(idat, {expected});
    if (!raw) {
        if (raw.error().code() == ErrorCode::LimitExceeded) {
            return corrupt("more image data than the header allows");
        }
        return Error(raw.error().code(), "png: " + raw.error().message());
    }
    std::vector<std::byte> &filtered = raw.value();
    if (filtered.size() < expected) {
        return corrupt("image data is truncated");
    }

    Result<Image> created = Image::create(h.width, h.height, AlphaMode::Straight, limits);
    if (!created) {
        return created;
    }
    Image image = std::move(created).value();
    auto *bytes = reinterpret_cast<std::uint8_t *>(filtered.data());
    const std::size_t bpp = h.filterStride();
    std::vector<std::uint8_t> passRow;
    std::size_t offset = 0;
    for (int pass = h.interlaced ? 0 : -1; pass < (h.interlaced ? 7 : 0); ++pass) {
        const PassSize ps = passSize(h, pass);
        if (ps.width == 0 || ps.height == 0) {
            continue;
        }
        const std::size_t rowBytes = h.rowBytes(ps.width);
        const std::uint8_t *prev = nullptr;
        passRow.resize(std::size_t{ps.width} * 4);
        for (std::uint32_t y = 0; y < ps.height; ++y) {
            std::uint8_t *row = bytes + offset + 1;
            if (!unfilter(bytes[offset], row, prev, rowBytes, bpp)) {
                return corrupt("invalid filter type");
            }
            if (pass < 0) {
                convertRow(h, row, ps.width, image.row(y).data(), palette, trns);
            } else {
                const auto p = static_cast<std::size_t>(pass);
                convertRow(h, row, ps.width, passRow.data(), palette, trns);
                std::uint8_t *dst = image.row(kPassY[p] + y * kPassDy[p]).data();
                for (std::uint32_t x = 0; x < ps.width; ++x) {
                    std::memcpy(dst + std::size_t{kPassX[p] + x * kPassDx[p]} * 4, passRow.data() + std::size_t{x} * 4, 4);
                }
            }
            prev = row;
            offset += 1 + rowBytes;
        }
    }
    return image;
}

void appendBe32(std::vector<std::byte> &out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((v >> shift) & 0xFFu));
    }
}

void appendChunk(std::vector<std::byte> &out, const char *type, Span<const std::byte> body) {
    appendBe32(out, static_cast<std::uint32_t>(body.size()));
    const std::size_t crcStart = out.size();
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>(type[i]));
    }
    out.insert(out.end(), body.begin(), body.end());
    appendBe32(out, crc32(Span<const std::byte>(out.data() + crcStart, out.size() - crcStart)));
}

Result<std::vector<std::byte>> encode(const Image &source, const PngEncodeOptions &options) {
    if (source.empty()) {
        return Error(ErrorCode::InvalidArgument, "png: cannot encode an empty image");
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
    const bool opaque = image->isOpaque();
    const std::size_t channels = opaque ? 3 : 4;
    const std::size_t rowBytes = std::size_t{image->width()} * channels;
    const std::uint32_t height = image->height();

    std::vector<std::byte> filtered(std::size_t{height} * (rowBytes + 1));
    std::vector<std::uint8_t> previous(rowBytes, 0);
    std::vector<std::uint8_t> current(rowBytes);
    std::array<std::vector<std::uint8_t>, 5> candidates;
    for (auto &c : candidates) {
        c.resize(rowBytes);
    }
    const bool adaptive = options.compressionLevel > 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        const Span<const std::uint8_t> src = image->row(y);
        if (opaque) {
            for (std::size_t x = 0; x < image->width(); ++x) {
                std::memcpy(current.data() + x * 3, src.data() + x * 4, 3);
            }
        } else {
            std::memcpy(current.data(), src.data(), rowBytes);
        }
        std::size_t best = 0;
        if (adaptive) {
            std::uint64_t bestCost = UINT64_MAX;
            for (std::size_t type = 0; type < 5; ++type) {
                std::uint8_t *out = candidates[type].data();
                std::uint64_t cost = 0;
                for (std::size_t i = 0; i < rowBytes; ++i) {
                    const int left = i >= channels ? current[i - channels] : 0;
                    const int up = y > 0 ? previous[i] : 0;
                    const int upLeft = (i >= channels && y > 0) ? previous[i - channels] : 0;
                    int predictor = 0;
                    switch (type) {
                    case 1: predictor = left; break;
                    case 2: predictor = up; break;
                    case 3: predictor = (left + up) >> 1; break;
                    case 4: predictor = paeth(left, up, upLeft); break;
                    default: break;
                    }
                    const auto v = static_cast<std::uint8_t>(current[i] - predictor);
                    out[i] = v;
                    cost += static_cast<std::uint64_t>(v < 128 ? v : 256 - v);
                }
                if (cost < bestCost) {
                    bestCost = cost;
                    best = type;
                }
            }
        } else {
            std::memcpy(candidates[0].data(), current.data(), rowBytes);
        }
        std::byte *dst = filtered.data() + std::size_t{y} * (rowBytes + 1);
        dst[0] = static_cast<std::byte>(best);
        std::memcpy(dst + 1, candidates[best].data(), rowBytes);
        previous.swap(current);
    }

    const std::vector<std::byte> compressed = zlibCompress(filtered, {options.compressionLevel});
    std::vector<std::byte> out;
    out.reserve(compressed.size() + 64);
    for (const std::uint8_t b : kSignature) {
        out.push_back(static_cast<std::byte>(b));
    }
    std::vector<std::byte> ihdr;
    appendBe32(ihdr, image->width());
    appendBe32(ihdr, height);
    ihdr.push_back(std::byte{8});
    ihdr.push_back(static_cast<std::byte>(opaque ? 2 : 6));
    ihdr.push_back(std::byte{0});
    ihdr.push_back(std::byte{0});
    ihdr.push_back(std::byte{0});
    appendChunk(out, "IHDR", ihdr);
    constexpr std::size_t kIdatChunk = 1u << 20;
    for (std::size_t at = 0; at < compressed.size(); at += kIdatChunk) {
        appendChunk(out, "IDAT",
                    Span<const std::byte>(compressed).subspan(at, std::min(kIdatChunk, compressed.size() - at)));
    }
    appendChunk(out, "IEND", {});
    return out;
}

} // namespace

bool isPng(Span<const std::byte> data) noexcept {
    return data.size() >= kSignature.size() && std::memcmp(data.data(), kSignature.data(), kSignature.size()) == 0;
}

Result<Image> decodePng(Span<const std::byte> data, const ImageLimits &limits) {
    try {
        return decode(data, limits);
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "png: out of memory");
    }
}

Result<std::vector<std::byte>> encodePng(const Image &image, const PngEncodeOptions &options) {
    try {
        return encode(image, options);
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "png: out of memory");
    }
}

} // namespace cfw
