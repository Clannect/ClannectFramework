#include "cfw/gfx/RasterPaintBackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cfw/image/Image.h"

namespace cfw {

namespace {

// x * y / 255, rounded, for x, y in [0, 255].
inline std::uint32_t mul255(std::uint32_t x, std::uint32_t y) noexcept {
    const std::uint32_t t = x * y + 128u;
    return (t + (t >> 8)) >> 8;
}

inline std::uint8_t toByte(std::uint32_t v) noexcept { return static_cast<std::uint8_t>(std::min(v, 255u)); }

inline std::uint8_t unitToByte(float v) noexcept {
    if (!(v > 0.0f)) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::lround(std::min(v, 1.0f) * 255.0f));
}

// Premultiplied 8-bit RGBA of a straight sRGB colour.
std::array<std::uint8_t, 4> premultiplied(const Color &c) noexcept {
    const std::array<std::uint8_t, 4> s = c.toRgba8();
    return {static_cast<std::uint8_t>(mul255(s[0], s[3])), static_cast<std::uint8_t>(mul255(s[1], s[3])),
            static_cast<std::uint8_t>(mul255(s[2], s[3])), s[3]};
}

// ---- Blending: premultiplied source over destination, mixed by coverage. ----

// The per-channel result of each mode at full coverage.
template <BlendMode M>
inline std::uint32_t op(std::uint32_t s, std::uint32_t d, std::uint32_t sa, std::uint32_t da) noexcept {
    if constexpr (M == BlendMode::Source) {
        return s;
    } else if constexpr (M == BlendMode::DestinationIn) {
        return mul255(d, sa);
    } else if constexpr (M == BlendMode::DestinationOut) {
        return mul255(d, 255u - sa);
    } else if constexpr (M == BlendMode::Multiply) {
        return std::min((s * d + s * (255u - da) + d * (255u - sa) + 127u) / 255u, 255u);
    } else if constexpr (M == BlendMode::Screen) {
        return s + d - mul255(s, d);
    } else if constexpr (M == BlendMode::Plus) {
        return std::min(s + d, 255u);
    } else {
        return s + mul255(d, 255u - sa);
    }
}

// `step` is 4 for a row of source pixels, 0 for one solid colour.
template <BlendMode M>
void blendRow(std::uint8_t *d, const std::uint8_t *s, int step, const std::uint8_t *coverage, int count) {
    for (int i = 0; i < count; ++i, d += 4, s += step) {
        const std::uint32_t c = coverage[i];
        if (c == 0) {
            continue;
        }
        if constexpr (M == BlendMode::SourceOver) {
            std::uint32_t s0 = s[0];
            std::uint32_t s1 = s[1];
            std::uint32_t s2 = s[2];
            std::uint32_t sa = s[3];
            if (c != 255) {
                s0 = mul255(s0, c);
                s1 = mul255(s1, c);
                s2 = mul255(s2, c);
                sa = mul255(sa, c);
            }
            if (sa == 255) {
                d[0] = static_cast<std::uint8_t>(s0);
                d[1] = static_cast<std::uint8_t>(s1);
                d[2] = static_cast<std::uint8_t>(s2);
                d[3] = 255;
            } else {
                const std::uint32_t inv = 255u - sa;
                d[0] = toByte(s0 + mul255(d[0], inv));
                d[1] = toByte(s1 + mul255(d[1], inv));
                d[2] = toByte(s2 + mul255(d[2], inv));
                d[3] = toByte(sa + mul255(d[3], inv));
            }
        } else {
            const std::uint32_t sa = s[3];
            const std::uint32_t da = d[3];
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t r = op<M>(s[k], d[k], sa, da);
                d[k] = c == 255 ? toByte(r) : toByte(mul255(r, c) + mul255(d[k], 255u - c));
            }
        }
    }
}

using BlendFn = void (*)(std::uint8_t *, const std::uint8_t *, int, const std::uint8_t *, int);

