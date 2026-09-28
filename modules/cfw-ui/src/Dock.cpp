#include "cfw/ui/Dock.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "ControlText.h"
#include "cfw/gfx/Painter.h"
#include "cfw/ui/Surface.h"

namespace cfw {

using detail::centredOrigin;
using detail::layoutIn;

namespace {

bool inside(const RectF &r, Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.right() && p.y < r.bottom(); }

constexpr float kDragThreshold = 5.0f;
constexpr float kMinimumCentral = 120.0f;

// Lucide "x" (https://lucide.dev, ISC), in the text colour.
Icon closeIcon() {
    static const Icon icon = Icon::fromSvg(
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" )"
        R"(stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18"/><path d="m6 6 12 12"/></svg>)");
    return icon;
}

char areaLetter(DockArea area) { return area == DockArea::Left ? 'L' : area == DockArea::Right ? 'R' : 'B'; }

std::optional<DockArea> areaFromLetter(StringView text) {
    if (text == "L") return DockArea::Left;
    if (text == "R") return DockArea::Right;
    if (text == "B") return DockArea::Bottom;
    return std::nullopt;
}

std::vector<StringView> split(StringView text, char separator) {
    std::vector<StringView> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(separator, start), text.size());
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::optional<float> number(StringView text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const String copy(text);
    char *end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// The translucent box showing where a dragged panel will go.
class DropIndicator : public Element {
public:
    void paint(Painter &painter, const Theme &theme) override {
        Color fill = theme.accent;
        fill.a = 0.18f;
        painter.fillRect(rect(), fill);
        painter.strokeRect(rect().grownBy(-1, -1, -1, -1), Pen(theme.accent, 2.0f));
    }
};

} // namespace

// ---- DockPanel -----------------------------------------------------------------------

DockPanel::DockPanel(String id, String title, std::unique_ptr<Element> content)
    : m_id(std::move(id)), m_title(std::move(title)) {
    setRole(Role::Group);
    setAccessibleName(m_title);
    setClipsChildren(true);
    m_actions = &add<Stack>(Stack::Direction::Row, 0.0f);
    m_close = &m_actions->add<Button>(closeIcon());
    m_close->setFlat(true);
    m_close->setFocusable(false);
    m_close->setIconSize(14.0f);
    m_close->setToolTip("Close " + m_title);
    m_close->setAccessibleName("Close " + m_title);
    static_cast<void>(m_close->clicked.connect([this] {
        if (DockLayout *dock = layout()) {
            dock->setPanelVisible(*this, false);
        } else {
            setVisible(false);
        }
    }));
    m_content = &add(std::move(content));
}

DockLayout *DockPanel::layout() const { return dynamic_cast<DockLayout *>(parent()); }

Button &DockPanel::addAction(Icon icon, String toolTip, std::function<void()> action) {
    auto button = std::make_unique<Button>(std::move(icon));
    button->setFlat(true);
    button->setFocusable(false);
    button->setIconSize(14.0f);
    button->setAccessibleName(toolTip);
    button->setToolTip(std::move(toolTip));
    if (action) {
        static_cast<void>(button->clicked.connect(std::move(action)));
    }
    // Before the close button: take it out, add the action, put it back.
    std::unique_ptr<Element> close = m_actions->remove(*m_close);
    Button &added = static_cast<Button &>(m_actions->add(std::move(button)));
    m_actions->add(std::move(close));
    invalidateLayout();
    return added;
}

void DockPanel::setTitleBarVisible(bool visible) {
    m_titleBarVisible = visible;
    m_actions->setVisible(visible);
    invalidateLayout();
    invalidatePaint();
}

void DockPanel::setClosable(bool closable) {
    m_close->setVisible(closable);
    invalidateLayout();
}

float DockPanel::titleBarHeight() const { return m_titleBarVisible ? std::round(theme().controlHeight + 2.0f) : 0.0f; }

Vec2 DockPanel::measureContent(Vec2 available) {
    const float title = titleBarHeight();
    const Vec2 content = m_content->measure({available.x, std::max(0.0f, available.y - title)});
    return {content.x, content.y + title};
}

void DockPanel::arrangeContent(const RectF &r) {
    const float title = titleBarHeight();
    if (m_titleBarVisible) {
        // Square buttons at the right end of the title bar.
        const float button = std::round(theme().controlHeight - 4.0f);
        const float top = r.y + (title - button) / 2.0f;
        std::size_t shown = 0;
        for (const auto &child : m_actions->children()) {
            shown += child->isVisible() ? 1u : 0u;
        }
        const float left = r.right() - float(shown) * button - 4.0f;
        m_actions->arrange({left, top, float(shown) * button, button});
        std::size_t placed = 0;
        for (const auto &child : m_actions->children()) {
            if (child->isVisible()) {
                child->arrange({left + float(placed) * button, top, button, button});
                ++placed;
            }
        }
    }
    m_content->arrange({r.x, r.y + title, r.width, std::max(0.0f, r.height - title)});
}

void DockPanel::paint(Painter &painter, const Theme &theme) {
    painter.fillRect(rect(), theme.panel);
    if (!m_titleBarVisible) {
        return;
    }
    const RectF bar{rect().x, rect().y, rect().width, titleBarHeight()};
    painter.fillRect(bar, theme.window);
    painter.drawLine({bar.x, bar.bottom() - 0.5f}, {bar.right(), bar.bottom() - 0.5f}, Pen(theme.border, 1.0f));
    if (layoutIn(m_titleLayout, m_title, theme, std::numeric_limits<float>::infinity(), false, TextAlign::Start)) {
        const Vec2 origin = centredOrigin(m_titleLayout, bar);
        painter.drawText(m_titleLayout, {bar.x + theme.padding, origin.y}, theme.text);
    }
}

bool DockPanel::onPointer(const PointerEvent &event) {
    const bool onTitle = m_titleBarVisible && event.position.y >= rect().y &&
                         event.position.y < rect().y + titleBarHeight();
    DockLayout *dock = layout();
    switch (event.type) {
    case PointerEvent::Type::Press:
        if (!onTitle || event.button != PointerButton::Left || !dock) {
            return false;
        }
        m_pressAt = event.position;
        return true;
    case PointerEvent::Type::Move:
        if (!isPressed() || !m_pressAt || !dock) {
            return false;
        }
        if (dock->m_dragging != this) {
            if ((event.position - *m_pressAt).length() < kDragThreshold) {
                return true;
            }
            dock->beginPanelDrag(*this);
        }
        dock->updatePanelDrag(event.position);
        return true;
    case PointerEvent::Type::Release:
        if (!isPressed()) {
            return false;
        }
        m_pressAt.reset();
        if (dock && dock->m_dragging == this) {
            dock->endPanelDrag(event.position);
        }
        return true;
    default:
        return false;
    }
}

// ---- DockLayout ---------------------------------------------------------------------

DockLayout::DockLayout() {
    setRole(Role::Group);
    setAccessibleName("docks");
}

Element &DockLayout::setCentral(std::unique_ptr<Element> central) {
    if (m_central) {
        static_cast<void>(remove(*m_central));
    }
    m_central = &add(std::move(central));
    invalidateLayout();
    return *m_central;
}

DockPanel &DockLayout::addPanel(std::unique_ptr<DockPanel> panel, DockArea area) {
    DockPanel &added = static_cast<DockPanel &>(add(std::move(panel)));
    m_entries.push_back({&added, area, 1.0f});
    invalidateLayout();
    return added;
}

DockPanel *DockLayout::panel(StringView id) const {
    for (const Entry &entry : m_entries) {
        if (entry.panel->id() == id) {
            return entry.panel;
        }
    }
    return nullptr;
}

std::vector<DockPanel *> DockLayout::panels(DockArea area) const {
    std::vector<DockPanel *> out;
    for (const Entry &entry : m_entries) {
        if (entry.area == area) {
            out.push_back(entry.panel);
        }
    }
    return out;
}

std::ptrdiff_t DockLayout::entryOf(const DockPanel &panel) const {
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].panel == &panel) {
            return std::ptrdiff_t(i);
        }
    }
    return -1;
}

