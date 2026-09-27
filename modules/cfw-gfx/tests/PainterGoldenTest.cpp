// Golden-image tests (spec §9): every scene in testdata/scenes is rendered
// through Painter and the CPU backend and compared with Qt 6.11's rendering
// of the same scene (testdata/golden, made by testing/qt-oracle). The
// comparison is in premultiplied colour, per channel, within the tolerance
// each scene states. On a mismatch the rendering and a difference image are
// written next to the test binary.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/image/Png.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/Json.h"
#include "cfw/io/JsonReader.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;

namespace {

const Path kData(CFW_GFX_TESTDATA);
const Path kOutput(CFW_GFX_OUTPUT);

float num(const JsonValue &v, double fallback = 0.0) { return static_cast<float>(v.toDouble(fallback)); }

Color color(const JsonValue &c) {
    return Color::fromRgba8(static_cast<std::uint8_t>(c[0].toDouble(0)), static_cast<std::uint8_t>(c[1].toDouble(0)),
                            static_cast<std::uint8_t>(c[2].toDouble(0)), static_cast<std::uint8_t>(c[3].toDouble(255)));
}

RectF rect(const JsonValue &r) { return {num(r[0]), num(r[1]), num(r[2]), num(r[3])}; }

PainterPath path(const JsonValue &commands) {
    PainterPath p;
    if (const JsonArray *list = commands.asArray()) {
        for (const JsonValue &c : *list) {
            const StringView k = c[0].toString("");
            const auto a = [&](std::size_t i) { return num(c[i + 1]); };
            if (k == "M") {
                p.moveTo({a(0), a(1)});
            } else if (k == "L") {
                p.lineTo({a(0), a(1)});
            } else if (k == "Q") {
                p.quadTo({a(0), a(1)}, {a(2), a(3)});
            } else if (k == "C") {
                p.cubicTo({a(0), a(1)}, {a(2), a(3)}, {a(4), a(5)});
            } else if (k == "Z") {
                p.close();
            } else if (k == "rect") {
                p.addRect({a(0), a(1), a(2), a(3)});
            } else if (k == "rrect") {
                p.addRoundedRect({a(0), a(1), a(2), a(3)}, a(4), a(5));
            } else if (k == "ellipse") {
                p.addEllipse({a(0), a(1), a(2), a(3)});
            } else if (k == "arc") {
                p.arcTo({a(0), a(1), a(2), a(3)}, a(4), a(5));
            } else if (k == "poly") {
                std::vector<Vec2> points;
                for (std::size_t i = 0; i + 1 < c.asArray()->size() - 1; i += 2) {
                    points.push_back({a(i), a(i + 1)});
                }
                p.addPolygon(points);
            } else {
                check(false, "unknown path command");
            }
        }
    }
    return p;
}

FillRule rule(const JsonValue &op) {
    return op["rule"].toString("nonzero") == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
}

Brush brush(const JsonValue &b) {
    if (!b["color"].isNull()) {
        return color(b["color"]);
    }
    std::vector<GradientStop> stops;
    if (const JsonArray *list = b["stops"].asArray()) {
        for (const JsonValue &s : *list) {
            stops.push_back({num(s[0]), Color::fromRgba8(static_cast<std::uint8_t>(s[1].toDouble(0)),
                                                         static_cast<std::uint8_t>(s[2].toDouble(0)),
                                                         static_cast<std::uint8_t>(s[3].toDouble(0)),
                                                         static_cast<std::uint8_t>(s[4].toDouble(255)))});
        }
    }
    const StringView s = b["spread"].toString("pad");
    const GradientSpread spread = s == "repeat"    ? GradientSpread::Repeat
                                  : s == "reflect" ? GradientSpread::Reflect
                                                   : GradientSpread::Pad;
    if (!b["linear"].isNull()) {
        const JsonValue &l = b["linear"];
        return Brush::linearGradient({num(l[0]), num(l[1])}, {num(l[2]), num(l[3])}, stops, spread);
    }
    const JsonValue &r = b["radial"];
    const Vec2 center{num(r[0]), num(r[1])};
    const JsonValue &f = b["focal"];
    const Vec2 focal = f.isNull() ? center : Vec2{num(f[0]), num(f[1])};
    return Brush::radialGradient(center, num(r[2]), focal, stops, spread);
}

Pen pen(const JsonValue &p) {
    Pen out;
    out.brush = !p["brush"].isNull() ? brush(p["brush"]) : Brush(p["color"].isNull() ? Color{0, 0, 0, 1} : color(p["color"]));
    out.width = num(p["width"], 1.0);
    const StringView cap = p["cap"].toString("square");
    out.cap = cap == "flat" ? CapStyle::Flat : cap == "round" ? CapStyle::Round : CapStyle::Square;
    const StringView join = p["join"].toString("bevel");
    out.join = join == "miter"      ? JoinStyle::Miter
               : join == "svgmiter" ? JoinStyle::SvgMiter
               : join == "round"    ? JoinStyle::Round
                                    : JoinStyle::Bevel;
    out.miterLimit = num(p["miter"], 2.0);
    if (const JsonArray *d = p["dashes"].asArray()) {
        for (const JsonValue &v : *d) {
            out.dashes.push_back(num(v));
        }
    }
    out.dashOffset = num(p["offset"], 0.0);
    out.cosmetic = p["cosmetic"].toBool(false);
    return out;
}

BlendMode blend(StringView mode) {
    if (mode == "source") {
        return BlendMode::Source;
    }
    if (mode == "destination-in") {
        return BlendMode::DestinationIn;
    }
    if (mode == "destination-out") {
        return BlendMode::DestinationOut;
    }
    if (mode == "multiply") {
        return BlendMode::Multiply;
    }
    if (mode == "screen") {
        return BlendMode::Screen;
    }
    if (mode == "plus") {
        return BlendMode::Plus;
    }
    return BlendMode::SourceOver;
}

// Straight RGBA test images; testing/qt-oracle/render_scenes.py makes the same ones.
Image testImage(StringView name) {
    std::uint32_t w = 4;
    std::uint32_t h = 4;
    if (name == "photo") {
        w = 32;
        h = 24;
    } else if (name == "alpha") {
        w = 16;
        h = 16;
    }
    Image img = std::move(Image::create(w, h).value());
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t *row = img.row(y).data();
        for (std::uint32_t x = 0; x < w; ++x) {
            std::array<std::uint32_t, 4> p{};
            if (name == "photo") {
                p = {(x * 8) % 256, (y * 10) % 256, (x * y) % 256, 255};
            } else if (name == "alpha") {
                p = {255, 128, x * 16, y * 16 + 15};
            } else {
                p = (x + y) % 2 ? std::array<std::uint32_t, 4>{0, 0, 255, 255}
                                : std::array<std::uint32_t, 4>{255, 255, 0, 128};
            }
            for (std::size_t k = 0; k < 4; ++k) {
                row[x * 4 + k] = static_cast<std::uint8_t>(p[k]);
            }
        }
    }
    return img;
}