BlendFn blendFor(BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::SourceOver: return blendRow<BlendMode::SourceOver>;
    case BlendMode::Source: return blendRow<BlendMode::Source>;
    case BlendMode::DestinationIn: return blendRow<BlendMode::DestinationIn>;
    case BlendMode::DestinationOut: return blendRow<BlendMode::DestinationOut>;
    case BlendMode::Multiply: return blendRow<BlendMode::Multiply>;
    case BlendMode::Screen: return blendRow<BlendMode::Screen>;
    case BlendMode::Plus: return blendRow<BlendMode::Plus>;
    }
    return blendRow<BlendMode::SourceOver>;
}

// The colour-table entry for gradient offset t. Spreading works on table
// indices, exactly as Qt's qt_gradient_pixel does: the index is
// int(t * 1023 + 0.5), and Repeat and Reflect wrap it modulo 1024 and 2048
// (so t = 1 is the last colour, not the first).
std::size_t gradientIndex(double t, GradientSpread spread) noexcept {
    if (!std::isfinite(t)) {
        return 0;
    }
    double x = t * 1023.0 + 0.5;
    if (std::abs(x) > 1e9) {
        x = std::fmod(x, 2048.0); // a whole number of periods of either spread
    }
    int i = static_cast<int>(x); // truncates towards zero, as Qt's cast does
    switch (spread) {
    case GradientSpread::Pad: i = std::clamp(i, 0, 1023); break;
    case GradientSpread::Repeat:
        i %= 1024;
        i = i < 0 ? i + 1024 : i;
        break;
    case GradientSpread::Reflect:
        i %= 2048;
        i = i < 0 ? i + 2048 : i;
        i = i >= 1024 ? 2047 - i : i;
        break;
    }
    return static_cast<std::size_t>(i);
}

int wrap(int i, int begin, int period) noexcept {
    int m = (i - begin) % period;
    if (m < 0) {
        m += period;
    }
    return begin + m;
}

} // namespace

// What fetch() reads: a gradient or an image, mapped from device pixels.
struct RasterPaintBackend::Source {
    SourceKind kind = SourceKind::Solid;
    std::array<std::uint8_t, 4> color{};
    Transform2D inverse; // device -> gradient or image space
    bool affine = true;
    GradientSpread spread = GradientSpread::Pad;
    // Linear: t = (u - start) . d / |d|^2.
    double sx = 0, sy = 0, dx = 0, dy = 0, invLength2 = 0;
    // Radial: focal f, centre - focal cd, radius r, a = cd.cd - r^2.
    double fx = 0, fy = 0, cdx = 0, cdy = 0, a = 0;
    bool degenerate = false;
    // Image.
    const Image *image = nullptr;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // texels [x0, x1) x [y0, y1)
    bool tile = false;
    bool smooth = true;
    bool straight = false;
    bool tinted = false;
    std::array<std::uint32_t, 4> tint{255, 255, 255, 255};
};

RasterPaintBackend::RasterPaintBackend(Image &target) : m_target(target) {
    if (m_target.alphaMode() != AlphaMode::Premultiplied) {
        m_target.premultiply();
    }
    m_width = static_cast<int>(m_target.width());
    m_height = static_cast<int>(m_target.height());
    m_coverage.resize(static_cast<std::size_t>(m_width));
    m_pixels.resize(static_cast<std::size_t>(m_width) * 4u);
}

Vec2i RasterPaintBackend::size() const noexcept { return {m_width, m_height}; }

Recti RasterPaintBackend::clipBox() const noexcept {
    return m_clips.empty() ? Recti{0, 0, m_width, m_height} : m_clips.back().box;
}

const std::uint8_t *RasterPaintBackend::clipMask() const noexcept {
    if (m_clips.empty() || m_clips.back().mask < 0) {
        return nullptr;
    }
    return m_masks[static_cast<std::size_t>(m_clips.back().mask)].data();
}