std::optional<DockArea> DockLayout::areaOf(const DockPanel &panel) const {
    const std::ptrdiff_t at = entryOf(panel);
    return at < 0 ? std::nullopt : std::optional<DockArea>(m_entries[std::size_t(at)].area);
}

void DockLayout::setPanelVisible(DockPanel &panel, bool visible) {
    if (panel.isVisible() == visible) {
        return;
    }
    panel.setVisible(visible);
    invalidateLayout();
    invalidatePaint();
    visibilityChanged.emit(panel, visible);
}

void DockLayout::movePanel(DockPanel &panel, DockArea area, std::size_t index) {
    const std::ptrdiff_t at = entryOf(panel);
    if (at < 0) {
        return;
    }
    Entry entry = m_entries[std::size_t(at)];
    m_entries.erase(m_entries.begin() + at);
    entry.area = area;
    entry.share = 1.0f;
    // `index` counts panels already in the area.
    std::size_t seen = 0;
    auto position = m_entries.end();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->area == area) {
            if (seen == index) {
                position = it;
                break;
            }
            ++seen;
        }
    }
    if (position == m_entries.end()) {
        // After the last panel of the area (or at the end).
        for (auto it = m_entries.end(); it != m_entries.begin();) {
            --it;
            if (it->area == area) {
                position = it + 1;
                break;
            }
        }
    }
    m_entries.insert(position, entry);
    invalidateLayout();
    invalidatePaint();
}

