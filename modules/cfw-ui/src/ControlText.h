#pragma once

// Text helpers shared by the controls (internal).

#include <cmath>
#include <limits>

#include "cfw/text/TextLayout.h"
#include "cfw/ui/Theme.h"

namespace cfw::detail {



// Lays `text` out in the theme's font; false when the theme has no font.
inline bool layoutIn(TextLayout &layout, StringView text, const Theme &theme, float maxWidth, bool wrap, TextAlign align) {
    if (!theme.font) {
        return false;
    }
    TextStyle style;
    style.font = theme.font;
    style.pixelSize = theme.fontSize;
    style.fallback = theme.fonts.get();
    TextLayoutOptions options;
    options.maxWidth = maxWidth;
    options.wrap = wrap;
    options.align = align;
    layout.setText(text);
    layout.layout(style, options);
    return true;
}

// The first baseline on a whole pixel, the text block centred vertically.
inline Vec2 centredOrigin(const TextLayout &layout, const RectF &rect) {
    const auto lines = layout.lines();
    if (lines.empty()) {
        return rect.origin();
    }
    const auto &last = lines[lines.size() - 1];
    const float height = last.top + last.ascent + last.descent;
    const float top = rect.y + (rect.height - height) / 2.0f;
    const float baseline = lines[0].baseline;
    return {rect.x, std::round(top + baseline) - baseline};
}


} // namespace cfw::detail
