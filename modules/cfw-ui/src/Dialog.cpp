// Dialog, MessageBox, ColorPicker and ColorSwatch.

#include "cfw/ui/Dialog.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ControlText.h"
#include "cfw/gfx/Brush.h"
#include "cfw/gfx/Painter.h"
#include "cfw/ui/Surface.h"

namespace cfw {

using detail::centredOrigin;
using detail::layoutIn;

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

bool inside(const RectF &r, Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.right() && p.y < r.bottom(); }

// A checkerboard under translucent colours.
void checkerboard(Painter &painter, const RectF &r) {
    painter.fillRect(r, Color{0.8f, 0.8f, 0.8f, 1});
    constexpr float kCell = 6.0f;
    painter.save();
    painter.clipRect(r);
    for (float y = r.y; y < r.bottom(); y += kCell) {
        for (float x = r.x + (int((y - r.y) / kCell) % 2 ? kCell : 0.0f); x < r.right(); x += 2 * kCell) {
            painter.fillRect({x, y, kCell, kCell}, Color{0.6f, 0.6f, 0.6f, 1});
        }
    }
    painter.restore();
}

} // namespace

// ---- Dialog ------------------------------------------------------------------------------

Dialog::Dialog(String title) : m_title(std::move(title)) {
    setRole(Role::Dialog);
    setAccessibleName(m_title);
    m_body = &add<Stack>(Stack::Direction::Column, -1.0f, 0.0f);
    m_buttons = &add<Stack>(Stack::Direction::Row, 8.0f);
    m_buttons->add<Element>().setStretch(1.0f); // buttons sit at the right
}

Button &Dialog::addButton(String text, int result, bool isDefault) {
    Button &button = m_buttons->add<Button>(std::move(text));
    button.setFixedSize({std::max(84.0f, button.fixedSize().x), 0.0f});
    static_cast<void>(button.clicked.connect([this, result] { finish(result); }));
    if (isDefault) {
        if (m_default) {
            m_default->setPrimary(false);
        }
        m_default = &button;
        button.setPrimary(true);
    }
    return button;
}

void Dialog::finish(int result) {
    if (m_finished) {
        return;
    }
    m_result = result;
    if (Surface *s = surface()) {
        s->closePopup(*this); // onClosed (from open()) emits finished
    } else {
        m_finished = true;
        finished.emit(result);
    }
}

Dialog &Dialog::open(Surface &surface, std::unique_ptr<Dialog> dialog) {
    Dialog *opened = dialog.get();
    Surface::PopupOptions options;
    options.modal = true;
    options.centred = true;
    surface.openPopup(std::move(dialog), {}, options, [opened] {
        // Closed by a button, Escape or the program: report once.
        if (!opened->m_finished) {
            opened->m_finished = true;
            opened->finished.emit(opened->m_result);
        }
    });
    // The first control has the focus; with a default button, it has it.
    if (opened->m_default) {
        surface.setFocus(opened->m_default);
    }
    return *opened;
}

float Dialog::titleHeight() const { return std::round(theme().controlHeight + 10.0f); }

Vec2 Dialog::measureContent(Vec2 available) {
    const Theme &t = theme();
    const float pad = t.padding * 2.0f;
    const float maxWidth = std::min(available.x - 40.0f, 560.0f);
    const Vec2 body = m_body->measure({maxWidth - 2 * pad, kInfinity});
    const Vec2 buttons = m_buttons->measure({maxWidth - 2 * pad, t.controlHeight});
    const float width = std::clamp(std::max(body.x, buttons.x) + 2 * pad, 320.0f, std::max(320.0f, maxWidth));
    return {width, titleHeight() + body.y + pad + buttons.y + pad + t.padding};
}