void DockLayout::setAreaSize(DockArea area, float size) {
    m_sizes[std::size_t(area)] = std::max(40.0f, size);
    invalidateLayout();
    invalidatePaint();
}

float DockLayout::areaSize(DockArea area) const { return m_sizes[std::size_t(area)]; }

void DockLayout::setPanelShare(DockPanel &panel, float share) {
    const std::ptrdiff_t at = entryOf(panel);
    if (at >= 0) {
        m_entries[std::size_t(at)].share = std::max(0.01f, share);
        invalidateLayout();
    }
}

std::vector<std::size_t> DockLayout::visibleIn(DockArea area) const {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].area == area && m_entries[i].panel->isVisible()) {
            out.push_back(i);
        }
    }
    return out;
}

RectF DockLayout::areaRect(DockArea area) const { return m_areaRects[std::size_t(area)]; }

void DockLayout::arrangeContent(const RectF &r) {
    RectF rest = r;
    for (RectF &area : m_areaRects) {
        area = {};
    }
    const auto sizeOf = [this](DockArea area, float room) {
        float minimum = 0.0f;
        for (const std::size_t i : visibleIn(area)) {
            minimum = std::max(minimum, m_entries[i].panel->minimumExtent());
        }
        return std::clamp(m_sizes[std::size_t(area)], std::min(minimum, room), std::max(0.0f, room));
    };
    if (!visibleIn(DockArea::Bottom).empty()) {
        const float h = sizeOf(DockArea::Bottom, r.height - kMinimumCentral - kGap);
        m_areaRects[std::size_t(DockArea::Bottom)] = {r.x, r.bottom() - h, r.width, h};
        rest.height = std::max(0.0f, r.height - h - kGap);
    }
    if (!visibleIn(DockArea::Left).empty()) {
        const float w = sizeOf(DockArea::Left, rest.width - kMinimumCentral - kGap);
        m_areaRects[std::size_t(DockArea::Left)] = {rest.x, rest.y, w, rest.height};
        rest.x += w + kGap;
        rest.width = std::max(0.0f, rest.width - w - kGap);
    }
    if (!visibleIn(DockArea::Right).empty()) {
        const float w = sizeOf(DockArea::Right, rest.width - kMinimumCentral - kGap);
        m_areaRects[std::size_t(DockArea::Right)] = {rest.right() - w, rest.y, w, rest.height};
        rest.width = std::max(0.0f, rest.width - w - kGap);
    }
    m_centralRect = rest;
    if (m_central) {
        m_central->arrange(rest);
    }

    // Panels split their area by share: side areas top to bottom, the
    // bottom area left to right.
    for (const DockArea area : {DockArea::Left, DockArea::Right, DockArea::Bottom}) {
        const std::vector<std::size_t> shown = visibleIn(area);
        if (shown.empty()) {
            continue;
        }
        const RectF a = m_areaRects[std::size_t(area)];
        const bool across = area == DockArea::Bottom;
        const float extent = (across ? a.width : a.height) - kGap * float(shown.size() - 1);
        float total = 0.0f;
        for (const std::size_t i : shown) {
            total += m_entries[i].share;
        }
        float at = across ? a.x : a.y;
        for (std::size_t k = 0; k < shown.size(); ++k) {
            const Entry &entry = m_entries[shown[k]];
            const float size = k + 1 == shown.size() ? (across ? a.right() : a.bottom()) - at
                                                     : std::round(extent * entry.share / total);
            entry.panel->arrange(across ? RectF{at, a.y, size, a.height} : RectF{a.x, at, a.width, size});
            at += size + kGap;
        }
    }
}

void DockLayout::paint(Painter &painter, const Theme &theme) {
    painter.fillRect(rect(), theme.window); // the gaps between panels
}

