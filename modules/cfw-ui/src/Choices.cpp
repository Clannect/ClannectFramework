// CheckBox, NumberField, Dropdown, MenuItem and Menu.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ControlText.h"
#include "cfw/core/Strings.h"
#include "cfw/gfx/Painter.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Surface.h"

namespace cfw {

using detail::centredOrigin;
using detail::layoutIn;

namespace {

constexpr float kBox = 14.0f;
const float kInfinity = std::numeric_limits<float>::infinity();

bool activates(const KeyEvent &e) {
    return e.type == KeyEvent::Type::Press && !e.repeat && (e.key == Key::Space || e.key == Key::Enter);
}

void focusRing(Painter &painter, const RectF &rect, const Theme &theme) {
    PainterPath ring;
    const float inset = theme.focusRingWidth / 2.0f;
    ring.addRoundedRect(rect.grownBy(-inset, -inset, -inset, -inset), theme.radius, theme.radius);
    painter.strokePath(ring, Pen(theme.accent, theme.focusRingWidth));
}

// A downward chevron centred at `c`.
void chevron(Painter &painter, Vec2 c, const Color &color) {
    PainterPath path;
    path.moveTo({c.x - 4, c.y - 2});
    path.lineTo({c.x, c.y + 2});
    path.lineTo({c.x + 4, c.y - 2});
    Pen pen(color, 1.5f);
    pen.cap = CapStyle::Round;
    pen.join = JoinStyle::Round;
    painter.strokePath(path, pen);
}

} // namespace

// ---- CheckBox --------------------------------------------------------------------

CheckBox::CheckBox(String text, bool checked) : m_text(std::move(text)), m_checked(checked) {
    setRole(Role::CheckBox);
    setAccessibleName(m_text);
    setFocusable(true);
}

void CheckBox::setChecked(bool checked) {
    m_checked = checked;
    m_partial = false;
    invalidatePaint();
}

void CheckBox::setPartial(bool partial) {
    m_partial = partial;
    invalidatePaint();
}

void CheckBox::toggle() {
    m_checked = m_partial || !m_checked;
    m_partial = false;
    invalidatePaint();
    toggled.emit(m_checked);
}

Vec2 CheckBox::measureContent(Vec2) {
    const Theme &t = theme();
    m_laidOut = layoutIn(m_layout, m_text, t, kInfinity, false, TextAlign::Start);
    const float text = m_laidOut && !m_text.empty() ? t.spacing + std::ceil(m_layout.size().x) : 0.0f;
    return {kBox + text + 4.0f, t.controlHeight};
}

void CheckBox::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    const RectF box{r.x + 2.0f, std::round(r.y + (r.height - kBox) / 2.0f), kBox, kBox};
    PainterPath shape;
    shape.addRoundedRect(box, 3.0f, 3.0f);
    const bool enabled = isEnabled();
    if (m_partial) {
        painter.fillPath(shape, enabled ? theme.accent : theme.controlDisabled);
        Pen pen(theme.accentText, 1.8f);
        pen.cap = CapStyle::Round;
        painter.drawLine({box.x + 4.0f, box.y + kBox / 2.0f}, {box.right() - 4.0f, box.y + kBox / 2.0f}, pen);
    } else if (m_checked) {
        painter.fillPath(shape, enabled ? theme.accent : theme.controlDisabled);
        PainterPath tick;
        tick.moveTo({box.x + 3.5f, box.y + 7.5f});
        tick.lineTo({box.x + 6.0f, box.y + 10.0f});
        tick.lineTo({box.x + 10.5f, box.y + 4.5f});
        Pen pen(theme.accentText, 1.8f);
        pen.cap = CapStyle::Round;
        pen.join = JoinStyle::Round;
        painter.strokePath(tick, pen);
    } else {
        painter.fillPath(shape, isHovered() ? theme.controlHover : theme.control);
        painter.strokePath(shape, Pen(theme.border, 1.0f));
    }
    if (hasFocus()) {
        focusRing(painter, box.grownBy(2, 2, 2, 2), theme);
    }
    if (!m_laidOut && !m_text.empty()) {
        m_laidOut = layoutIn(m_layout, m_text, theme, kInfinity, false, TextAlign::Start);
    }
    if (m_laidOut) {
        const Vec2 origin = centredOrigin(m_layout, r);
        painter.drawText(m_layout, {box.right() + theme.spacing, origin.y}, enabled ? theme.text : theme.textDisabled);
    }
}