void Dialog::arrangeContent(const RectF &r) {
    const Theme &t = theme();
    const float pad = t.padding * 2.0f;
    const float buttonsHeight = t.controlHeight;
    const float bodyTop = r.y + titleHeight();
    const float buttonsTop = r.bottom() - pad - buttonsHeight;
    m_body->arrange({r.x + pad, bodyTop, r.width - 2 * pad, std::max(0.0f, buttonsTop - t.padding - bodyTop)});
    m_buttons->arrange({r.x + pad, buttonsTop, r.width - 2 * pad, buttonsHeight});
}

void Dialog::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    PainterPath shape;
    shape.addRoundedRect(r, theme.radius + 2.0f, theme.radius + 2.0f);
    painter.fillPath(shape, theme.panel);
    painter.strokePath(shape, Pen(theme.border, 1.0f));
    if (layoutIn(m_titleLayout, m_title, theme, r.width - 4 * theme.padding, false, TextAlign::Start)) {
        const RectF title{r.x + 2 * theme.padding, r.y + 4.0f, r.width - 4 * theme.padding, titleHeight() - 4.0f};
        painter.drawText(m_titleLayout, centredOrigin(m_titleLayout, title), theme.text);
    }
}

bool Dialog::onKey(const KeyEvent &event) {
    if (event.type == KeyEvent::Type::Press && event.key == Key::Enter && m_default && m_default->isEnabled()) {
        m_default->clicked.emit();
        return true;
    }
    return false;
}

// ---- MessageBox ----------------------------------------------------------------------------

Dialog &MessageBox::show(Surface &surface, String title, String text, std::vector<String> buttons,
                         std::function<void(int)> done, int defaultButton) {
    auto dialog = std::make_unique<Dialog>(std::move(title));
    Label &message = dialog->body().add<Label>(std::move(text));
    message.setWrap(true);
    if (buttons.empty()) {
        buttons.push_back("OK");
    }
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        dialog->addButton(std::move(buttons[i]), int(i), int(i) == defaultButton);
    }
    if (done) {
        static_cast<void>(dialog->finished.connect(std::move(done)));
    }
    return Dialog::open(surface, std::move(dialog));
}

// ---- ColorPicker ---------------------------------------------------------------------------

ColorPicker::ColorPicker(Color color) : m_hsv(color.toHsv()), m_color(color), m_original(color) {
    setRole(Role::Group);
    setAccessibleName("colour picker");
    m_hex = &add<TextField>(String(color.toHex()));
    m_hex->setAccessibleName("hex");
    m_hex->setMaxLength(9);
    const auto parse = [this] {
        if (const std::optional<Color> parsed = Color::fromHex(m_hex->text())) {
            Hsv hsv = parsed->toHsv();
            if (hsv.s == 0.0f) {
                hsv.h = m_hsv.h; // a grey keeps the hue the strip is on
            }
            change(hsv);
        } else {
            m_hex->setText(m_color.toHex()); // undo a typo
        }
    };
    m_connections.push_back(m_hex->submitted.connect([parse](const String &) { parse(); }));
    m_connections.push_back(m_hex->editingFinished.connect(parse));
}

void ColorPicker::setColor(Color color) {
    m_color = color;
    m_original = color;
    m_hsv = color.toHsv();
    m_hex->setText(color.toHex());
    invalidatePaint();
}

void ColorPicker::change(const Hsv &hsv) {
    m_hsv = hsv;
    const Color color = Color::fromHsv(hsv);
    const bool changed = color.toRgba8() != m_color.toRgba8();
    m_color = color;
    if (!m_hex->hasFocus()) {
        m_hex->setText(color.toHex());
    }
    invalidatePaint();
    if (changed) {
        colorChanged.emit(color);
    }
}

RectF ColorPicker::square() const { return {rect().x, rect().y, kSquare, kSquare * 0.8f}; }

RectF ColorPicker::strip() const {
    const RectF sq = square();
    return {sq.right() + 10.0f, sq.y, kStrip, sq.height};
}

RectF ColorPicker::preview() const {
    const RectF sq = square();
    return {sq.x, sq.bottom() + 10.0f, 96.0f, theme().controlHeight};
}

