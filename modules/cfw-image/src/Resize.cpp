#include "cfw/image/Resize.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <numbers>
#include <vector>

namespace cfw {

namespace {

// For each output coordinate, the source indices and weights to combine.
struct Contributions {
    std::vector<std::uint32_t> start; // first source index per output
    std::vector<std::uint32_t> count;
    std::vector<std::uint32_t> offset; // into weights
    std::vector<float> weights;
};

double sinc(double x) noexcept {
    if (std::abs(x) < 1e-8) {
        return 1.0;
    }
    x *= std::numbers::pi;
    return std::sin(x) / x;
}

Contributions contributions(std::uint32_t srcSize, std::uint32_t dstSize, ResizeFilter filter) {
    Contributions c;
    c.start.resize(dstSize);
    c.count.resize(dstSize);
    c.offset.resize(dstSize);
    const double scale = static_cast<double>(srcSize) / dstSize;
    const double stretch = std::max(scale, 1.0); // widen the kernel when shrinking
    const double radius = filter == ResizeFilter::Lanczos3 ? 3.0 : 1.0;
    std::vector<double> w;
    for (std::uint32_t x = 0; x < dstSize; ++x) {
        w.clear();
        std::int64_t first = 0;
        if (filter == ResizeFilter::Box) {
            // Exact overlap of each source pixel [i, i+1) with [x, x+1) * scale.
            const double lo = x * scale;
            const double hi = (x + 1) * scale;
            first = static_cast<std::int64_t>(std::floor(lo));
            const auto last = std::min<std::int64_t>(static_cast<std::int64_t>(std::ceil(hi)) - 1, srcSize - 1);
            for (std::int64_t i = first; i <= last; ++i) {
                w.push_back(std::min(hi, static_cast<double>(i + 1)) - std::max(lo, static_cast<double>(i)));
            }
        } else {
            const double center = (x + 0.5) * scale - 0.5;
            const double support = radius * stretch;
            first = static_cast<std::int64_t>(std::ceil(center - support));
            const auto last = static_cast<std::int64_t>(std::floor(center + support));
            // Taps beyond an edge fold onto the edge pixel (clamp to edge).
            std::vector<double> folded(static_cast<std::size_t>(last - first + 1), 0.0);
            for (std::int64_t i = first; i <= last; ++i) {
                const double t = (static_cast<double>(i) - center) / stretch;
                double k = 0;
                if (filter == ResizeFilter::Bilinear) {
                    k = std::max(0.0, 1.0 - std::abs(t));
                } else if (std::abs(t) < 3.0) {
                    k = sinc(t) * sinc(t / 3.0);
                }
                folded[static_cast<std::size_t>(i - first)] = k;
            }
            const std::int64_t lo = std::max<std::int64_t>(first, 0);
            const std::int64_t hi = std::min<std::int64_t>(last, srcSize - 1);
            w.assign(static_cast<std::size_t>(hi - lo + 1), 0.0);
            for (std::int64_t i = first; i <= last; ++i) {
                const std::int64_t target = std::clamp<std::int64_t>(i, lo, hi);
                w[static_cast<std::size_t>(target - lo)] += folded[static_cast<std::size_t>(i - first)];
            }
            first = lo;
        }
        double sum = 0;
        for (const double v : w) {
            sum += v;
        }
        c.start[x] = static_cast<std::uint32_t>(first);
        c.count[x] = static_cast<std::uint32_t>(w.size());
        c.offset[x] = static_cast<std::uint32_t>(c.weights.size());
        for (const double v : w) {
            c.weights.push_back(static_cast<float>(sum != 0 ? v / sum : 0));
        }
    }
    return c;
}

// Filters `count` lines of `len` pixels. src/dst are premultiplied float
// RGBA; `srcStep`/`dstStep` are the distances between neighbouring pixels
// along the filtered axis, `srcLine`/`dstLine` between lines.
void filterAxis(const float *src, std::size_t srcStep, std::size_t srcLine, float *dst, std::size_t dstStep,
                std::size_t dstLine, std::size_t lines, const Contributions &c) {
    for (std::size_t line = 0; line < lines; ++line) {
        const float *in = src + line * srcLine;
        float *out = dst + line * dstLine;
        for (std::size_t x = 0; x < c.start.size(); ++x) {
            float acc[4] = {0, 0, 0, 0};
            const float *w = c.weights.data() + c.offset[x];
            const float *p = in + std::size_t{c.start[x]} * srcStep;
            for (std::uint32_t k = 0; k < c.count[x]; ++k, p += srcStep) {
                acc[0] += w[k] * p[0];
                acc[1] += w[k] * p[1];
                acc[2] += w[k] * p[2];
                acc[3] += w[k] * p[3];
            }
            float *o = out + x * dstStep;
            o[0] = acc[0];
            o[1] = acc[1];
            o[2] = acc[2];
            o[3] = acc[3];
        }
    }
}

std::uint8_t toByte(float v) noexcept { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

Result<Image> resize(const Image &source, std::uint32_t width, std::uint32_t height, ResizeFilter filter,
                     const ImageLimits &limits) {
    if (source.empty() || width == 0 || height == 0) {
        return Error(ErrorCode::InvalidArgument, "resize: empty source or zero target size");
    }
    Result<Image> created = Image::create(width, height, source.alphaMode(), limits);
    if (!created) {
        return created;
    }
    Image out = std::move(created).value();
    const std::size_t sw = source.width();
    const std::size_t sh = source.height();
    const bool straight = source.alphaMode() == AlphaMode::Straight;

    // Premultiplied float source.
    const std::size_t floats = sw * sh * 4;
    if (floats > limits.maxDecodedBytes) {
        return Error(ErrorCode::LimitExceeded, "resize: working buffer would exceed the limit");
    }
    std::vector<float> in(floats);
    const std::uint8_t *px = source.pixels().data();
    for (std::size_t i = 0; i < sw * sh; ++i, px += 4) {
        const float a = px[3];
        const float f = straight ? a / 255.0f : 1.0f;
        in[i * 4 + 0] = px[0] * f;
        in[i * 4 + 1] = px[1] * f;
        in[i * 4 + 2] = px[2] * f;
        in[i * 4 + 3] = a;
    }
    const Contributions cx = contributions(static_cast<std::uint32_t>(sw), width, filter);
    const Contributions cy = contributions(static_cast<std::uint32_t>(sh), height, filter);
    std::vector<float> result(std::size_t{width} * height * 4);
    // Whichever order needs the smaller intermediate.
    if (std::size_t{width} * sh <= sw * height) {
        std::vector<float> tmp(std::size_t{width} * sh * 4);
        filterAxis(in.data(), 4, sw * 4, tmp.data(), 4, std::size_t{width} * 4, sh, cx);
        filterAxis(tmp.data(), std::size_t{width} * 4, 4, result.data(), std::size_t{width} * 4, 4, width, cy);
    } else {
        std::vector<float> tmp(sw * height * 4);
        filterAxis(in.data(), sw * 4, 4, tmp.data(), sw * 4, 4, sw, cy);
        filterAxis(tmp.data(), 4, sw * 4, result.data(), 4, std::size_t{width} * 4, height, cx);
    }
    std::uint8_t *o = out.pixels().data();
    for (std::size_t i = 0; i < std::size_t{width} * height; ++i, o += 4) {
        const float *r = result.data() + i * 4;
        const std::uint8_t a = toByte(r[3]);
        o[3] = a;
        if (a == 0) {
            o[0] = o[1] = o[2] = 0;
            continue;
        }
        // Premultiplied colour cannot exceed its alpha.
        const float limit = straight ? 255.0f : static_cast<float>(a);
        const float f = straight ? 255.0f / std::max(r[3], 1e-6f) : 1.0f;
        for (std::size_t ch = 0; ch < 3; ++ch) {
            o[ch] = toByte(std::min(r[ch] * f, limit));
        }
    }
    return out;
}

} // namespace

Result<Image> resizeImage(const Image &source, std::uint32_t width, std::uint32_t height, ResizeFilter filter,
                          const ImageLimits &limits) {
    try {
        return resize(source, width, height, filter, limits);
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "resize: out of memory");
    }
}

} // namespace cfw