bool CheckBox::onPointer(const PointerEvent &event) {
    if (event.button != PointerButton::Left) {
        return false;
    }
    if (event.type == PointerEvent::Type::Release && isPressed() && isHovered()) {
        toggle();
    }
    return event.type == PointerEvent::Type::Press || event.type == PointerEvent::Type::Release;
}

bool CheckBox::onKey(const KeyEvent &event) {
    if (activates(event)) {
        toggle();
        return true;
    }
    return false;
}

// ---- NumberField -----------------------------------------------------------------

NumberField::NumberField(double value, int decimals) : m_value(value), m_decimals(std::clamp(decimals, 0, 10)) {
    showValue();
    m_finished = editingFinished.connect([this] {
        const String typed = text();
        char *end = nullptr;
        const double parsed = std::strtod(typed.c_str(), &end);
        if (end && end != typed.c_str() && trim(StringView(end)).empty() && std::isfinite(parsed)) {
            commit(parsed);
        } else {
            showValue(); // not a number: back to the last value
        }
    });
}

void NumberField::setRange(double minimum, double maximum) {
    m_minimum = std::min(minimum, maximum);
    m_maximum = std::max(minimum, maximum);
    setValue(m_value);
}

void NumberField::setDecimals(int decimals) {
    m_decimals = std::clamp(decimals, 0, 10);
    setValue(m_value);
}

void NumberField::setValue(double value) {
    const double scale = std::pow(10.0, m_decimals);
    m_value = std::round(std::clamp(value, m_minimum, m_maximum) * scale) / scale;
    showValue();
}

void NumberField::showValue() {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", m_decimals, m_value);
    setText(buffer == StringView("-0") ? "0" : buffer);
}

void NumberField::commit(double value) {
    const double before = m_value;
    setValue(value);
    if (m_value != before) {
        valueChanged.emit(m_value);
    }
}

bool NumberField::onKey(const KeyEvent &event) {
    if (event.type == KeyEvent::Type::Press && (event.key == Key::Up || event.key == Key::Down)) {
        const double steps = hasModifier(event.modifiers, Modifier::Shift) ? 10.0 : 1.0;
        commit(m_value + (event.key == Key::Up ? steps : -steps) * m_step);
        selectAll();
        return true;
    }
    return TextField::onKey(event);
}

bool NumberField::onPointer(const PointerEvent &event) {
    if (event.type == PointerEvent::Type::Wheel && hasFocus() && event.wheelDelta.y != 0.0f) {
        commit(m_value + (event.wheelDelta.y > 0 ? m_step : -m_step));
        return true;
    }
    return TextField::onPointer(event);
}

// ---- MenuItem and Menu ----------------------------------------------------------

MenuItem::MenuItem(String text, String shortcut) : m_text(std::move(text)), m_shortcut(std::move(shortcut)) {
    setRole(Role::MenuItem);
    setAccessibleName(m_text);
    setFocusable(true);
}

void MenuItem::setText(String text) {
    m_text = std::move(text);
    setAccessibleName(m_text);
    invalidateLayout();
    invalidatePaint();
}

void MenuItem::setShortcutText(String shortcut) {
    m_shortcut = std::move(shortcut);
    invalidateLayout();
    invalidatePaint();
}

void MenuItem::setIcon(Icon icon) {
    m_icon = std::move(icon);
    invalidatePaint();
}

void MenuItem::setSubmenu(std::function<void(Menu &)> fill) {
    m_submenu = std::move(fill);
    invalidateLayout();
    invalidatePaint();
}

Menu *MenuItem::menu() const { return dynamic_cast<Menu *>(parent()); }

Vec2 MenuItem::measureContent(Vec2) {
    const Theme &t = theme();
    float width = 0.0f;
    if (layoutIn(m_layout, m_text, t, kInfinity, false, TextAlign::Start)) {
        width = std::ceil(m_layout.size().x);
        if (!m_shortcut.empty() && layoutIn(m_shortcutLayout, m_shortcut, t, kInfinity, false, TextAlign::Start)) {
            width += 3 * t.padding + std::ceil(m_shortcutLayout.size().x);
        }
    }
    if (m_submenu) {
        width += 2 * t.padding; // the arrow
    }
    return {width + 2 * t.padding + kBox + t.spacing, t.controlHeight};
}

