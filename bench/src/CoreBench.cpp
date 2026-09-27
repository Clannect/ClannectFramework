// cfw-bench: the §7 benchmarks that exist so far (cfw-core and cfw-io).
//
//   cfw-bench                       run everything, print a table
//   cfw-bench --quick               allocation budgets only (few runs; what CTest runs)
//   cfw-bench --repeat N            run the suite N times, keep each best median
//   cfw-bench --json out.json       also write machine-readable results
//   cfw-bench --baseline b.json     fail (exit 3) if any median regressed > 5%
//   cfw-bench --qt-reference q.json print the ratio to the Qt build's times
//   cfw-bench --write-scene f       write the synthetic 5 MB scene and exit
//
// Allocation budgets are always enforced (exit 2 on violation): they are
// deterministic, so they can gate every build. Time is only compared against
// a baseline recorded on the same machine.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <vector>

#include "cfw/bench/Bench.h"
#include "cfw/core/Arena.h"
#include "cfw/core/ClassInfo.h"
#include "cfw/core/PropertyBag.h"
#include "cfw/core/Quat.h"
#include "cfw/core/Sha256.h"
#include "cfw/core/Signal.h"
#include "cfw/core/Transform2D.h"
#include "cfw/core/Utf8.h"
#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/JsonReader.h"
#include "cfw/io/JsonWriter.h"

using namespace cfw;
using cfw::bench::keep;
using cfw::bench::Measurement;
using cfw::bench::measure;

namespace {

struct Budget {
    double maxAllocationsPerOp = -1.0; // < 0: no allocation budget
};

struct Entry {
    Measurement m;
    Budget budget;
};

// A scene in the engine's .cescene format, about `targetBytes` long: a
// Workspace holding many Parts with the property shapes real scenes have.
JsonValue makeSceneWithParts(int partCount) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> coord(-500.0, 500.0);
    std::uniform_int_distribution<int> byte(0, 255);
    const auto prop = [](const char *type, JsonValue value) {
        return JsonValue(JsonObject{{"type", type}, {"value", std::move(value)}});
    };
    const auto vec3 = [&](double x, double y, double z) { return JsonValue(JsonArray{x, y, z}); };

    JsonArray parts;
    for (int i = 0; i < partCount; ++i) {
        JsonObject properties{
            {"Anchored", prop("Bool", i % 3 == 0)},
            {"CanCollide", prop("Bool", true)},
            {"Color", prop("Color", JsonArray{byte(rng), byte(rng), byte(rng), 255})},
            {"Material", prop("String", i % 2 ? "Plastic" : "SmoothPlastic")},
            {"Position", prop("Vector3", vec3(coord(rng), coord(rng) * 0.1, coord(rng)))},
            {"Rotation", prop("Vector3", vec3(0, coord(rng) * 0.36, 0))},
            {"Size", prop("Vector3", vec3(4, 1.2, 2))},
            {"Transparency", prop("Number", i % 5 == 0 ? 0.25 : 0.0)},
        };
        parts.push_back(JsonObject{{"class", "Part"}, {"name", "Part" + std::to_string(i)}, {"properties", std::move(properties)}});
    }
    JsonObject workspace{{"class", "Workspace"}, {"name", "Workspace"}, {"children", std::move(parts)}};
    JsonObject root{{"class", "DataModel"}, {"name", "Game"}, {"children", JsonArray{std::move(workspace)}}};
    return JsonObject{{"formatVersion", 1}, {"root", std::move(root)}};
}

// A scene whose indented text is close to `targetBytes`: size a sample, then scale.
JsonValue makeScene(std::size_t targetBytes) {
    const std::size_t sample = writeJson(makeSceneWithParts(1000)).size();
    const auto parts = static_cast<int>(static_cast<double>(targetBytes) / static_cast<double>(sample) * 1000.0);
    return makeSceneWithParts(parts);
}

