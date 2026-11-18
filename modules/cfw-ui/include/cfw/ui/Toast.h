#pragma once

// Toasts: short notices that stack in a corner of the window ("Clip saved",
// "Copied link") and go away by themselves.
//
// A ToastHost is an overlay: add it as the last child of an element that
// fills the window (children of a plain Element are stacked on top of each
// other), and it draws its toasts in the bottom-right corner, newest at the
// bottom. The pointer passes through it to what is underneath.
//
//     auto &toasts = root.add<ToastHost>();
//     toasts.show("Saved to Library", Icon::fromSvg(kCheckSvg));
//
// Each toast slides in (200 ms), stays `duration`, then fades out (300 ms);
// with Theme::reducedMotion it appears and disappears without moving. At
// most kMaxVisible show at once: older ones leave early.
//
// Threads: the surface's thread.

#include <chrono>
#include <vector>

#include "cfw/core/Clock.h"
#include "cfw/core/String.h"
#include "cfw/text/TextLayout.h"
#include "cfw/ui/Element.h"
#include "cfw/ui/Icon.h"
#include "cfw/ui/Surface.h"

namespace cfw {

class ToastHost : public Element {
public:
    ToastHost();
    ~ToastHost() override;

    static constexpr std::size_t kMaxVisible = 4;
    static constexpr Duration kDefaultDuration = std::chrono::milliseconds(3200);

    // Shows `text` with an optional icon drawn in the accent colour.
    void show(String text, Icon icon = {}, Duration duration = kDefaultDuration);
    // Toasts shown now, oldest first (leaving ones included).
    [[nodiscard]] std::size_t count() const noexcept { return m_toasts.size(); }
    [[nodiscard]] String textAt(std::size_t index) const;
    // Distance from the window's edges (default 18).
    void setMargin(float margin);

    void paint(Painter &painter, const Theme &theme) override;

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    struct Toast {
        String text;
        Icon icon;
        TextLayout layout;
        bool laidOut = false;
        TimePoint shown{};
        TimePoint leaves{};
    };
    void tick();
    void ensureTimer();
    [[nodiscard]] RectF toastRect(const Toast &toast, float bottom) const;
    std::vector<Toast> m_toasts;
    float m_margin = 18.0f;
    RectF m_painted; // what the last paint covered
    Surface::TimerId m_timer = 0;
};

} // namespace cfw