void RasterPaintBackend::buildGradientTable(const Brush &brush) {
    const Span<const GradientStop> stops = brush.stops();
    std::size_t k = 0;
    const auto premul = [](const Color &c) {
        const float a = std::clamp(c.a, 0.0f, 1.0f);
        return std::array<float, 4>{std::clamp(c.r, 0.0f, 1.0f) * a, std::clamp(c.g, 0.0f, 1.0f) * a,
                                    std::clamp(c.b, 0.0f, 1.0f) * a, a};
    };
    for (std::size_t i = 0; i < 1024; ++i) {
        const float t = static_cast<float>(i) / 1023.0f;
        while (k < stops.size() && stops[k].offset <= t) {
            ++k;
        }
        std::array<float, 4> c{};
        if (k == 0) {
            c = premul(stops.front().color);
        } else if (k == stops.size()) {
            c = premul(stops.back().color);
        } else {
            const GradientStop &lo = stops[k - 1];
            const GradientStop &hi = stops[k];
            const float span = hi.offset - lo.offset;
            const float f = span > 0.0f ? (t - lo.offset) / span : 1.0f;
            const std::array<float, 4> p = premul(lo.color);
            const std::array<float, 4> q = premul(hi.color);
            for (int j = 0; j < 4; ++j) {
                c[static_cast<std::size_t>(j)] = p[static_cast<std::size_t>(j)] +
                                                 (q[static_cast<std::size_t>(j)] - p[static_cast<std::size_t>(j)]) * f;
            }
        }
        for (std::size_t j = 0; j < 4; ++j) {
            m_gradient[i * 4 + j] = unitToByte(c[j]);
        }
    }
}

void RasterPaintBackend::fillPath(const PainterPath &path, const Transform2D &transform, FillRule rule, const Brush &brush,
                                  const Transform2D &brushTransform, const CompositeState &state) {
    if (brush.isNone() || clipBox().isEmpty()) {
        return;
    }
    Source source;
    switch (brush.kind()) {
    case Brush::Kind::None: return;
    case Brush::Kind::Solid:
        source.kind = SourceKind::Solid;
        source.color = premultiplied(brush.color());
        break;
    case Brush::Kind::LinearGradient:
    case Brush::Kind::RadialGradient: {
        const std::optional<Transform2D> inverse = brushTransform.inverse();
        if (!inverse) {
            return;
        }
        source.inverse = *inverse;
        source.affine = inverse->isAffine();
        source.spread = brush.spread();
        buildGradientTable(brush);
        if (brush.kind() == Brush::Kind::LinearGradient) {
            source.kind = SourceKind::Linear;
            source.sx = brush.start().x;
            source.sy = brush.start().y;
            source.dx = static_cast<double>(brush.end().x) - brush.start().x;
            source.dy = static_cast<double>(brush.end().y) - brush.start().y;
            const double l2 = source.dx * source.dx + source.dy * source.dy;
            source.invLength2 = l2 > 0.0 ? 1.0 / l2 : 0.0;
        } else {
            source.kind = SourceKind::Radial;
            source.fx = brush.focal().x;
            source.fy = brush.focal().y;
            source.cdx = static_cast<double>(brush.center().x) - brush.focal().x;
            source.cdy = static_cast<double>(brush.center().y) - brush.focal().y;
            const double r = brush.radius();
            source.a = source.cdx * source.cdx + source.cdy * source.cdy - r * r;
            source.degenerate = !(r > 0.0);
        }
        break;
    }
    }
    m_rasterizer.reset(m_width, m_height);
    m_rasterizer.addPath(path, transform);
    paint(source, rule, state);
}

