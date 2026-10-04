#include "cfw/ui/Surface.h"

#include <algorithm>
#include <vector>

#include "cfw/gfx/Painter.h"
#include "cfw/ui/Controls.h"

namespace cfw {

namespace {

bool contains(const Element &ancestor, const Element *element) {
    for (const Element *e = element; e; e = e->parent()) {
        if (e == &ancestor) {
            return true;
        }
    }
    return false;
}

bool inside(const RectF &r, Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.right() && p.y < r.bottom(); }

void collectFocusable(Element &element, std::vector<Element *> &out) {
    if (!element.isVisible() || !element.isEnabled()) {
        return;
    }
    if (element.isFocusable()) {
        out.push_back(&element);
    }
    for (const auto &child : element.children()) {
        collectFocusable(*child, out);
    }
}

Element *hitTestIn(Element &element, Vec2 point) {
    if (!element.isVisible() || !inside(element.rect(), point)) {
        return nullptr;
    }
    const auto children = element.children();
    for (std::size_t i = children.size(); i-- > 0;) { // topmost (last painted) first
        if (Element *hit = hitTestIn(*children[i], point)) {
            return hit;
        }
    }
    return &element;
}

// The small panel a tooltip is: one muted-border box with the text.
class ToolTipBox : public Element {
public:
    explicit ToolTipBox(String text) {
        auto &label = add<Label>(std::move(text));
        label.setWrap(true);
        setRole(Role::Label);
    }
    void paint(Painter &painter, const Theme &theme) override {
        PainterPath box;
        box.addRoundedRect(rect(), theme.radius, theme.radius);
        painter.fillPath(box, theme.panel);
        Pen pen(theme.border, 1.0f);
        PainterPath outline;
        outline.addRoundedRect({rect().x + 0.5f, rect().y + 0.5f, rect().width - 1.0f, rect().height - 1.0f},
                               theme.radius, theme.radius);
        painter.strokePath(outline, pen);
    }

protected:
    Vec2 measureContent(Vec2 available) override {
        constexpr float kMaxWidth = 360.0f;
        const float pad = theme().spacing;
        const Vec2 text = children()[0]->measure({std::min(available.x, kMaxWidth) - 2 * pad, available.y});
        return {text.x + 2 * pad, text.y + pad};
    }
    void arrangeContent(const RectF &r) override {
        const float pad = theme().spacing;
        children()[0]->arrange({r.x + pad, r.y + pad / 2, r.width - 2 * pad, r.height - pad});
    }
};

// A quick second tooltip: moving to a neighbour soon after one closed shows
// its tooltip at once, as Qt does.
constexpr Duration kToolTipFallThrough = std::chrono::milliseconds(500);

} // namespace

Surface::Surface(Theme theme) : m_theme(std::move(theme)) { setRoot(std::make_unique<Element>()); }

Surface::~Surface() {
    // Popups first: their owners (still in the tree) are told they closed.
    closePopups();
    m_closed.clear();
    m_timers.clear();
    m_root.reset();
}

void Surface::setRoot(std::unique_ptr<Element> root) {
    if (m_root) {
        elementRemoved(*m_root);
        m_root->setSurface(nullptr);
    }
    m_root = root ? std::move(root) : std::make_unique<Element>();
    m_root->setSurface(this);
    m_root->invalidateLayout();
    addDamage({0, 0, m_size.x, m_size.y});
}

void Surface::setSize(Vec2 size) {
    if (size == m_size) {
        return;
    }
    m_size = size;
    m_root->invalidateLayout();
    addDamage({0, 0, size.x, size.y});
}

void Surface::setTheme(Theme theme) {
    m_theme = std::move(theme);
    // Everything measures from the theme, so everything is invalid.
    std::vector<Element *> stack{m_root.get()};
    for (Popup &popup : m_popups) {
        stack.push_back(popup.element.get());
    }
    while (!stack.empty()) {
        Element *e = stack.back();
        stack.pop_back();
        e->m_measuredFor.reset();
        for (const auto &child : e->children()) {
            stack.push_back(child.get());
        }
    }
    m_layoutDirty = true;
    addDamage({0, 0, m_size.x, m_size.y});
}

void Surface::layout() {
    m_closed.clear();
    if (!m_layoutDirty) {
        return;
    }
    m_layoutDirty = false;
    static_cast<void>(m_root->measure(m_size));
    m_root->arrange({0, 0, m_size.x, m_size.y});
    for (Popup &popup : m_popups) {
        const Vec2 size = popup.element->measure(m_size);
        const float w = std::min(size.x, m_size.x);
        const float h = std::min(size.y, m_size.y);
        float x = popup.position.x;
        float y = popup.position.y;
        if (popup.options.centred) {
            x = std::round((m_size.x - w) / 2.0f);
            y = std::round((m_size.y - h) / 2.0f);
        }
        x = std::clamp(x, 0.0f, std::max(0.0f, m_size.x - w));
        // Below the anchor if it fits, else as low as it fits.
        if (y + h > m_size.y) {
            y = std::max(0.0f, m_size.y - h);
        }
        popup.element->arrange({x, y, w, h});
        addDamage(popup.element->rect());
    }
}

Element &Surface::openPopup(std::unique_ptr<Element> popup, Vec2 position, std::function<void()> onClosed) {
    return openPopup(std::move(popup), position, PopupOptions{}, std::move(onClosed));
}

Element &Surface::openPopup(std::unique_ptr<Element> popup, Vec2 position, const PopupOptions &options,
                            std::function<void()> onClosed) {
    if (options.takesInput) {
        hideToolTip();
    }
    popup->setSurface(this);
    m_popups.push_back({std::move(popup), position, std::move(onClosed), options});
    m_layoutDirty = true;
    Element &element = *m_popups.back().element;
    element.invalidateLayout();
    if (options.modal) {
        addDamage({0, 0, m_size.x, m_size.y}); // the scrim
        std::vector<Element *> focusable;
        collectFocusable(element, focusable);
        setFocus(focusable.empty() ? nullptr : focusable.front());
        if (m_pressed && !contains(element, m_pressed)) {
            Element *released = m_pressed;
            m_pressed = nullptr;
            released->onPressedChanged(false);
        }
        setHovered(nullptr);
    }
    return element;
}

void Surface::closePopup(Element &popup) {
    const auto it = std::find_if(m_popups.begin(), m_popups.end(),
                                 [&popup](const Popup &p) { return p.element.get() == &popup; });
    if (it == m_popups.end()) {
        return;
    }
    // Popups are a stack: what was opened above this one (a submenu, a
    // dropdown's list in a dialog, a tooltip) closes first.
    const std::size_t index = std::size_t(it - m_popups.begin());
    while (m_popups.size() > index + 1) {
        closePopup(*m_popups.back().element);
    }
    Popup closing = std::move(m_popups[index]);
    m_popups.erase(m_popups.begin() + std::ptrdiff_t(index));
    if (closing.element.get() == m_toolTip) {
        m_toolTip = nullptr;
        m_toolTipFor = nullptr;
    }
    elementRemoved(*closing.element);
    addDamage(closing.options.modal ? RectF{0, 0, m_size.x, m_size.y} : closing.element->rect());
    closing.element->setSurface(nullptr);
    m_closed.push_back(std::move(closing.element));
    m_layoutDirty = true;
    if (closing.onClosed) {
        closing.onClosed();
    }
}

void Surface::closePopups() {
    while (!m_popups.empty()) {
        closePopup(*m_popups.back().element);
    }
}

std::ptrdiff_t Surface::topModal() const noexcept {
    for (std::size_t i = m_popups.size(); i-- > 0;) {
        if (m_popups[i].options.modal) {
            return std::ptrdiff_t(i);
        }
    }
    return -1;
}

bool Surface::acceptsInput(const Element *element) const noexcept {
    const std::ptrdiff_t modal = topModal();
    if (modal < 0) {
        return true;
    }
    for (std::size_t i = std::size_t(modal); i < m_popups.size(); ++i) {
        if (contains(*m_popups[i].element, element)) {
            return true;
        }
    }
    return false;
}

void Surface::paint(Painter &painter) {
    layout();
    painter.fillRect({0, 0, m_size.x, m_size.y}, m_theme.window);
    paintTree(painter, *m_root);
    for (Popup &popup : m_popups) {
        if (popup.options.modal) {
            painter.fillRect({0, 0, m_size.x, m_size.y}, m_theme.scrim);
        }
        paintTree(painter, *popup.element);
    }
}

void Surface::paintTree(Painter &painter, Element &element) {
    if (!element.isVisible() || element.rect().isEmpty()) {
        return;
    }
    element.paint(painter, m_theme);
    if (!element.children().empty()) {
        painter.save();
        if (element.clipsChildren()) {
            painter.clipRect(element.rect());
        }
        for (const auto &child : element.children()) {
            paintTree(painter, *child);
        }
        painter.restore();
    }
    element.paintOverlay(painter, m_theme);
}

String Surface::clipboardText() const { return readClipboard ? readClipboard() : m_clipboard; }

void Surface::setClipboardText(StringView text) {
    if (writeClipboard) {
        writeClipboard(text);
    } else {
        m_clipboard = String(text);
    }
}

RectF Surface::takeDamage() {
    const RectF damage = m_damage.intersected({0, 0, m_size.x, m_size.y});
    m_damage = {};
    return damage;
}

void Surface::addDamage(const RectF &rect) { m_damage = m_damage.united(rect); }

void Surface::elementRemoved(Element &element) {
    if (contains(element, m_hovered)) {
        m_hovered = nullptr;
    }
    if (contains(element, m_pressed)) {
        m_pressed = nullptr;
    }
    if (contains(element, m_focus)) {
        m_focus = nullptr;
    }
    if (contains(element, m_dropTarget)) {
        m_dropTarget = nullptr;
    }
    if (m_toolTipFor && contains(element, m_toolTipFor)) {
        m_toolTipFor = nullptr;
        if (m_toolTip) {
            // Deferred: the tooltip is a popup, and this may be mid-close.
            Element *tip = m_toolTip;
            m_toolTip = nullptr;
            const auto it = std::find_if(m_popups.begin(), m_popups.end(),
                                         [tip](const Popup &p) { return p.element.get() == tip; });
            if (it != m_popups.end() && it->element.get() != &element) {
                closePopup(*tip);
            }
        }
    }
}

Element *Surface::hitTest(Vec2 point) {
    layout();
    const std::ptrdiff_t modal = topModal();
    for (std::size_t i = m_popups.size(); i-- > 0;) {
        if (!m_popups[i].options.takesInput) {
            continue;
        }
        if (Element *hit = hitTestIn(*m_popups[i].element, point)) {
            return hit;
        }
        if (std::ptrdiff_t(i) == modal) {
            return nullptr; // nothing under a dialog is reachable
        }
    }
    return hitTestIn(*m_root, point);
}

void Surface::setHovered(Element *element) {
    if (element == m_hovered) {
        return;
    }
    Element *old = m_hovered;
    m_hovered = element;
    if (old) {
        old->onHoverChanged(false);
    }
    if (element) {
        element->onHoverChanged(true);
    }
}

// Checks, when a dispatch or focus change is over, whether the node with
// the keyboard changed, and says so to whoever listens.
struct AccessibleFocusWatch {
    explicit AccessibleFocusWatch(Surface &s) : surface(s) {}
    ~AccessibleFocusWatch() {
        if (surface.accessibleFocusChanged.connectionCount() > 0) {
            surface.checkAccessibleFocus();
        }
    }
    AccessibleFocusWatch(const AccessibleFocusWatch &) = delete;
    AccessibleFocusWatch &operator=(const AccessibleFocusWatch &) = delete;
    Surface &surface;
};

void Surface::setFocus(Element *element) {
    const AccessibleFocusWatch watch(*this);
    if (element == m_focus) {
        return;
    }
    Element *old = m_focus;
    m_focus = element;
    if (old) {
        old->onFocusChanged(false);
    }
    if (element) {
        element->onFocusChanged(true);
    }
}

void Surface::focusNext(bool backwards) {
    std::vector<Element *> order;
    const std::ptrdiff_t modal = topModal();
    collectFocusable(modal >= 0 ? *m_popups[std::size_t(modal)].element : *m_root, order);
    if (order.empty()) {
        return;
    }
    std::size_t index = 0;
    const auto current = std::find(order.begin(), order.end(), m_focus);
    if (current == order.end()) {
        index = backwards ? order.size() - 1 : 0;
    } else {
        const std::size_t at = std::size_t(current - order.begin());
        index = backwards ? (at + order.size() - 1) % order.size() : (at + 1) % order.size();
    }
    setFocus(order[index]);
}

// ---- Timers ------------------------------------------------------------------

Surface::TimerId Surface::startTimer(Duration delay, std::function<void()> callback, bool repeat) {
    const TimerId id = m_nextTimer++;
    m_timers.push_back({id, now() + delay, delay, repeat, std::move(callback)});
    return id;
}

void Surface::stopTimer(TimerId id) {
    std::erase_if(m_timers, [id](const Timer &t) { return t.id == id; });
}

void Surface::runTimers() {
    const TimePoint time = now();
    // Callbacks may start and stop timers: collect the due ones first, and
    // run each only if it is still there.
    std::vector<TimerId> due;
    for (const Timer &timer : m_timers) {
        if (timer.due <= time) {
            due.push_back(timer.id);
        }
    }
    for (const TimerId id : due) {
        const auto it = std::find_if(m_timers.begin(), m_timers.end(), [id](const Timer &t) { return t.id == id; });
        if (it == m_timers.end()) {
            continue;
        }
        std::function<void()> callback;
        if (it->repeat) {
            // Keep the cadence without piling up after a stall.
            it->due = std::max(it->due + it->interval, time);
            callback = it->callback;
        } else {
            callback = std::move(it->callback);
            m_timers.erase(it);
        }
        callback();
    }
    m_closed.clear();
}

std::optional<TimePoint> Surface::nextTimer() const {
    std::optional<TimePoint> next;
    for (const Timer &timer : m_timers) {
        if (!next || timer.due < *next) {
            next = timer.due;
        }
    }
    return next;
}

// ---- Shortcuts, cursor, tooltips ----------------------------------------------

Surface::ShortcutId Surface::addShortcut(KeyChord chord, std::function<void()> action) {
    const ShortcutId id = m_nextShortcut++;
    m_shortcuts.push_back({id, chord, std::move(action)});
    return id;
}

void Surface::removeShortcut(ShortcutId id) {
    std::erase_if(m_shortcuts, [id](const Shortcut &s) { return s.id == id; });
}

Cursor Surface::cursor() {
    Element *under = m_pressed;
    if (!under && m_pointerInside) {
        under = hitTest(m_pointer);
    }
    for (const Element *e = under; e; e = e->parent()) {
        if (const std::optional<Cursor> shape = e->cursorAt(m_pointer)) {
            return *shape;
        }
    }
    return Cursor::Arrow;
}

void Surface::hideToolTip() {
    if (m_toolTipTimer) {
        stopTimer(m_toolTipTimer);
        m_toolTipTimer = 0;
    }
    if (m_toolTip) {
        Element *tip = m_toolTip;
        m_toolTip = nullptr;
        m_toolTipHidden = now();
        closePopup(*tip);
    }
    m_toolTipFor = nullptr;
}

void Surface::scheduleToolTip() {
    Element *owner = nullptr;
    for (Element *e = m_hovered; e; e = e->parent()) {
        if (!e->toolTip().empty()) {
            owner = e;
            break;
        }
    }
    if (owner == m_toolTipFor && (m_toolTip || m_toolTipTimer)) {
        return; // already showing, or about to, for this element
    }
    const bool quick = m_toolTip != nullptr || (m_toolTipHidden != TimePoint{} &&
                                                now() - m_toolTipHidden < kToolTipFallThrough);
    hideToolTip();
    if (!owner) {
        return;
    }
    m_toolTipFor = owner;
    const auto show = [this] {
        m_toolTipTimer = 0;
        if (!m_toolTipFor || m_pressed) {
            return;
        }
        PopupOptions options;
        options.takesInput = false;
        m_toolTip = &openPopup(std::make_unique<ToolTipBox>(m_toolTipFor->toolTip()),
                               {m_pointer.x, m_pointer.y + 20.0f}, options);
    };
    if (quick) {
        show();
    } else {
        m_toolTipTimer = startTimer(kToolTipDelay, show);
    }
}

// ---- Input ---------------------------------------------------------------------

bool Surface::dispatch(const CompositionEvent &event) {
    const AccessibleFocusWatch watch(*this);
    m_closed.clear();
    layout();
    return m_focus && acceptsInput(m_focus) && m_focus->onComposition(event);
}

std::optional<RectF> Surface::textInputArea() const {
    if (!m_focus || !m_focus->isVisible() || !m_focus->isEnabled()) {
        return std::nullopt;
    }
    return m_focus->textInputArea();
}

bool Surface::dispatch(const DropEvent &event) {
    const AccessibleFocusWatch watch(*this);
    m_closed.clear();
    layout();
    const auto leaveTarget = [this, &event] {
        if (m_dropTarget) {
            DropEvent leave;
            leave.type = DropEvent::Type::Leave;
            leave.position = event.position;
            Element *target = m_dropTarget;
            m_dropTarget = nullptr;
            target->onDrop(leave);
        }
    };
    if (event.type == DropEvent::Type::Leave) {
        leaveTarget();
        return false;
    }
    // The element under the point, or the nearest ancestor taking the files.
    // The current target is asked with a Move, a new one with an Enter.
    Element *candidate = hitTest(event.position);
    while (candidate && (!candidate->isEnabled() || !acceptsInput(candidate))) {
        candidate = candidate->parent();
    }
    Element *target = nullptr;
    for (; candidate; candidate = candidate->parent()) {
        DropEvent asked = event;
        if (candidate != m_dropTarget) {
            asked.type = DropEvent::Type::Enter;
        } else if (event.type == DropEvent::Type::Enter) {
            asked.type = DropEvent::Type::Move;
        }
        if (event.type == DropEvent::Type::Drop) {
            asked.type = candidate == m_dropTarget ? DropEvent::Type::Move : DropEvent::Type::Enter;
        }
        if (candidate->onDrop(asked)) {
            target = candidate;
            break;
        }
    }
    if (target != m_dropTarget) {
        leaveTarget();
        m_dropTarget = target;
    }
    if (event.type != DropEvent::Type::Drop) {
        return target != nullptr;
    }
    m_dropTarget = nullptr;
    return target && target->onDrop(event);
}

bool Surface::dispatch(const PointerEvent &event) {
    const AccessibleFocusWatch watch(*this);
    m_closed.clear();
    layout();
    if (event.type == PointerEvent::Type::Cancel) {
        // A touch the system took away: the press ends, but as a release
        // somewhere else would, so nothing is clicked. The pressed element
        // is told through an ordinary Release while it is not hovered.
        hideToolTip();
        m_pointerInside = false;
        setHovered(nullptr);
        bool handled = false;
        if (Element *released = m_pressed) {
            PointerEvent release = event;
            release.type = PointerEvent::Type::Release;
            release.button = PointerButton::Left;
            for (Element *e = released; e && !handled; e = e->parent()) {
                handled = e->onPointer(release);
            }
            if (m_pressed == released) {
                m_pressed = nullptr;
                released->onPressedChanged(false);
            }
        }
        return handled;
    }
    if (event.type == PointerEvent::Type::Leave) {
        m_pointerInside = false;
    } else {
        m_pointer = event.position;
        m_pointerInside = true;
    }
    if (event.type != PointerEvent::Type::Move) {
        hideToolTip();
    }
    Element *hit = event.type == PointerEvent::Type::Leave ? nullptr : hitTest(event.position);
    if (event.type == PointerEvent::Type::Press && !m_pressed && !m_popups.empty()) {
        std::ptrdiff_t inPopup = -1;
        for (std::size_t i = 0; i < m_popups.size(); ++i) {
            if (m_popups[i].options.takesInput && contains(*m_popups[i].element, hit)) {
                inPopup = std::ptrdiff_t(i);
            }
        }
        const std::ptrdiff_t modal = topModal();
        if (inPopup < 0) {
            // The press only dismisses: every popup above the top dialog.
            bool closed = false;
            while (!m_popups.empty() && std::ptrdiff_t(m_popups.size()) - 1 > modal) {
                closePopup(*m_popups.back().element);
                closed = true;
            }
            if (closed || modal >= 0) {
                setHovered(hitTest(event.position));
                return true;
            }
        }
    }
    // Disabled elements are hover- and click-transparent to their enabled parent.
    while (hit && !hit->isEnabled()) {
        hit = hit->parent();
    }
    if (!m_pressed) {
        setHovered(hit);
    } else {
        // While a button is held only the pressed element can be hovered, so
        // dragging off a button and releasing does not click it.
        setHovered(contains(*m_pressed, hit) ? m_pressed : nullptr);
    }
    if (event.type == PointerEvent::Type::Move && !m_pressed) {
        scheduleToolTip();
    }

    // While a button is held, the pressed element gets everything.
    Element *target = m_pressed ? m_pressed : hit;
    if (event.type == PointerEvent::Type::Press && !m_pressed) {
        // Focus goes to the nearest focusable element; pressing elsewhere clears it.
        Element *focusable = hit;
        while (focusable && !focusable->isFocusable()) {
            focusable = focusable->parent();
        }
        setFocus(focusable);
    }

    bool handled = false;
    Element *handler = nullptr;
    for (Element *e = target; e && !handled; e = e->parent()) {
        handled = e->onPointer(event);
        handler = handled ? e : nullptr;
    }

    if (event.type == PointerEvent::Type::Press && handler && !m_pressed && handler->surface() == this) {
        m_pressed = handler;
        handler->onPressedChanged(true);
    } else if (event.type == PointerEvent::Type::Release && m_pressed) {
        Element *released = m_pressed;
        m_pressed = nullptr;
        released->onPressedChanged(false);
        // Handling the release may have rebuilt what is under the pointer
        // (and destroyed `hit`), so look again.
        Element *under = hitTest(event.position);
        while (under && !under->isEnabled()) {
            under = under->parent();
        }
        setHovered(under);
    }
    return handled;
}

bool Surface::dispatch(const KeyEvent &event) {
    const AccessibleFocusWatch watch(*this);
    m_closed.clear();
    if (event.type == KeyEvent::Type::Press) {
        hideToolTip();
    }
    // With nothing focused, a dialog still hears its keys (Enter, Escape).
    Element *start = m_focus;
    if (!start && topModal() >= 0) {
        start = m_popups[std::size_t(topModal())].element.get();
    }
    for (Element *e = start; e; e = e->parent()) {
        if (e->onKey(event)) {
            return true;
        }
    }
    if (event.type != KeyEvent::Type::Press) {
        return false;
    }
    if (!m_popups.empty() && event.key == Key::Escape) {
        closePopup(*m_popups.back().element);
        return true;
    }
    if (event.key == Key::Tab && !hasModifier(event.modifiers, Modifier::Control)) {
        focusNext(hasModifier(event.modifiers, Modifier::Shift));
        return true;
    }
    if (topModal() < 0) {
        for (const Shortcut &shortcut : m_shortcuts) {
            if (shortcut.chord.matches(event)) {
                const std::function<void()> action = shortcut.action; // may remove itself
                action();
                return true;
            }
        }
    }
    return false;
}

bool Surface::dispatch(const TextEvent &event) {
    const AccessibleFocusWatch watch(*this);
    m_closed.clear();
    for (Element *e = m_focus; e; e = e->parent()) {
        if (e->onText(event)) {
            return true;
        }
    }
    return false;
}

// ---- Accessibility ----

std::uint64_t Surface::itemId(std::uint64_t element, std::uint64_t key) {
    const auto [it, inserted] = m_itemIds.try_emplace({element, key}, 0);
    if (inserted) {
        it->second = m_nextItemId++;
    }
    return it->second;
}

void Surface::describeTree(Element &element, std::vector<AccessibleNode> &out,
                           std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> &seen) {
    if (!element.isVisible()) {
        return;
    }
    AccessibleNode node;
    node.id = element.accessibleId();
    node.bounds = element.rect();
    element.describeAccessible(node);
    const bool anonymous = node.role == Role::None && node.name.empty();
    std::vector<AccessibleNode> &children = anonymous ? out : node.children;
    for (const auto &child : element.children()) {
        describeTree(*child, children, seen);
    }
    std::vector<AccessibleNode> items;
    element.accessibleItems(items);
    for (AccessibleNode &item : items) {
        const std::pair key{element.accessibleId(), item.itemKey};
        item.id = itemId(key.first, key.second);
        seen[key] = item.id;
        children.push_back(std::move(item));
    }
    if (!anonymous) {
        out.push_back(std::move(node));
    }
}

AccessibleNode Surface::accessibilityTree() {
    layout();
    AccessibleNode window;
    window.id = 1;
    window.role = Role::Window;
    window.name = accessibleTitle;
    window.bounds = {0, 0, m_size.x, m_size.y};
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> seen;
    describeTree(*m_root, window.children, seen);
    for (const Popup &popup : m_popups) {
        if (popup.options.takesInput) {
            describeTree(*popup.element, window.children, seen);
        }
    }
    // Items no longer shown lose their ids (the focused one keeps its own).
    if (m_focus) {
        if (const std::optional<std::uint64_t> item = m_focus->accessibleFocusedItem()) {
            seen[{m_focus->accessibleId(), *item}] = itemId(m_focus->accessibleId(), *item);
        }
    }
    m_itemIds = std::move(seen);
    return window;
}

Element *Surface::findAccessible(Element &from, std::uint64_t id) {
    if (from.accessibleId() == id) {
        return &from;
    }
    for (const auto &child : from.children()) {
        if (Element *found = findAccessible(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

bool Surface::performAccessibleAction(std::uint64_t id, AccessibleAction action, StringView value) {
    const AccessibleFocusWatch watch(*this);
    m_closed.clear();
    layout();
    std::uint64_t elementId = id;
    std::optional<std::uint64_t> item;
    for (const auto &[key, itemNode] : m_itemIds) {
        if (itemNode == id) {
            elementId = key.first;
            item = key.second;
            break;
        }
    }
    Element *element = findAccessible(*m_root, elementId);
    for (std::size_t i = 0; !element && i < m_popups.size(); ++i) {
        element = findAccessible(*m_popups[i].element, elementId);
    }
    if (!element || !element->isVisible() || !element->isEnabled() || !acceptsInput(element)) {
        return false;
    }
    return element->accessibleAction(action, item, value);
}

std::uint64_t Surface::accessibleFocus() {
    if (!m_focus) {
        return 0;
    }
    if (const std::optional<std::uint64_t> item = m_focus->accessibleFocusedItem()) {
        return itemId(m_focus->accessibleId(), *item);
    }
    return m_focus->accessibleId();
}

void Surface::checkAccessibleFocus() {
    const std::uint64_t focus = accessibleFocus();
    if (focus != m_lastAccessibleFocus) {
        m_lastAccessibleFocus = focus;
        accessibleFocusChanged.emit(focus);
    }
}

} // namespace cfw