void run(const JsonValue &scene, Painter &p) {
    const JsonArray *ops = scene["ops"].asArray();
    if (ops == nullptr) {
        return;
    }
    for (const JsonValue &op : *ops) {
        const StringView k = op["op"].toString("");
        if (k == "save") {
            p.save();
        } else if (k == "restore") {
            p.restore();
        } else if (k == "translate") {
            p.translate(num(op["d"][0]), num(op["d"][1]));
        } else if (k == "scale") {
            p.scale(num(op["s"][0]), num(op["s"][1]));
        } else if (k == "rotate") {
            p.rotate(num(op["deg"]));
        } else if (k == "transform") {
            const JsonValue &m = op["m"];
            p.setTransform(Transform2D::fromRows(m[0].toDouble(1), m[1].toDouble(0), m[2].toDouble(0), m[3].toDouble(0),
                                                 m[4].toDouble(1), m[5].toDouble(0)),
                           true);
        } else if (k == "quad") {
            std::array<Vec2, 4> from{};
            std::array<Vec2, 4> to{};
            for (std::size_t i = 0; i < 4; ++i) {
                from[i] = {num(op["from"][i][0]), num(op["from"][i][1])};
                to[i] = {num(op["to"][i][0]), num(op["to"][i][1])};
            }
            const std::optional<Transform2D> q = Transform2D::quadToQuad(from, to);
            check(q.has_value(), "the scene's quad transform exists");
            p.setTransform(q.value_or(Transform2D{}), true);
        } else if (k == "opacity") {
            p.setOpacity(num(op["v"], 1.0));
        } else if (k == "blend") {
            p.setBlendMode(blend(op["mode"].toString("")));
        } else if (k == "clipRect") {
            p.clipRect(rect(op["r"]));
        } else if (k == "clipPath") {
            p.clipPath(path(op["path"]), rule(op));
        } else if (k == "fill") {
            p.fillPath(path(op["path"]), brush(op["brush"]), rule(op));
        } else if (k == "fillRect") {
            p.fillRect(rect(op["r"]), brush(op["brush"]));
        } else if (k == "stroke") {
            p.strokePath(path(op["path"]), pen(op["pen"]));
        } else if (k == "image") {
            const Image img = testImage(op["image"].toString(""));
            ImageOptions options;
            options.smooth = op["smooth"].toBool(true);
            if (!op["tint"].isNull()) {
                options.tint = color(op["tint"]);
            }
            const RectF whole{0, 0, static_cast<float>(img.width()), static_cast<float>(img.height())};
            if (op["tile"].toBool(false)) {
                options.tileSize = {whole.width, whole.height};
            }
            p.drawImage(rect(op["target"]), img, op["source"].isNull() ? whole : rect(op["source"]), options);
        } else {
            check(false, "unknown scene operation");
        }
    }
}