void MenuItem::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    const bool enabled = isEnabled();
    const Menu *owner = menu();
    const bool open = owner && owner->m_submenuItem == this;
    const bool lit = enabled && (isHovered() || hasFocus() || open);
    if (lit) {
        PainterPath shape;
        shape.addRoundedRect(r.grownBy(-3, -1, -3, -1), theme.radius, theme.radius);
        painter.fillPath(shape, theme.accent);
    }
    const Color text = !enabled ? theme.textDisabled : lit ? theme.accentText : theme.text;
    const float boxX = r.x + theme.padding;
    if (!m_icon.isNull()) {
        const float y = std::round(r.y + (r.height - kBox) / 2.0f);
        m_icon.paint(painter, {boxX, y, kBox, kBox}, text);
    } else if (m_checked) {
        PainterPath tick;
        const float x = boxX;
        const float y = r.y + r.height / 2.0f;
        tick.moveTo({x + 1, y});
        tick.lineTo({x + 4, y + 3});
        tick.lineTo({x + 9, y - 3});
        painter.strokePath(tick, Pen(text, 1.6f));
    }
    if (m_submenu) {
        PainterPath arrow;
        const float x = r.right() - theme.padding - 3.0f;
        const float y = r.y + r.height / 2.0f;
        arrow.moveTo({x - 3, y - 4});
        arrow.lineTo({x + 1, y});
        arrow.lineTo({x - 3, y + 4});
        Pen pen(text, 1.5f);
        pen.cap = CapStyle::Round;
        painter.strokePath(arrow, pen);
    }
    if (!theme.font) {
        return;
    }
    const Vec2 origin = centredOrigin(m_layout, r);
    painter.drawText(m_layout, {r.x + theme.padding + kBox + theme.spacing, origin.y}, text);
    if (!m_shortcut.empty()) {
        painter.drawText(m_shortcutLayout, {r.right() - theme.padding - m_shortcutLayout.size().x, origin.y},
                         !enabled ? theme.textDisabled : lit ? theme.accentText : theme.textMuted);
    }
}

void MenuItem::onHoverChanged(bool hovered) {
    invalidatePaint();
    Menu *owner = menu();
    if (!hovered || !owner || !isEnabled()) {
        return;
    }
    // Hovering an item opens its submenu, and closes a sibling's.
    if (m_submenu) {
        if (owner->m_submenuItem != this) {
            owner->openSubmenuOf(*this, false);
        }
    } else {
        owner->closeSubmenu();
    }
}

bool MenuItem::onPointer(const PointerEvent &event) {
    if (event.type == PointerEvent::Type::Release && isHovered()) {
        if (m_submenu) {
            if (Menu *owner = menu(); owner && owner->m_submenuItem != this) {
                owner->openSubmenuOf(*this, false);
            }
        } else {
            activated.emit();
        }
        return true;
    }
    return event.type == PointerEvent::Type::Press;
}

bool MenuItem::onKey(const KeyEvent &event) {
    if (m_submenu && event.type == KeyEvent::Type::Press &&
        (event.key == Key::Right || event.key == Key::Enter || event.key == Key::Space)) {
        if (Menu *owner = menu()) {
            owner->openSubmenuOf(*this, true);
        }
        return true;
    }
    if (!m_submenu && activates(event)) {
        activated.emit();
        return true;
    }
    return false;
}

Menu::Menu() : Stack(Direction::Column, 0.0f, 4.0f) { setRole(Role::Menu); }

MenuItem &Menu::addItem(String text, std::function<void()> action, String shortcut) {
    MenuItem &item = add<MenuItem>(std::move(text), std::move(shortcut));
    // Closing destroys the menu (and the item), so the action runs after.
    static_cast<void>(item.activated.connect([this, action = std::move(action)] {
        const std::function<void()> run = action;
        closeAll();
        if (run) {
            run();
        }
    }));
    return item;
}

MenuItem &Menu::addItem(Icon icon, String text, std::function<void()> action, String shortcut) {
    MenuItem &item = addItem(std::move(text), std::move(action), std::move(shortcut));
    item.setIcon(std::move(icon));
    return item;
}

MenuItem &Menu::addSubmenu(String text, std::function<void(Menu &)> fill, Icon icon) {
    MenuItem &item = add<MenuItem>(std::move(text));
    item.setSubmenu(std::move(fill));
    item.setIcon(std::move(icon));
    return item;
}

void Menu::addSeparator() {
    Element &line = add<Element>();
    line.setFixedSize({0.0f, 9.0f});
    line.setAccessibleName("separator");
}

Menu &Menu::popup(Surface &surface, std::unique_ptr<Menu> menu, Vec2 position) {
    Menu &opened = static_cast<Menu &>(surface.openPopup(std::move(menu), position));
    return opened;
}

