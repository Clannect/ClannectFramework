#include "cfw/ui/Theme.h"

#include <initializer_list>

#include "cfw/text/FontDatabase.h"

namespace cfw {

namespace {

constexpr Color hex(std::uint32_t rgb) {
    return Color::fromRgba8(std::uint8_t(rgb >> 16), std::uint8_t(rgb >> 8), std::uint8_t(rgb));
}

} // namespace

Theme Theme::dark() {
    Theme t;
    t.window = hex(0x1b1e24);
    t.panel = hex(0x23272e);
    t.control = hex(0x2f333a);
    t.controlHover = hex(0x363b43);   // QColor::lighter(115) of control, as the editor's buttons
    t.controlPressed = hex(0x24272d); // QColor::darker(130)
    t.controlDisabled = hex(0x272729);
    t.border = hex(0x3a4049);
    t.accent = hex(0x5b8def);
    t.accentText = hex(0xffffff);
    t.text = hex(0xf2f4f7);
    t.textMuted = hex(0x8a929e);
    t.textDisabled = hex(0x5a616b);
    t.scrim = Color{0.0f, 0.0f, 0.0f, 0.45f};
    t.error = hex(0xf05a4f);
    return t;
}

Theme Theme::light() {
    Theme t;
    t.window = hex(0xf4f5f7);
    t.panel = hex(0xffffff);
    t.control = hex(0xe8eaee);
    t.controlHover = hex(0xdde0e6);
    t.controlPressed = hex(0xcfd3da);
    t.controlDisabled = hex(0xeeeff2);
    t.border = hex(0xc9cdd4);
    t.accent = hex(0x2f6fe4);
    t.accentText = hex(0xffffff);
    t.text = hex(0x1b1e24);
    t.textMuted = hex(0x5a616b);
    t.textDisabled = hex(0xa4aab3);
    t.scrim = Color{0.0f, 0.0f, 0.0f, 0.25f};
    t.error = hex(0xd23c32);
    return t;
}

Theme Theme::withSystemFonts() const {
    Theme t = *this;
    auto database = std::make_shared<FontDatabase>();
    database->addSystemFonts();
    for (const StringView family : {"Segoe UI", ".AppleSystemUIFont", "Helvetica Neue", "DejaVu Sans", "Noto Sans",
                                    "Liberation Sans", "Arial"}) {
        if (auto face = database->match(family)) {
            t.font = std::move(face);
            break;
        }
    }
    if (!t.font && !database->faces().empty()) {
        t.font = database->face(0);
    }
    for (const StringView family : {"Cascadia Mono", "Consolas", "SF Mono", "Menlo", "DejaVu Sans Mono",
                                    "Noto Sans Mono", "Liberation Mono", "Courier New"}) {
        if (auto face = database->match(family)) {
            t.monoFont = std::move(face);
            break;
        }
    }
    t.fonts = std::move(database);
    return t;
}

} // namespace cfw