std::optional<DockLayout::Handle> DockLayout::handleAt(Vec2 p) const {
    const RectF left = areaRect(DockArea::Left);
    const RectF right = areaRect(DockArea::Right);
    const RectF bottom = areaRect(DockArea::Bottom);
    if (!left.isEmpty() && inside({left.right(), left.y, kGap, left.height}, p)) {
        return Handle{{left.right(), left.y, kGap, left.height}, DockArea::Left, -1, -1, true};
    }
    if (!right.isEmpty() && inside({right.x - kGap, right.y, kGap, right.height}, p)) {
        return Handle{{right.x - kGap, right.y, kGap, right.height}, DockArea::Right, -1, -1, true};
    }
    if (!bottom.isEmpty() && inside({bottom.x, bottom.y - kGap, bottom.width, kGap}, p)) {
        return Handle{{bottom.x, bottom.y - kGap, bottom.width, kGap}, DockArea::Bottom, -1, -1, false};
    }
    // Between two panels of one area.
    for (const DockArea area : {DockArea::Left, DockArea::Right, DockArea::Bottom}) {
        const std::vector<std::size_t> shown = visibleIn(area);
        for (std::size_t k = 0; k + 1 < shown.size(); ++k) {
            const RectF a = m_entries[shown[k]].panel->rect();
            const bool across = area == DockArea::Bottom;
            const RectF gap = across ? RectF{a.right(), a.y, kGap, a.height} : RectF{a.x, a.bottom(), a.width, kGap};
            if (inside(gap, p)) {
                return Handle{gap, std::nullopt, std::ptrdiff_t(shown[k]), std::ptrdiff_t(shown[k + 1]), across};
            }
        }
    }
    return std::nullopt;
}

std::optional<Cursor> DockLayout::cursorAt(Vec2 position) const {
    if (m_resizing) {
        return m_resizing->vertical ? Cursor::SizeHorizontal : Cursor::SizeVertical;
    }
    if (const std::optional<Handle> handle = handleAt(position)) {
        return handle->vertical ? Cursor::SizeHorizontal : Cursor::SizeVertical;
    }
    return Element::cursorAt(position);
}

