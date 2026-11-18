#include "cfw/ui/Toast.h"

#include <algorithm>
#include <cmath>

#include "cfw/gfx/Painter.h"
#include "cfw/ui/Theme.h"

namespace cfw {

namespace {

constexpr auto kIn = std::chrono::milliseconds(200);
constexpr auto kOut = std::chrono::milliseconds(300);
constexpr float kGap = 8.0f;
constexpr float kMaxWidth = 380.0f;
constexpr float kPadX = 14.0f;
constexpr float kPadY = 10.0f;
constexpr float kIconSize = 16.0f;

float fraction(Duration part, Duration whole) {
    return std::clamp(std::chrono::duration<float>(part).count() / std::chrono::duration<float>(whole).count(), 0.0f,
                      1.0f);
}

} // namespace

ToastHost::ToastHost() {
    setTransparentForInput(true);
    setRole(Role::None);
}

ToastHost::~ToastHost() {
    if (m_timer) {
        if (Surface *s = surface()) {
            s->stopTimer(m_timer);
        }
    }
}

void ToastHost::setMargin(float margin) {
    m_margin = margin;
    invalidatePaint();
}

String ToastHost::textAt(std::size_t index) const { return index < m_toasts.size() ? m_toasts[index].text : String(); }

Vec2 ToastHost::measureContent(Vec2) { return {}; } // fills its parent; wants nothing

void ToastHost::show(String text, Icon icon, Duration duration) {
    Surface *s = surface();
    const TimePoint now = s ? s->now() : Clock::now();
    Toast toast;
    toast.text = std::move(text);
    toast.icon = std::move(icon);
    toast.shown = now;
    toast.leaves = now + std::chrono::duration_cast<TimePoint::duration>(duration);
    m_toasts.push_back(std::move(toast));
    // Too many: the oldest still showing starts leaving now.
    std::size_t staying = 0;
    for (const Toast &t : m_toasts) {
        staying += t.leaves > now ? 1u : 0u;
    }
    for (Toast &t : m_toasts) {
        if (staying <= kMaxVisible) {
            break;
        }
        if (t.leaves > now) {
            t.leaves = now;
            --staying;
        }
    }
    invalidatePaint(m_painted.isEmpty() ? rect() : m_painted.united({rect().x + rect().width - kMaxWidth - 60.0f,
                                                                     rect().y + rect().height - 120.0f,
                                                                     kMaxWidth + 60.0f, 120.0f}));
    ensureTimer();
}

void ToastHost::ensureTimer() {
    Surface *s = surface();
    if (!s || m_timer) {
        return;
    }
    m_timer = s->startTimer(std::chrono::milliseconds(16), [this] { tick(); }, true);
}

void ToastHost::tick() {
    Surface *s = surface();
    if (!s) {
        return;
    }
    const TimePoint now = s->now();
    std::erase_if(m_toasts, [&](const Toast &t) { return now >= t.leaves + kOut; });
    invalidatePaint(m_painted); // where they were
    m_painted = {};
    if (!m_toasts.empty()) {
        // Where they will be: the column above the corner, widest toast wide.
        const float h = std::min(rect().height, 4.0f * 60.0f + 40.0f);
        invalidatePaint({rect().x + rect().width - m_margin - kMaxWidth - 30.0f, rect().y + rect().height - h,
                         kMaxWidth + m_margin + 30.0f, h});
    }
    if (m_toasts.empty()) {
        s->stopTimer(m_timer);
        m_timer = 0;
    }
}

RectF ToastHost::toastRect(const Toast &toast, float bottom) const {
    const float iconWidth = toast.icon.isNull() ? 0.0f : kIconSize + 10.0f;
    const float width = std::min(kMaxWidth, std::ceil(toast.layout.size().x) + iconWidth + kPadX * 2.0f);
    const float height = std::max(kIconSize, std::ceil(toast.layout.size().y)) + kPadY * 2.0f;
    return {rect().x + rect().width - m_margin - width, bottom - height, width, height};
}

void ToastHost::paint(Painter &painter, const Theme &theme) {
    if (m_toasts.empty()) {
        return;
    }
    Surface *s = surface();
    const TimePoint now = s ? s->now() : Clock::now();
    m_painted = {};
    float bottom = rect().y + rect().height - m_margin;
    // Newest at the bottom: walk backwards.
    for (std::size_t i = m_toasts.size(); i-- > 0;) {
        Toast &t = m_toasts[i];
        if (!t.laidOut && theme.font) {
            TextStyle style;
            style.font = theme.font;
            style.pixelSize = theme.fontSize;
            style.fallback = theme.fonts.get();
            TextLayoutOptions options;
            options.maxWidth = kMaxWidth - kPadX * 2.0f - (t.icon.isNull() ? 0.0f : kIconSize + 10.0f);
            t.layout.setText(t.text);
            t.layout.layout(style, options);
            t.laidOut = true;
        }
        const float in = theme.reducedMotion ? 1.0f : fraction(now - t.shown, kIn);
        const float out = now < t.leaves ? 1.0f : 1.0f - fraction(now - t.leaves, kOut);
        const float opacity = std::min(in, out);
        RectF r = toastRect(t, bottom);
        if (!theme.reducedMotion) {
            r.y += std::round((1.0f - in) * 8.0f); // rises into place
        }
        painter.save();
        painter.setOpacity(opacity);
        // A soft shadow: a few widening, fading rounded rectangles.
        for (int k = 3; k >= 1; --k) {
            const float g = float(k) * 3.0f;
            PainterPath shadow;
            shadow.addRoundedRect({r.x - g, r.y - g + 6.0f, r.width + g * 2.0f, r.height + g * 2.0f}, 10.0f + g,
                                  10.0f + g);
            painter.fillPath(shadow, Color{0, 0, 0, 0.10f});
        }
        PainterPath box;
        box.addRoundedRect(r, 10.0f, 10.0f);
        painter.fillPath(box, theme.control);
        PainterPath outline;
        outline.addRoundedRect(r.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), 9.5f, 9.5f);
        painter.strokePath(outline, Pen(theme.border, 1.0f));
        float x = r.x + kPadX;
        if (!t.icon.isNull()) {
            t.icon.paint(painter, {x, std::round(r.y + (r.height - kIconSize) / 2.0f), kIconSize, kIconSize},
                         theme.accent);
            x += kIconSize + 10.0f;
        }
        const auto lines = t.layout.lines();
        if (!lines.empty()) {
            const float h = t.layout.size().y;
            const float top = r.y + (r.height - h) / 2.0f;
            painter.drawText(t.layout, {x, std::round(top)}, theme.text);
        }
        painter.restore();
        m_painted = m_painted.isEmpty() ? r.grownBy(12, 12, 12, 12) : m_painted.united(r.grownBy(12, 12, 12, 12));
        bottom = r.y - kGap;
    }
}

} // namespace cfw