void RasterPaintBackend::drawImage(const Image &image, const RectF &source, const RectF &target,
                                   const Transform2D &transform, const ImageOptions &options,
                                   const CompositeState &state) {
    if (image.empty() || clipBox().isEmpty() || !(target.width > 0.0f) || !(target.height > 0.0f) ||
        !(source.width > 0.0f) || !(source.height > 0.0f)) {
        return;
    }
    Source s;
    s.kind = SourceKind::Image;
    s.image = &image;
    s.smooth = options.smooth;
    s.straight = image.alphaMode() != AlphaMode::Premultiplied;
    const int w = static_cast<int>(image.width());
    const int h = static_cast<int>(image.height());
    const auto finiteRect = [](const RectF &r) {
        return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.right()) && std::isfinite(r.bottom());
    };
    if (!finiteRect(source) || !finiteRect(target) || !std::isfinite(options.tileSize.x) ||
        !std::isfinite(options.tileSize.y)) {
        return;
    }
    // Clamped as doubles first: a float outside int's range must not be cast.
    const auto texel = [](double v, int limit) { return static_cast<int>(std::clamp(v, 0.0, static_cast<double>(limit))); };
    s.x0 = texel(std::floor(source.x), w);
    s.y0 = texel(std::floor(source.y), h);
    s.x1 = texel(std::ceil(source.right()), w);
    s.y1 = texel(std::ceil(source.bottom()), h);
    if (s.x1 <= s.x0 || s.y1 <= s.y0) {
        return;
    }
    s.tile = options.tileSize.x > 0.0f && options.tileSize.y > 0.0f;
    // Image space -> target space.
    const double scaleX = s.tile ? options.tileSize.x / static_cast<double>(s.x1 - s.x0) : target.width / source.width;
    const double scaleY = s.tile ? options.tileSize.y / static_cast<double>(s.y1 - s.y0) : target.height / source.height;
    const double originX = s.tile ? static_cast<double>(s.x0) : static_cast<double>(source.x);
    const double originY = s.tile ? static_cast<double>(s.y0) : static_cast<double>(source.y);
    const Transform2D toTarget = Transform2D::translation(target.x, target.y) * Transform2D::scaling(scaleX, scaleY) *
                                 Transform2D::translation(-originX, -originY);
    const std::optional<Transform2D> inverse = (transform * toTarget).inverse();
    if (!inverse) {
        return;
    }
    s.inverse = *inverse;
    s.affine = inverse->isAffine();
    const Color &t = options.tint;
    if (!(t.r >= 1.0f && t.g >= 1.0f && t.b >= 1.0f && t.a >= 1.0f)) {
        const std::array<std::uint8_t, 4> p = premultiplied(t);
        s.tint = {p[0], p[1], p[2], p[3]};
        s.tinted = true;
    }
    m_scratch.clear();
    m_scratch.addRect(target);
    m_rasterizer.reset(m_width, m_height);
    m_rasterizer.addPath(m_scratch, transform);
    paint(s, FillRule::NonZero, state);
}

