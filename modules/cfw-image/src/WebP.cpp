// WebP container (RIFF, VP8X extended format, ALPH), per the WebP Container
// Specification; the bitstreams are decoded in WebPLossy.cpp and
// WebPLossless.cpp.

#include "cfw/image/WebP.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <string>

#include "WebPInternal.h"

namespace cfw {

namespace {

Error corrupt(const char *what) { return Error(ErrorCode::Corrupt, String("webp: ") + what); }

std::uint32_t le24(const std::uint8_t *p) noexcept {
    return p[0] | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16;
}
std::uint32_t le32(const std::uint8_t *p) noexcept { return le24(p) | static_cast<std::uint32_t>(p[3]) << 24; }

struct Chunk {
    const std::uint8_t *fourcc;
    const std::uint8_t *data;
    std::size_t size;
};

// Splits [p, end) into chunks. Sizes are checked; odd sizes are padded.
Result<std::vector<Chunk>> chunks(const std::uint8_t *p, const std::uint8_t *end) {
    std::vector<Chunk> out;
    while (end - p >= 8) {
        const std::size_t size = le32(p + 4);
        if (size > static_cast<std::size_t>(end - p) - 8) {
            return corrupt("chunk runs past the end of the file");
        }
        out.push_back({p, p + 8, size});
        p += 8 + size + (size & 1);
        if (p > end) {
            p = end; // a missing pad byte at the very end is tolerated
        }
    }
    return out;
}

bool is(const Chunk &c, const char *fourcc) noexcept { return std::memcmp(c.fourcc, fourcc, 4) == 0; }

// ALPH: a header byte, then raw or VP8L-compressed alpha, filtered by rows.
Result<std::vector<std::uint8_t>> decodeAlpha(const Chunk &chunk, std::uint32_t width, std::uint32_t height,
                                              const ImageLimits &limits) {
    if (chunk.size <= 1) {
        return corrupt("empty alpha chunk");
    }
    const unsigned header = chunk.data[0];
    const unsigned method = header & 3u;
    const unsigned filter = (header >> 2) & 3u;
    const unsigned preprocessing = (header >> 4) & 3u;
    if (method > 1 || preprocessing > 1 || (header >> 6) != 0) {
        return corrupt("invalid alpha header");
    }
    const std::size_t n = std::size_t{width} * height;
    std::vector<std::uint8_t> alpha(n);
    const Span<const std::byte> payload(reinterpret_cast<const std::byte *>(chunk.data + 1), chunk.size - 1);
    if (method == 0) {
        if (payload.size() < n) {
            return corrupt("alpha data is truncated");
        }
        std::memcpy(alpha.data(), payload.data(), n);
    } else {
        Result<webp::ArgbImage> image = webp::decodeLosslessStream(payload, width, height, limits);
        if (!image) {
            return std::move(image).error();
        }
        for (std::size_t i = 0; i < n; ++i) {
            alpha[i] = static_cast<std::uint8_t>((image.value().pixels[i] >> 8) & 0xFFu);
        }
    }
    // Unfilter row by row (dsp/filters.c): the first row is always
    // horizontal-predicted from 0.
    for (std::uint32_t y = 0; y < height && filter != 0; ++y) {
        std::uint8_t *row = alpha.data() + std::size_t{y} * width;
        const std::uint8_t *prev = y > 0 ? row - width : nullptr;
        if (filter == 1 || prev == nullptr) {
            std::uint8_t pred = prev != nullptr ? prev[0] : 0;
            for (std::uint32_t x = 0; x < width; ++x) {
                row[x] = static_cast<std::uint8_t>(row[x] + pred);
                pred = row[x];
            }
        } else if (filter == 2) {
            for (std::uint32_t x = 0; x < width; ++x) {
                row[x] = static_cast<std::uint8_t>(row[x] + prev[x]);
            }
        } else {
            std::uint8_t left = prev[0];
            std::uint8_t topLeft = prev[0];
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t top = prev[x];
                const int g = std::clamp(left + top - topLeft, 0, 255);
                left = static_cast<std::uint8_t>(row[x] + g);
                topLeft = top;
                row[x] = left;
            }
        }
    }
    return alpha;
}

// A frame: optional ALPH, then VP8 or VP8L.
Result<Image> decodeFrame(const std::vector<Chunk> &frame, bool allowAlphaChunk, const ImageLimits &limits) {
    const Chunk *alphaChunk = nullptr;
    for (const Chunk &c : frame) {
        if (is(c, "ALPH") && allowAlphaChunk && alphaChunk == nullptr) {
            alphaChunk = &c;
        } else if (is(c, "VP8L")) {
            Result<webp::ArgbImage> argb =
                webp::decodeLossless(Span<const std::byte>(reinterpret_cast<const std::byte *>(c.data), c.size), limits);
            if (!argb) {
                return std::move(argb).error();
            }
            const webp::ArgbImage &src = argb.value();
            Result<Image> image = Image::create(src.width, src.height, AlphaMode::Straight, limits);
            if (!image) {
                return image;
            }
            std::uint8_t *out = image.value().pixels().data();
            for (const std::uint32_t p : src.pixels) {
                *out++ = static_cast<std::uint8_t>(p >> 16);
                *out++ = static_cast<std::uint8_t>(p >> 8);
                *out++ = static_cast<std::uint8_t>(p);
                *out++ = static_cast<std::uint8_t>(p >> 24);
            }
            return image;
        } else if (is(c, "VP8 ")) {
            Result<webp::Rgba> rgba =
                webp::decodeLossy(Span<const std::byte>(reinterpret_cast<const std::byte *>(c.data), c.size), limits);
            if (!rgba) {
                return std::move(rgba).error();
            }
            webp::Rgba &src = rgba.value();
            Result<Image> image = Image::create(src.width, src.height, AlphaMode::Straight, limits);
            if (!image) {
                return image;
            }
            std::memcpy(image.value().pixels().data(), src.pixels.data(), src.pixels.size());
            if (alphaChunk != nullptr) {
                Result<std::vector<std::uint8_t>> alpha = decodeAlpha(*alphaChunk, src.width, src.height, limits);
                if (!alpha) {
                    return std::move(alpha).error();
                }
                std::uint8_t *px = image.value().pixels().data();
                for (const std::uint8_t a : alpha.value()) {
                    px[3] = a;
                    px += 4;
                }
            }
            return image;
        }
    }
    return corrupt("no image data");
}

Result<Image> decode(Span<const std::byte> input, const ImageLimits &limits) {
    if (input.size() > limits.maxInputBytes) {
        return Error(ErrorCode::LimitExceeded, "webp: file exceeds the size limit");
    }
    if (!isWebP(input)) {
        return corrupt("not a WebP file");
    }
    const auto *data = reinterpret_cast<const std::uint8_t *>(input.data());
    const std::size_t riffSize = le32(data + 4);
    if (riffSize < 12 || riffSize > input.size() - 8) {
        return corrupt("RIFF size is invalid or the file is truncated");
    }
    Result<std::vector<Chunk>> list = chunks(data + 12, data + 8 + riffSize);
    if (!list) {
        return std::move(list).error();
    }
    const std::vector<Chunk> &all = list.value();
    if (all.empty()) {
        return corrupt("no chunks");
    }
    if (!is(all[0], "VP8X")) {
        return decodeFrame(all, false, limits); // simple format: VP8 or VP8L first
    }
    const Chunk &vp8x = all[0];
    if (vp8x.size < 10) {
        return corrupt("VP8X chunk is too small");
    }
    const std::uint32_t flags = vp8x.data[0];
    const std::uint32_t canvasW = le24(vp8x.data + 4) + 1;
    const std::uint32_t canvasH = le24(vp8x.data + 7) + 1;
    if (Result<void> ok = checkImageSize(canvasW, canvasH, limits); !ok) {
        return std::move(ok).error();
    }
    if ((flags & 0x02u) == 0) {
        Result<Image> image = decodeFrame(all, true, limits);
        if (image && (image.value().width() != canvasW || image.value().height() != canvasH)) {
            return corrupt("image size differs from the canvas");
        }
        return image;
    }
    // Animation: the first ANMF frame on a transparent canvas.
    for (const Chunk &c : all) {
        if (!is(c, "ANMF")) {
            continue;
        }
        if (c.size < 16) {
            return corrupt("ANMF chunk is too small");
        }
        const std::uint32_t fx = le24(c.data) * 2;
        const std::uint32_t fy = le24(c.data + 3) * 2;
        const std::uint32_t fw = le24(c.data + 6) + 1;
        const std::uint32_t fh = le24(c.data + 9) + 1;
        if (std::uint64_t{fx} + fw > canvasW || std::uint64_t{fy} + fh > canvasH) {
            return corrupt("animation frame lies outside the canvas");
        }
        Result<std::vector<Chunk>> frameChunks = chunks(c.data + 16, c.data + c.size);
        if (!frameChunks) {
            return std::move(frameChunks).error();
        }
        Result<Image> frame = decodeFrame(frameChunks.value(), true, limits);
        if (!frame) {
            return frame;
        }
        if (frame.value().width() != fw || frame.value().height() != fh) {
            return corrupt("animation frame size differs from its header");
        }
        Result<Image> canvas = Image::create(canvasW, canvasH, AlphaMode::Straight, limits);
        if (!canvas) {
            return canvas;
        }
        for (std::uint32_t y = 0; y < fh; ++y) {
            std::memcpy(canvas.value().row(fy + y).data() + std::size_t{fx} * 4, frame.value().row(y).data(),
                        std::size_t{fw} * 4);
        }
        return canvas;
    }
    return corrupt("animation has no frames");
}

} // namespace

bool isWebP(Span<const std::byte> data) noexcept {
    return data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0 && std::memcmp(data.data() + 8, "WEBP", 4) == 0;
}

Result<Image> decodeWebP(Span<const std::byte> data, const ImageLimits &limits) {
    try {
        return decode(data, limits);
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "webp: out of memory");
    }
}

} // namespace cfw
