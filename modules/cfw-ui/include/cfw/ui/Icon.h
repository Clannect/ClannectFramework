#pragma once

// An icon: an SVG image drawn at any size. Icons drawn in "currentColor"
// (Lucide's outlines) take the colour of the text around them, so one glyph
// serves enabled, disabled, selected and accented states; an icon with its
// own colour (the Explorer's class icons) keeps it.
//
// Threads: an Icon is a value sharing its parsed image; paint on one thread.

#include <memory>
#include <optional>

#include "cfw/core/Color.h"
#include "cfw/core/Rect.h"
#include "cfw/core/String.h"

namespace cfw {

class Painter;
class SvgImage;

class Icon {
public:
    Icon() = default;
    explicit Icon(std::shared_ptr<const SvgImage> image, std::optional<Color> color = {});
    // A null icon when `svg` does not parse.
    [[nodiscard]] static Icon fromSvg(StringView svg, std::optional<Color> color = {});

    [[nodiscard]] bool isNull() const noexcept { return m_image == nullptr; }
    [[nodiscard]] const std::shared_ptr<const SvgImage> &image() const noexcept { return m_image; }
    [[nodiscard]] const std::optional<Color> &color() const noexcept { return m_color; }
    // The same image in its own colour.
    [[nodiscard]] Icon withColor(std::optional<Color> color) const { return Icon(m_image, color); }

    // Draws into `rect`: "currentColor" becomes the icon's own colour if it
    // has one, else `tint`.
    void paint(Painter &painter, const RectF &rect, Color tint) const;

    friend bool operator==(const Icon &, const Icon &) = default;

private:
    std::shared_ptr<const SvgImage> m_image;
    std::optional<Color> m_color;
};

} // namespace cfw