void RasterPaintBackend::fetch(const Source &s, int y, int x, int count, std::uint8_t *out) const {
    // Pixel centres mapped back: incrementally for affine maps.
    const Transform2D &m = s.inverse;
    const double px = x + 0.5;
    const double py = y + 0.5;
    double u = m(0, 0) * px + m(0, 1) * py + m(0, 2);
    double v = m(1, 0) * px + m(1, 1) * py + m(1, 2);
    double w = m(2, 0) * px + m(2, 1) * py + m(2, 2);
    const double du = m(0, 0);
    const double dv = m(1, 0);
    const double dw = m(2, 0);

    for (int i = 0; i < count; ++i, u += du, v += dv, w += dw, out += 4) {
        double gx = u;
        double gy = v;
        if (!s.affine) {
            if (!(std::abs(w) > 1e-12)) {
                std::memset(out, 0, 4);
                continue;
            }
            gx = u / w;
            gy = v / w;
        }
        if (s.kind == SourceKind::Linear || s.kind == SourceKind::Radial) {
            double t = 0.0;
            if (s.kind == SourceKind::Linear) {
                t = ((gx - s.sx) * s.dx + (gy - s.sy) * s.dy) * s.invLength2;
            } else if (s.degenerate) {
                t = 1.0;
            } else {
                // Smallest circle (focal + t cd, radius t r) through the point.
                const double pdx = gx - s.fx;
                const double pdy = gy - s.fy;
                const double b = pdx * s.cdx + pdy * s.cdy;
                const double c = pdx * pdx + pdy * pdy;
                const double disc = std::max(b * b - s.a * c, 0.0);
                t = (b - std::sqrt(disc)) / s.a;
            }
            std::memcpy(out, m_gradient.data() + gradientIndex(t, s.spread) * 4, 4);
            continue;
        }
        // Image sampling.
        const Image &img = *s.image;
        const std::size_t stride = img.stride();
        const std::uint8_t *base = img.pixels().data();
        const int periodX = s.x1 - s.x0;
        const int periodY = s.y1 - s.y0;
        const auto clampOrWrapX = [&](int i0) {
            return s.tile ? wrap(i0, s.x0, periodX) : std::clamp(i0, s.x0, s.x1 - 1);
        };
        const auto clampOrWrapY = [&](int i0) {
            return s.tile ? wrap(i0, s.y0, periodY) : std::clamp(i0, s.y0, s.y1 - 1);
        };
        const auto texel = [&](int tx, int ty, std::uint32_t *p) {
            const std::uint8_t *q =
                base + static_cast<std::size_t>(ty) * stride + static_cast<std::size_t>(tx) * 4u;
            if (s.straight) {
                p[0] = mul255(q[0], q[3]);
                p[1] = mul255(q[1], q[3]);
                p[2] = mul255(q[2], q[3]);
            } else {
                p[0] = q[0];
                p[1] = q[1];
                p[2] = q[2];
            }
            p[3] = q[3];
        };
        if (!std::isfinite(gx) || !std::isfinite(gy) || std::abs(gx) > 1e9 || std::abs(gy) > 1e9) {
            std::memset(out, 0, 4);
            continue;
        }
        std::uint32_t r[4];
        if (!s.smooth) {
            texel(clampOrWrapX(static_cast<int>(std::floor(gx))), clampOrWrapY(static_cast<int>(std::floor(gy))), r);
        } else {
            const double fxd = gx - 0.5;
            const double fyd = gy - 0.5;
            const double flx = std::floor(fxd);
            const double fly = std::floor(fyd);
            const auto fx = static_cast<std::uint32_t>((fxd - flx) * 256.0);
            const auto fy = static_cast<std::uint32_t>((fyd - fly) * 256.0);
            const int ix = static_cast<int>(flx);
            const int iy = static_cast<int>(fly);
            const int ax = clampOrWrapX(ix);
            const int bx = clampOrWrapX(ix + 1);
            const int ay = clampOrWrapY(iy);
            const int by = clampOrWrapY(iy + 1);
            std::uint32_t p00[4], p10[4], p01[4], p11[4];
            texel(ax, ay, p00);
            texel(bx, ay, p10);
            texel(ax, by, p01);
            texel(bx, by, p11);
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t top = p00[k] * (256u - fx) + p10[k] * fx;
                const std::uint32_t bottom = p01[k] * (256u - fx) + p11[k] * fx;
                r[k] = (top * (256u - fy) + bottom * fy + 32768u) >> 16;
            }
        }
        if (s.tinted) {
            for (int k = 0; k < 4; ++k) {
                r[k] = mul255(r[k], s.tint[static_cast<std::size_t>(k)]);
            }
        }
        for (int k = 0; k < 4; ++k) {
            out[k] = toByte(r[k]);
        }
    }
}

void RasterPaintBackend::paint(const Source &source, FillRule rule, const CompositeState &state) {
    const int opacity = static_cast<int>(std::lround(std::clamp(state.opacity, 0.0f, 1.0f) * 256.0f));
    if (opacity == 0) {
        return;
    }
    const Recti box = clipBox();
    const std::uint8_t *mask = clipMask();
    const BlendFn blend = blendFor(state.blend);
    std::uint8_t *pixels = m_target.pixels().data();
    const std::size_t stride = m_target.stride();
    m_rasterizer.sweep(rule, [&](int y, int x, Span<const std::uint8_t> coverage) {
        if (y < box.y || y >= box.bottom()) {
            return;
        }
        const int begin = std::max(x, box.x);
        const int end = std::min(x + static_cast<int>(coverage.size()), box.right());
        if (begin >= end) {
            return;
        }
        const int count = end - begin;
        const std::uint8_t *c = coverage.data() + (begin - x);
        if (mask != nullptr || opacity != 256) {
            const std::uint8_t *m = mask != nullptr ? mask + static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) +
                                                          static_cast<std::size_t>(begin)
                                                    : nullptr;
            for (int i = 0; i < count; ++i) {
                std::uint32_t v = c[i];
                if (m != nullptr) {
                    v = mul255(v, m[i]);
                }
                m_coverage[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v * static_cast<std::uint32_t>(opacity)) >> 8);
            }
            c = m_coverage.data();
        }
        std::uint8_t *d = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(begin) * 4u;
        if (source.kind == SourceKind::Solid) {
            blend(d, source.color.data(), 0, c, count);
        } else {
            fetch(source, y, begin, count, m_pixels.data());
            blend(d, m_pixels.data(), 4, c, count);
        }
    });
}