std::vector<Entry> runAll(const String &sceneText) {
    std::vector<Entry> results;

    // --- cfw-io: the 5 MB scene (spec §7: "Scene load, 5 MB JSON ≤ Qt's time").
    {
        results.push_back({measure("json.parse.scene5mb", 15, 1, [&] {
                               Result<JsonValue> v = parseJson(sceneText);
                               keep(v);
                           }), {}});
        const JsonValue scene = parseJson(sceneText).value();
        results.push_back({measure("json.write.scene5mb", 15, 1, [&] {
                               String out = writeJson(scene);
                               keep(out);
                           }), {}});
    }

    // --- Property access by Name (spec §4.1: O(1), no allocation).
    const ClassInfo part = ClassInfo::Builder("Part")
                               .property({.name = "Anchored", .type = VariantType::Bool, .defaultValue = false})
                               .property({.name = "Color", .type = VariantType::Color, .defaultValue = Color{}})
                               .property({.name = "Material", .type = VariantType::String, .defaultValue = "Plastic"})
                               .property({.name = "Position", .type = VariantType::Vec3, .defaultValue = Vec3{}})
                               .property({.name = "Rotation", .type = VariantType::Vec3, .defaultValue = Vec3{}})
                               .property({.name = "Size", .type = VariantType::Vec3, .defaultValue = Vec3{4, 1, 2}})
                               .property({.name = "Transparency", .type = VariantType::Double, .defaultValue = 0.0})
                               .property({.name = "Name", .type = VariantType::String, .defaultValue = "Part"})
                               .build()
                               .value();
    PropertyBag bag(part);
    (void)bag.set("Position", Vec3{1, 2, 3});
    constexpr Name kPosition = "Position";
    constexpr Name kTransparency = "Transparency";
    constexpr std::uint64_t kOps = 100000;

    results.push_back({measure("property.get.byName", 21, kOps, [&] {
                           for (std::uint64_t i = 0; i < kOps; ++i) {
                               keep(bag.get(kPosition));
                           }
                       }), {0.0}});
    results.push_back({measure("property.set.vec3", 21, kOps, [&] {
                           for (std::uint64_t i = 0; i < kOps; ++i) {
                               Result<bool> r = bag.set(kPosition, Vec3{static_cast<float>(i & 7), 2, 3});
                               keep(r);
                           }
                       }), {0.0}});
    results.push_back({measure("property.indexOf.text", 21, kOps, [&] {
                           for (std::uint64_t i = 0; i < kOps; ++i) {
                               keep(part.indexOf(StringView("Transparency")));
                           }
                       }), {0.0}});
    (void)kTransparency;

    // --- Signals.
    Signal<int> signal;
    std::uint32_t sum = 0; // unsigned: the slots wrap around by design
    const ScopedConnection a = signal.connect([&](int v) { sum += static_cast<std::uint32_t>(v); });
    const ScopedConnection b = signal.connect([&](int v) { sum ^= static_cast<std::uint32_t>(v); });
    const ScopedConnection c = signal.connect([&](int v) { sum -= static_cast<std::uint32_t>(v / 2); });
    results.push_back({measure("signal.emit.3slots", 21, kOps, [&] {
                           for (std::uint64_t i = 0; i < kOps; ++i) {
                               signal.emit(static_cast<int>(i));
                           }
                           keep(sum);
                       }), {0.0}});

    // --- Per-frame allocation (spec §7: 0 allocations per repainted frame).
    Arena arena(64 * 1024);
    struct Quad {
        float x, y, w, h;
        std::uint32_t colour;
    };
    results.push_back({measure("arena.frame.2000quads", 51, 1, [&] {
                           arena.reset();
                           for (int i = 0; i < 2000; ++i) {
                               keep(arena.make<Quad>(Quad{1, 2, 3, 4, 0xFFFFFFFFu}));
                           }
                       }), {0.0}});

    // --- 2D painting on the CPU backend (spec §7: 10,000 anti-aliased rounded
    // rectangles at 1440p within 1.2x Qt; 0 allocations per repainted frame).
    {
        static Image screen = std::move(Image::create(2560, 1440, AlphaMode::Premultiplied).value());
        static RasterPaintBackend backend(screen);
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> px(0, 2460);
        std::uniform_real_distribution<float> py(0, 1340);
        std::uniform_real_distribution<float> size(10, 100);
        struct Shape {
            PainterPath path;
            Brush brush;
        };
        static std::vector<Shape> shapes;
        for (int i = 0; i < 10000; ++i) {
            PainterPath path;
            path.addRoundedRect({px(rng), py(rng), size(rng), size(rng)}, 6, 6);
            shapes.push_back({std::move(path),
                              Color::fromRgba8(static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng()),
                                               static_cast<std::uint8_t>(rng()),
                                               static_cast<std::uint8_t>(128 + rng() % 128))});
        }
        results.push_back({measure("gfx.fill.10000roundrects.1440p", 5, 1, [&] {
                               Painter painter(backend);
                               for (const Shape &s : shapes) {
                                   painter.fillPath(s.path, s.brush);
                               }
                           }), {-1.0}});

        // A UI-like frame: panels, text-field outlines, a dashed selection, a
        // gradient, clipped content, an icon. Built once; repainted per op.
        static Image icon = std::move(Image::create(32, 32).value());
        std::fill(icon.pixels().begin(), icon.pixels().end(), std::uint8_t{200});
        static Image frame = std::move(Image::create(800, 600, AlphaMode::Premultiplied).value());
        static RasterPaintBackend frameBackend(frame);
        static Painter painter(frameBackend);
        static PainterPath panel;
        panel.addRoundedRect({10, 10, 300, 580}, 8, 8);
        static PainterPath circle;
        circle.addEllipse({400, 100, 200, 200});
        const std::array<GradientStop, 2> stops{GradientStop{0, Color{0.2f, 0.3f, 0.9f, 1}},
                                                GradientStop{1, Color{0.9f, 0.9f, 1, 1}}};
        static const Brush gradient = Brush::linearGradient({0, 0}, {0, 600}, stops);
        static Pen outline(Color{0.3f, 0.3f, 0.3f, 1}, 1);
        static Pen dashed(Color{0.1f, 0.5f, 1, 1}, 2);
        dashed.dashes = {4, 2};
        results.push_back({measure("gfx.frame.ui.800x600", 21, 1, [&] {
                               painter.fillRect({0, 0, 800, 600}, gradient);
                               painter.fillPath(panel, Color{1, 1, 1, 0.9f});
                               for (int i = 0; i < 40; ++i) {
                                   const float y = 20.0f + static_cast<float>(i) * 14.0f;
                                   painter.fillRect({20, y, 280, 12}, Color{0.95f, 0.95f, 0.95f, 1});
                                   painter.strokeRect({20.5f, y + 0.5f, 279, 11}, outline);
                               }
                               painter.save();
                               painter.clipPath(circle);
                               painter.translate(500, 200);
                               painter.rotate(15);
                               painter.drawImage({-80, -80, 160, 160}, icon);
                               painter.restore();
                               painter.strokeRect({395.5f, 95.5f, 209, 209}, dashed);
                           }), {0.0}});
    }

    // --- Hashing and text throughput (1 MB inputs; per-op = whole buffer).
    String megabyte(1 << 20, 'a');
    for (std::size_t i = 0; i < megabyte.size(); i += 97) {
        megabyte[i] = static_cast<char>('A' + i % 26);
    }
    results.push_back({measure("sha256.1mb", 15, 1, [&] { keep(Sha256::hash(megabyte)); }), {0.0}});
    String mixed;
    while (mixed.size() < (1u << 20)) {
        mixed += "Clannect \xE6\x97\xA5\xE6\x9C\xAC caf\xC3\xA9 \xF0\x9F\x98\x80 ";
    }
    results.push_back({measure("utf8.validate.1mb", 15, 1, [&] { keep(isValidUtf8(mixed)); }), {0.0}});

    // --- Math used every frame.
    results.push_back({measure("quat.fromEuler", 21, kOps, [&] {
                           for (std::uint64_t i = 0; i < kOps; ++i) {
                               keep(Quat::fromEulerDegrees({static_cast<float>(i & 63), 45, 10}));
                           }
                       }), {0.0}});
    const Transform2D t = Transform2D::quadToQuad({Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}},
                                                  {Vec2{10, 20}, Vec2{210, 40}, Vec2{180, 160}, Vec2{30, 120}})
                              .value();
    results.push_back({measure("transform2d.map.perspective", 21, kOps, [&] {
                           for (std::uint64_t i = 0; i < kOps; ++i) {
                               keep(t.map({static_cast<float>(i & 127), 25}));
                           }
                       }), {0.0}});
    return results;
}