void Menu::focusFirstItem() {
    for (const auto &child : children()) {
        if (child->isFocusable() && child->isEnabled() && child->isVisible()) {
            if (Surface *s = surface()) {
                s->setFocus(child.get());
            }
            return;
        }
    }
}

void Menu::closeAll() {
    Menu *root = this;
    while (root->m_parentMenu) {
        root = root->m_parentMenu;
    }
    if (Surface *s = root->surface()) {
        s->closePopup(*root); // closes the submenus above it too
    }
}

void Menu::openSubmenuOf(MenuItem &item, bool focusFirst) {
    Surface *s = surface();
    if (!s || !item.m_submenu) {
        return;
    }
    closeSubmenu();
    auto submenu = std::make_unique<Menu>();
    submenu->m_parentMenu = this;
    item.m_submenu(*submenu);
    const Vec2 at{rect().right() - 4.0f, item.rect().y - 4.0f};
    Menu *opened = submenu.get();
    s->openPopup(std::move(submenu), at, [this, opened] {
        if (m_openSubmenu == opened) {
            m_openSubmenu = nullptr;
            if (m_submenuItem) {
                m_submenuItem->invalidatePaint();
            }
            m_submenuItem = nullptr;
        }
    });
    m_openSubmenu = opened;
    m_submenuItem = &item;
    item.invalidatePaint();
    if (focusFirst) {
        s->layout();
        opened->focusFirstItem();
    }
}

void Menu::closeSubmenu() {
    if (m_openSubmenu) {
        if (Surface *s = surface()) {
            s->closePopup(*m_openSubmenu); // its onClosed clears the pointers
        }
    }
}

void Menu::paint(Painter &painter, const Theme &theme) {
    PainterPath shape;
    shape.addRoundedRect(rect(), theme.radius + 2, theme.radius + 2);
    painter.fillPath(shape, theme.panel);
    painter.strokePath(shape, Pen(theme.border, 1.0f));
    for (const auto &child : children()) {
        if (child->accessibleName() == "separator") {
            const RectF r = child->rect();
            const float y = std::round(r.y + r.height / 2.0f) + 0.5f;
            painter.drawLine({r.x + 6, y}, {r.right() - 6, y}, Pen(theme.border, 1.0f));
        }
    }
}

bool Menu::onKey(const KeyEvent &event) {
    if (event.type != KeyEvent::Type::Press) {
        return false;
    }
    if (event.key == Key::Left || event.key == Key::Right) {
        if (event.key == Key::Left && m_parentMenu) {
            Menu *parent = m_parentMenu;
            MenuItem *item = parent->m_submenuItem;
            parent->closeSubmenu(); // destroys this menu: touch nothing of it after
            if (item && parent->surface()) {
                parent->surface()->setFocus(item);
            }
            return true;
        }
        Menu *root = this;
        while (root->m_parentMenu) {
            root = root->m_parentMenu;
        }
        if (root->sideways) {
            const std::function<void(int)> move = root->sideways;
            move(event.key == Key::Left ? -1 : 1);
            return true;
        }
        return true;
    }
    if (event.key != Key::Up && event.key != Key::Down) {
        return false;
    }
    std::vector<Element *> items;
    for (const auto &child : children()) {
        if (child->isFocusable() && child->isEnabled() && child->isVisible()) {
            items.push_back(child.get());
        }
    }
    if (items.empty() || !surface()) {
        return true;
    }
    auto it = std::find(items.begin(), items.end(), surface()->focus());
    std::size_t index = 0;
    if (it != items.end()) {
        const std::size_t at = std::size_t(it - items.begin());
        index = event.key == Key::Down ? (at + 1) % items.size() : (at + items.size() - 1) % items.size();
    } else if (event.key == Key::Up) {
        index = items.size() - 1;
    }
    surface()->setFocus(items[index]);
    return true;
}

// ---- Dropdown --------------------------------------------------------------------

Dropdown::Dropdown(std::vector<String> items, int current) : m_items(std::move(items)) {
    setRole(Role::ComboBox);
    setFocusable(true);
    setCurrentIndex(current);
}

Dropdown::~Dropdown() {
    if (m_popup && m_popupSurface) {
        Element *popup = m_popup;
        m_popup = nullptr;
        m_popupSurface->closePopup(*popup); // its callbacks point at this dropdown
    }
}

void Dropdown::setItems(std::vector<String> items) {
    m_items = std::move(items);
    setCurrentIndex(m_current);
    invalidateLayout();
}

void Dropdown::setCurrentIndex(int index) {
    m_current = m_items.empty() || index < 0 ? -1 : std::min(index, int(m_items.size()) - 1);
    setAccessibleName(currentText());
    invalidatePaint();
}