void RasterPaintBackend::pushClipRect(const RectF &rect, const Transform2D &transform) {
    if (transform.isAffine() && transform(0, 1) == 0.0 && transform(1, 0) == 0.0) {
        // Axis-aligned: whole pixels, each edge rounded to the nearest pixel
        // boundary (Qt does the same, anti-aliasing or not).
        const RectF r = transform.mapRect(rect);
        const auto edge = [](float v) {
            return static_cast<int>(std::clamp(std::floor(static_cast<double>(v) + 0.5), -1e9, 1e9));
        };
        Recti box{};
        if (std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.right()) && std::isfinite(r.bottom())) {
            const int left = edge(r.x);
            const int top = edge(r.y);
            box = Recti{left, top, edge(r.right()) - left, edge(r.bottom()) - top}.intersected(clipBox());
        } // a clip rectangle with non-finite edges clips everything
        const int mask = m_clips.empty() ? -1 : m_clips.back().mask;
        m_clips.push_back({box, mask});
        return;
    }
    m_scratch.clear();
    m_scratch.addRect(rect);
    pushClipPath(m_scratch, transform, FillRule::NonZero);
}

void RasterPaintBackend::pushClipPath(const PainterPath &path, const Transform2D &transform, FillRule rule) {
    const Recti outer = clipBox();
    const std::uint8_t *previous = clipMask();
    const int slot = m_clips.empty() || m_clips.back().mask < 0 ? 0 : m_clips.back().mask + 1;
    if (m_masks.size() <= static_cast<std::size_t>(slot)) {
        m_masks.resize(static_cast<std::size_t>(slot) + 1);
    }
    std::vector<std::uint8_t> &mask = m_masks[static_cast<std::size_t>(slot)];
    mask.resize(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height));
    for (int y = outer.y; y < outer.bottom(); ++y) {
        std::memset(mask.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) +
                        static_cast<std::size_t>(outer.x),
                    0, static_cast<std::size_t>(outer.width));
    }
    Recti covered{};
    if (!outer.isEmpty()) {
        m_rasterizer.reset(m_width, m_height);
        m_rasterizer.addPath(path, transform);
        int minX = outer.right();
        int maxX = outer.x;
        int minY = outer.bottom();
        int maxY = outer.y;
        m_rasterizer.sweep(rule, [&](int y, int x, Span<const std::uint8_t> coverage) {
            if (y < outer.y || y >= outer.bottom()) {
                return;
            }
            const int begin = std::max(x, outer.x);
            const int end = std::min(x + static_cast<int>(coverage.size()), outer.right());
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width);
            for (int i = begin; i < end; ++i) {
                std::uint32_t v = coverage[static_cast<std::size_t>(i - x)];
                if (previous != nullptr) {
                    v = mul255(v, previous[row + static_cast<std::size_t>(i)]);
                }
                if (v != 0) {
                    mask[row + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v);
                    minX = std::min(minX, i);
                    maxX = std::max(maxX, i + 1);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y + 1);
                }
            }
        });
        if (minX < maxX && minY < maxY) {
            covered = Recti{minX, minY, maxX - minX, maxY - minY};
        }
    }
    m_clips.push_back({covered, slot});
}

void RasterPaintBackend::popClip() {
    if (!m_clips.empty()) {
        m_clips.pop_back();
    }
}

} // namespace cfw