void writePng(const Image &img, const Path &file) {
    const Result<std::vector<std::byte>> png = encodePng(img);
    if (png) {
        (void)writeFileAtomic(file, png.value());
    }
}

void compare(StringView name) {
    const Result<std::vector<std::byte>> text = readFile(kData / "scenes" / (String(name) + ".json"));
    check(text.ok(), "scene file reads");
    if (!text) {
        return;
    }
    const Result<JsonValue> scene =
        parseJson(StringView(reinterpret_cast<const char *>(text.value().data()), text.value().size()));
    check(scene.ok(), "scene parses");
    if (!scene) {
        return;
    }
    const JsonValue &sc = scene.value();
    const auto width = static_cast<std::uint32_t>(sc["size"][0].toDouble(0));
    const auto height = static_cast<std::uint32_t>(sc["size"][1].toDouble(0));
    Image canvas = std::move(Image::create(width, height, AlphaMode::Premultiplied).value());
    {
        const Color bg = sc["background"].isNull() ? Color{0, 0, 0, 0} : color(sc["background"]);
        const std::array<std::uint8_t, 4> s = bg.toRgba8();
        const std::array<std::uint8_t, 4> pre{static_cast<std::uint8_t>((s[0] * s[3] + 127) / 255),
                                              static_cast<std::uint8_t>((s[1] * s[3] + 127) / 255),
                                              static_cast<std::uint8_t>((s[2] * s[3] + 127) / 255), s[3]};
        Span<std::uint8_t> px = canvas.pixels();
        for (std::size_t i = 0; i < px.size(); ++i) {
            px[i] = pre[i % 4];
        }
        RasterPaintBackend backend(canvas);
        Painter painter(backend);
        run(sc, painter);
    }

    const Result<std::vector<std::byte>> goldenFile = readFile(kData / "golden" / (String(name) + ".png"));
    check(goldenFile.ok(), "golden image reads");
    if (!goldenFile) {
        return;
    }
    Result<Image> decoded = decodePng(goldenFile.value());
    check(decoded.ok() && decoded.value().width() == width && decoded.value().height() == height,
          "golden image decodes");
    if (!decoded || decoded.value().width() != width || decoded.value().height() != height) {
        return;
    }
    Image &golden = decoded.value();
    golden.premultiply();

    const JsonValue &tol = sc["tolerance"];
    const int maxAllowed = static_cast<int>(tol["max"].toDouble(8));
    const int over = static_cast<int>(tol["over"].toDouble(8));
    const double fraction = tol["fraction"].toDouble(0.0);
    const Span<const std::uint8_t> a = canvas.pixels();
    const Span<const std::uint8_t> b = golden.pixels();
    int worst = 0;
    std::size_t outliers = 0;
    double sum = 0;
    Image diff = std::move(Image::create(width, height).value());
    for (std::size_t i = 0; i < a.size(); i += 4) {
        int d = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            d = std::max(d, std::abs(static_cast<int>(a[i + k]) - static_cast<int>(b[i + k])));
        }
        worst = std::max(worst, d);
        outliers += d > over ? 1 : 0;
        sum += d;
        const auto v = static_cast<std::uint8_t>(std::min(255, d * 8));
        diff.pixels()[i] = v;
        diff.pixels()[i + 1] = 0;
        diff.pixels()[i + 2] = 0;
        diff.pixels()[i + 3] = 255;
    }
    const std::size_t pixels = a.size() / 4;
    std::printf("  %-12s max %3d  mean %.3f  over %d: %zu of %zu\n", String(name).c_str(), worst, sum / static_cast<double>(pixels),
                over, outliers, pixels);
    const bool ok = worst <= maxAllowed && static_cast<double>(outliers) <= fraction * static_cast<double>(pixels);
    check(ok, "the rendering matches Qt's within the scene's tolerance");
    if (!ok) {
        canvas.unpremultiply();
        writePng(canvas, kOutput / (String(name) + "-cfw.png"));
        writePng(diff, kOutput / (String(name) + "-diff.png"));
        std::printf("  wrote %s-cfw.png and %s-diff.png to %s\n", String(name).c_str(), String(name).c_str(),
                    kOutput.toString().c_str());
    }
}

} // namespace

int main() {
    for (const char *scene : {"fills", "strokes", "gradients", "transforms", "blending", "clips", "images"}) {
        compare(scene);
    }
    return cfw::test::finish("PainterGoldenTest");
}