Vec2 ColorPicker::measureContent(Vec2) {
    const Theme &t = theme();
    return {kSquare + 10.0f + kStrip, kSquare * 0.8f + 10.0f + t.controlHeight};
}

void ColorPicker::arrangeContent(const RectF &) {
    const RectF p = preview();
    const float x = p.right() + 10.0f;
    m_hex->arrange({x, p.y, std::max(60.0f, strip().right() - x), p.height});
}

void ColorPicker::paint(Painter &painter, const Theme &theme) {
    const RectF sq = square();
    // Saturation grows to the right, value upwards: the hue, washed with
    // white from the left and darkened with black from the bottom.
    painter.fillRect(sq, Color::fromHsv({m_hsv.h, 1.0f, 1.0f, 1.0f}));
    const GradientStop white[] = {{0.0f, Color{1, 1, 1, 1}}, {1.0f, Color{1, 1, 1, 0}}};
    painter.fillRect(sq, Brush::linearGradient({sq.x, sq.y}, {sq.right(), sq.y}, white));
    const GradientStop black[] = {{0.0f, Color{0, 0, 0, 0}}, {1.0f, Color{0, 0, 0, 1}}};
    painter.fillRect(sq, Brush::linearGradient({sq.x, sq.y}, {sq.x, sq.bottom()}, black));
    painter.strokeRect(sq.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), Pen(theme.border, 1.0f));
    // The chosen point.
    const Vec2 at{sq.x + m_hsv.s * sq.width, sq.y + (1.0f - m_hsv.v) * sq.height};
    PainterPath ring;
    ring.addEllipse({at.x - 5, at.y - 5, 10, 10});
    painter.strokePath(ring, Pen(Color{0, 0, 0, 0.6f}, 3.0f));
    painter.strokePath(ring, Pen(Color{1, 1, 1, 1}, 1.5f));

    // The hue strip, top to bottom through the colour wheel.
    const RectF st = strip();
    GradientStop hues[7];
    for (int i = 0; i < 7; ++i) {
        hues[i] = {float(i) / 6.0f, Color::fromHsv({float(i) * 60.0f, 1.0f, 1.0f, 1.0f})};
    }
    painter.fillRect(st, Brush::linearGradient({st.x, st.y}, {st.x, st.bottom()}, hues));
    painter.strokeRect(st.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), Pen(theme.border, 1.0f));
    const float y = std::round(st.y + m_hsv.h / 360.0f * st.height);
    painter.strokeRect({st.x - 2.0f, y - 2.0f, st.width + 4.0f, 4.0f}, Pen(theme.text, 1.5f));

    // The old colour and the new one.
    const RectF p = preview();
    checkerboard(painter, p);
    painter.fillRect({p.x, p.y, p.width / 2.0f, p.height}, m_original);
    painter.fillRect({p.x + p.width / 2.0f, p.y, p.width / 2.0f, p.height}, m_color);
    painter.strokeRect(p.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), Pen(theme.border, 1.0f));
}

void ColorPicker::pick(Vec2 position) {
    Hsv hsv = m_hsv;
    if (m_drag == Drag::Square) {
        const RectF sq = square();
        hsv.s = std::clamp((position.x - sq.x) / sq.width, 0.0f, 1.0f);
        hsv.v = std::clamp(1.0f - (position.y - sq.y) / sq.height, 0.0f, 1.0f);
    } else if (m_drag == Drag::Hue) {
        const RectF st = strip();
        hsv.h = std::clamp((position.y - st.y) / st.height, 0.0f, 1.0f) * 360.0f;
        if (hsv.h >= 360.0f) {
            hsv.h = 0.0f;
        }
    }
    change(hsv);
}