bool DockLayout::onPointer(const PointerEvent &event) {
    switch (event.type) {
    case PointerEvent::Type::Press: {
        if (event.button != PointerButton::Left) {
            return false;
        }
        m_resizing = handleAt(event.position);
        if (!m_resizing) {
            return false;
        }
        m_resizeStart = event.position;
        if (m_resizing->area) {
            const RectF a = areaRect(*m_resizing->area);
            m_resizeFrom = *m_resizing->area == DockArea::Bottom ? a.height : a.width;
        } else {
            // Shares become pixel extents, so the two panels trade pixels.
            const bool across = m_resizing->vertical;
            for (Entry &entry : m_entries) {
                if (entry.panel->isVisible()) {
                    entry.share = std::max(1.0f, across ? entry.panel->rect().width : entry.panel->rect().height);
                }
            }
            m_resizeFrom = m_entries[std::size_t(m_resizing->first)].share;
            m_resizeFromSecond = m_entries[std::size_t(m_resizing->second)].share;
        }
        return true;
    }
    case PointerEvent::Type::Move: {
        if (!m_resizing || !isPressed()) {
            return false;
        }
        const Vec2 d = event.position - m_resizeStart;
        if (m_resizing->area) {
            const DockArea area = *m_resizing->area;
            const float delta = area == DockArea::Left ? d.x : area == DockArea::Right ? -d.x : -d.y;
            setAreaSize(area, m_resizeFrom + delta);
        } else {
            Entry &first = m_entries[std::size_t(m_resizing->first)];
            Entry &second = m_entries[std::size_t(m_resizing->second)];
            const float total = m_resizeFrom + m_resizeFromSecond;
            const float delta = m_resizing->vertical ? d.x : d.y;
            const float a = std::clamp(m_resizeFrom + delta, std::min(first.panel->minimumExtent(), total / 2.0f),
                                       total - std::min(second.panel->minimumExtent(), total / 2.0f));
            first.share = a;
            second.share = total - a;
            invalidateLayout();
            invalidatePaint();
        }
        return true;
    }
    case PointerEvent::Type::Release:
        if (m_resizing) {
            m_resizing.reset();
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ---- Moving panels ----

std::optional<DockArea> DockLayout::dropAreaAt(Vec2 p) const {
    const RectF r = rect();
    if (!inside(r, p)) {
        return std::nullopt;
    }
    if (p.x < r.x + r.width * 0.2f) {
        return DockArea::Left;
    }
    if (p.x > r.right() - r.width * 0.2f) {
        return DockArea::Right;
    }
    if (p.y > r.bottom() - r.height * 0.3f) {
        return DockArea::Bottom;
    }
    return std::nullopt;
}

void DockLayout::beginPanelDrag(DockPanel &panel) {
    m_dragging = &panel;
    m_dropArea.reset();
}

void DockLayout::updatePanelDrag(Vec2 position) {
    const std::optional<DockArea> area = dropAreaAt(position);
    if (area == m_dropArea) {
        return;
    }
    m_dropArea = area;
    Surface *s = surface();
    if (!s) {
        return;
    }
    // One indicator popup, replaced when the target changes.
    if (m_indicator) {
        Element *old = m_indicator;
        m_indicator = nullptr;
        s->closePopup(*old);
    }
    if (!area) {
        return;
    }
    // Where the panel would go: its area as it is, or a strip at that edge.
    RectF target = areaRect(*area);
    const RectF r = rect();
    if (target.isEmpty()) {
        target = *area == DockArea::Left    ? RectF{r.x, r.y, m_sizes[0], r.height}
                 : *area == DockArea::Right ? RectF{r.right() - m_sizes[1], r.y, m_sizes[1], r.height}
                                            : RectF{r.x, r.bottom() - m_sizes[2], r.width, m_sizes[2]};
    }
    auto indicator = std::make_unique<DropIndicator>();
    indicator->setFixedSize({target.width, target.height});
    Surface::PopupOptions options;
    options.takesInput = false;
    m_indicator = &s->openPopup(std::move(indicator), target.origin(), options);
}

void DockLayout::endPanelDrag(Vec2 position) {
    DockPanel *panel = m_dragging;
    m_dragging = nullptr;
    if (m_indicator) {
        Element *old = m_indicator;
        m_indicator = nullptr;
        if (Surface *s = surface()) {
            s->closePopup(*old);
        }
    }
    m_dropArea.reset();
    const std::optional<DockArea> area = dropAreaAt(position);
    if (panel && area && areaOf(*panel) != area) {
        movePanel(*panel, *area);
    }
}

// ---- State ----

String DockLayout::saveState() const {
    String out = "L=" + std::to_string(int(std::lround(m_sizes[0]))) + ";R=" +
                 std::to_string(int(std::lround(m_sizes[1]))) + ";B=" + std::to_string(int(std::lround(m_sizes[2])));
    for (const Entry &entry : m_entries) {
        // Shares as pixel-ish weights, rounded: stable text for settings files.
        out += ";" + entry.panel->id() + ":" + areaLetter(entry.area) + ":" +
               std::to_string(int(std::lround(entry.share * 100.0f))) + ":" + (entry.panel->isVisible() ? "1" : "0");
    }
    return out;
}

bool DockLayout::restoreState(StringView state) {
    struct Saved {
        DockPanel *panel;
        DockArea area;
        float share;
        bool visible;
    };
    std::vector<Saved> saved;
    float sizes[3] = {m_sizes[0], m_sizes[1], m_sizes[2]};
    for (const StringView item : split(state, ';')) {
        if (item.size() > 2 && item[1] == '=') {
            const std::optional<DockArea> area = areaFromLetter(item.substr(0, 1));
            const std::optional<float> size = number(item.substr(2));
            if (!area || !size) {
                return false;
            }
            sizes[std::size_t(*area)] = *size;
            continue;
        }
        const std::vector<StringView> fields = split(item, ':');
        if (fields.size() != 4) {
            return false;
        }
        const std::optional<DockArea> area = areaFromLetter(fields[1]);
        const std::optional<float> share = number(fields[2]);
        if (!area || !share || (fields[3] != "0" && fields[3] != "1")) {
            return false;
        }
        if (DockPanel *p = panel(fields[0])) {
            saved.push_back({p, *area, std::max(0.01f, *share / 100.0f), fields[3] == "1"});
        }
    }
    for (std::size_t i = 0; i < 3; ++i) {
        m_sizes[i] = std::max(40.0f, sizes[i]);
    }
    // Saved panels in saved order, then the ones the state did not mention.
    std::vector<Entry> entries;
    for (const Saved &s : saved) {
        entries.push_back({s.panel, s.area, s.share});
    }
    for (const Entry &entry : m_entries) {
        if (std::none_of(saved.begin(), saved.end(), [&entry](const Saved &s) { return s.panel == entry.panel; })) {
            entries.push_back(entry);
        }
    }
    m_entries = std::move(entries);
    for (const Saved &s : saved) {
        setPanelVisible(*s.panel, s.visible);
    }
    invalidateLayout();
    invalidatePaint();
    return true;
}

} // namespace cfw
