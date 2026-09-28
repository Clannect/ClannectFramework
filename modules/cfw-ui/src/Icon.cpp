#include "cfw/ui/Icon.h"

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/SvgImage.h"

namespace cfw {

Icon::Icon(std::shared_ptr<const SvgImage> image, std::optional<Color> color)
    : m_image(std::move(image)), m_color(color) {}

Icon Icon::fromSvg(StringView svg, std::optional<Color> color) {
    auto parsed = SvgImage::parse(svg);
    if (!parsed) {
        return {};
    }
    return Icon(std::make_shared<const SvgImage>(std::move(parsed).value()), color);
}

void Icon::paint(Painter &painter, const RectF &rect, Color tint) const {
    if (m_image) {
        m_image->render(painter, rect, m_color.value_or(tint));
    }
}

} // namespace cfw