String Dropdown::currentText() const { return m_current >= 0 ? m_items[std::size_t(m_current)] : String(); }

void Dropdown::setPlaceholder(String placeholder) {
    m_placeholder = std::move(placeholder);
    invalidatePaint();
}

void Dropdown::setSearchable(bool searchable, String placeholder) {
    m_searchable = searchable;
    m_searchPlaceholder = std::move(placeholder);
}

void Dropdown::choose(int index) {
    if (index == m_current || index < 0 || index >= int(m_items.size())) {
        return;
    }
    setCurrentIndex(index);
    currentChanged.emit(m_current);
}

Vec2 Dropdown::measureContent(Vec2) {
    const Theme &t = theme();
    float widest = 0.0f;
    TextLayout layout;
    for (const String &item : m_items) {
        if (layoutIn(layout, item, t, kInfinity, false, TextAlign::Start)) {
            widest = std::max(widest, std::ceil(layout.size().x));
        }
    }
    return {widest + 3 * t.padding + 12.0f, t.controlHeight};
}

void Dropdown::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    PainterPath shape;
    shape.addRoundedRect(r, theme.radius, theme.radius);
    const bool enabled = isEnabled();
    painter.fillPath(shape, !enabled ? theme.controlDisabled : isHovered() || isOpen() ? theme.controlHover : theme.control);
    if (hasFocus()) {
        focusRing(painter, r, theme);
    }
    TextLayout layout;
    const bool placeholder = m_current < 0;
    if (layoutIn(layout, placeholder ? m_placeholder : currentText(), theme, kInfinity, false, TextAlign::Start)) {
        painter.save();
        painter.clipRect(r.grownBy(-theme.padding, 0, -theme.padding - 16.0f, 0));
        const Vec2 origin = centredOrigin(layout, r);
        painter.drawText(layout, {r.x + theme.padding, origin.y},
                         !enabled ? theme.textDisabled : placeholder ? theme.textMuted : theme.text);
        painter.restore();
    }
    chevron(painter, {r.right() - theme.padding - 4.0f, r.y + r.height / 2.0f}, enabled ? theme.textMuted : theme.textDisabled);
}

void Dropdown::open() {
    Surface *s = surface();
    if (!s || m_items.empty() || m_popup) {
        return;
    }
    auto list = std::make_unique<Menu>();
    TextField *search = nullptr;
    if (m_searchable) {
        search = &list->add<TextField>();
        search->setPlaceholder(m_searchPlaceholder);
    }
    std::vector<MenuItem *> items;
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        MenuItem &item = list->addItem(m_items[i], [this, i] { choose(int(i)); });
        item.setChecked(int(i) == m_current);
        items.push_back(&item);
    }
    list->setFixedSize({rect().width, 0.0f});
    m_popupSurface = s;
    m_popup = &s->openPopup(std::move(list), {rect().x, rect().bottom() + 2.0f}, [this] {
        m_popup = nullptr;
        invalidatePaint();
    });
    if (search) {
        // Typing hides what does not match; Enter takes the first match.
        static_cast<void>(search->textChanged.connect([items](const String &text) {
            const String needle = toLowerAscii(trim(text));
            for (MenuItem *item : items) {
                item->setVisible(needle.empty() || toLowerAscii(item->text()).find(needle) != String::npos);
            }
        }));
        static_cast<void>(search->submitted.connect([items](const String &) {
            for (MenuItem *item : items) {
                if (item->isVisible()) {
                    item->activated.emit();
                    return;
                }
            }
        }));
        s->setFocus(search);
    } else if (m_current >= 0 && std::size_t(m_current) < items.size()) {
        // Focus the current item, so the keyboard continues in the list.
        s->setFocus(items[std::size_t(m_current)]);
    }
    invalidatePaint();
}

bool Dropdown::onPointer(const PointerEvent &event) {
    if (event.button != PointerButton::Left) {
        return false;
    }
    if (event.type == PointerEvent::Type::Press) {
        open();
        return true;
    }
    return event.type == PointerEvent::Type::Release;
}

bool Dropdown::onKey(const KeyEvent &event) {
    if (event.type != KeyEvent::Type::Press) {
        return false;
    }
    if (activates(event)) {
        open();
        return true;
    }
    if (event.key == Key::Up || event.key == Key::Down) {
        choose(m_current + (event.key == Key::Down ? 1 : -1));
        return true;
    }
    return false;
}

} // namespace cfw