String formatNs(double ns) {
    char buffer[32];
    if (ns >= 1e6) {
        std::snprintf(buffer, sizeof buffer, "%.2f ms", ns / 1e6);
    } else if (ns >= 1e3) {
        std::snprintf(buffer, sizeof buffer, "%.2f us", ns / 1e3);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.1f ns", ns);
    }
    return buffer;
}

std::map<String, double> loadMedians(const char *path) {
    std::map<String, double> medians;
    const Result<String> text = readTextFile(Path(path));
    if (!text) {
        std::fprintf(stderr, "cannot read %s: %s\n", path, text.error().describe().c_str());
        return medians;
    }
    const Result<JsonValue> json = parseJson(text.value());
    if (!json) {
        std::fprintf(stderr, "cannot parse %s: %s\n", path, json.error().describe().c_str());
        return medians;
    }
    if (const JsonObject *object = json.value()["results"].asObject()) {
        for (const auto &[name, value] : *object) {
            medians[name] = value["medianNs"].toDouble(0.0);
        }
    }
    return medians;
}

} // namespace

int main(int argc, char **argv) {
    const char *jsonOut = nullptr;
    const char *baseline = nullptr;
    const char *qtReference = nullptr;
    int repeat = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            cfw::bench::quickMode() = true;
        }
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--repeat") == 0) {
            repeat = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--json") == 0) {
            jsonOut = argv[++i];
        } else if (std::strcmp(argv[i], "--baseline") == 0) {
            baseline = argv[++i];
        } else if (std::strcmp(argv[i], "--qt-reference") == 0) {
            qtReference = argv[++i];
        } else if (std::strcmp(argv[i], "--write-scene") == 0) {
            const String scene = writeJson(makeScene(5u * 1024u * 1024u));
            const Result<void> written = writeFileAtomic(Path(argv[i + 1]), scene);
            std::printf("wrote %zu bytes to %s: %s\n", scene.size(), argv[i + 1], written ? "ok" : "FAILED");
            return written ? 0 : 1;
        }
    }

    const String sceneText = writeJson(makeScene(5u * 1024u * 1024u));
    std::printf("scene: %.2f MB\n\n", static_cast<double>(sceneText.size()) / (1024.0 * 1024.0));
    // Machine noise (turbo, core scheduling) only ever makes a run slower, so
    // across repeated runs of the whole suite keep each benchmark's best median.
    std::vector<Entry> results = runAll(sceneText);
    for (int run = 1; run < repeat; ++run) {
        const std::vector<Entry> again = runAll(sceneText);
        for (std::size_t i = 0; i < results.size(); ++i) {
            results[i].m.medianNs = std::min(results[i].m.medianNs, again[i].m.medianNs);
            results[i].m.minNs = std::min(results[i].m.minNs, again[i].m.minNs);
        }
    }

    const std::map<String, double> base = baseline ? loadMedians(baseline) : std::map<String, double>{};
    const std::map<String, double> qt = qtReference ? loadMedians(qtReference) : std::map<String, double>{};

    int exitCode = 0;
    std::printf("%-30s %12s %12s %10s %10s %s\n", "benchmark", "median", "min", "allocs/op", "bytes/op", "notes");
    for (const Entry &e : results) {
        String notes;
        if (e.budget.maxAllocationsPerOp >= 0.0 && e.m.allocationsPerOp > e.budget.maxAllocationsPerOp) {
            notes += "ALLOCATION BUDGET EXCEEDED ";
            exitCode = std::max(exitCode, 2);
        }
        if (const auto it = qt.find(e.m.name); it != qt.end() && it->second > 0.0) {
            char ratio[48];
            std::snprintf(ratio, sizeof ratio, "%.2fx Qt ", e.m.medianNs / it->second);
            notes += ratio;
        }
        if (const auto it = base.find(e.m.name); it != base.end() && it->second > 0.0) {
            const double change = e.m.medianNs / it->second - 1.0;
            char delta[48];
            std::snprintf(delta, sizeof delta, "%+.1f%% vs baseline ", change * 100.0);
            notes += delta;
            if (change > 0.05) {
                notes += "REGRESSION ";
                exitCode = std::max(exitCode, 3);
            }
        }
        std::printf("%-30s %12s %12s %10.2f %10.1f %s\n", e.m.name.c_str(), formatNs(e.m.medianNs).c_str(),
                    formatNs(e.m.minNs).c_str(), e.m.allocationsPerOp, e.m.bytesPerOp, notes.c_str());
    }

    if (jsonOut != nullptr) {
        JsonObject entries;
        for (const Entry &e : results) {
            entries.set(e.m.name, JsonObject{{"medianNs", e.m.medianNs},
                                             {"minNs", e.m.minNs},
                                             {"allocationsPerOp", e.m.allocationsPerOp},
                                             {"bytesPerOp", e.m.bytesPerOp}});
        }
        const JsonObject document{{"results", std::move(entries)}, {"sceneBytes", static_cast<std::int64_t>(sceneText.size())}};
        if (!writeFileAtomic(Path(jsonOut), writeJson(JsonValue(document)))) {
            std::fprintf(stderr, "cannot write %s\n", jsonOut);
            return 1;
        }
    }
    return exitCode;
}