bool ColorPicker::onPointer(const PointerEvent &event) {
    switch (event.type) {
    case PointerEvent::Type::Press:
        if (event.button != PointerButton::Left) {
            return false;
        }
        if (inside(square(), event.position)) {
            m_drag = Drag::Square;
        } else if (inside(strip().grownBy(-3, 0, -3, 0), event.position)) {
            m_drag = Drag::Hue;
        } else {
            return false;
        }
        pick(event.position);
        return true;
    case PointerEvent::Type::Move:
        if (m_drag != Drag::None && isPressed()) {
            pick(event.position);
            return true;
        }
        return false;
    case PointerEvent::Type::Release:
        if (m_drag != Drag::None) {
            m_drag = Drag::None;
            return true;
        }
        return false;
    default:
        return false;
    }
}

Dialog &ColorPicker::open(Surface &surface, Color initial, String title, std::function<void(Color)> accepted) {
    auto dialog = std::make_unique<Dialog>(std::move(title));
    ColorPicker &picker = dialog->body().add<ColorPicker>(initial);
    dialog->addButton("Cancel", Dialog::kCancelled);
    dialog->addButton("OK", 1, true);
    ColorPicker *chosen = &picker;
    static_cast<void>(dialog->finished.connect([chosen, accepted = std::move(accepted)](int result) {
        if (result == 1 && accepted) {
            accepted(chosen->color()); // the dialog (and picker) still exist until the next event
        }
    }));
    return Dialog::open(surface, std::move(dialog));
}

// ---- ColorSwatch ----------------------------------------------------------------------------

ColorSwatch::ColorSwatch(Color color) : m_color(color) {
    setRole(Role::Button);
    setAccessibleName("colour");
    setFocusable(true);
}

void ColorSwatch::setColor(Color color) {
    m_color = color;
    invalidatePaint();
}

Vec2 ColorSwatch::measureContent(Vec2) { return {90.0f, theme().controlHeight}; }

void ColorSwatch::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    PainterPath frame;
    frame.addRoundedRect(r, theme.radius, theme.radius);
    painter.fillPath(frame, isHovered() ? theme.controlHover : theme.control);
    const RectF box{r.x + 4.0f, r.y + 4.0f, r.height - 8.0f, r.height - 8.0f};
    checkerboard(painter, box);
    painter.fillRect(box, m_color);
    painter.strokeRect(box.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), Pen(theme.border, 1.0f));
    if (layoutIn(m_layout, m_color.toHex(), theme, kInfinity, false, TextAlign::Start)) {
        const RectF text{box.right() + theme.spacing, r.y, r.right() - box.right() - theme.spacing, r.height};
        painter.drawText(m_layout, centredOrigin(m_layout, text), isEnabled() ? theme.text : theme.textDisabled);
    }
    if (hasFocus()) {
        PainterPath ring;
        const float inset = theme.focusRingWidth / 2.0f;
        ring.addRoundedRect(r.grownBy(-inset, -inset, -inset, -inset), theme.radius, theme.radius);
        painter.strokePath(ring, Pen(theme.accent, theme.focusRingWidth));
    }
}

void ColorSwatch::openPicker() {
    Surface *s = surface();
    if (!s) {
        return;
    }
    ColorPicker::open(*s, m_color, m_title, [this](Color color) {
        if (color.toRgba8() != m_color.toRgba8()) {
            setColor(color);
            colorChanged.emit(color);
        }
    });
}

bool ColorSwatch::onPointer(const PointerEvent &event) {
    if (event.button != PointerButton::Left) {
        return false;
    }
    if (event.type == PointerEvent::Type::Press) {
        return true;
    }
    if (event.type == PointerEvent::Type::Release && isPressed() && isHovered()) {
        openPicker();
        return true;
    }
    return false;
}

bool ColorSwatch::onKey(const KeyEvent &event) {
    if (event.type == KeyEvent::Type::Press && !event.repeat && (event.key == Key::Space || event.key == Key::Enter)) {
        openPicker();
        return true;
    }
    return false;
}

} // namespace cfw
