#include "cfw/ui/Timeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "cfw/gfx/Painter.h"
#include "cfw/ui/Accessibility.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Theme.h"

namespace cfw {

namespace {

constexpr float kFlagSize = 22.0f;
constexpr float kBadgeSize = 22.0f;
constexpr float kBar = 6.0f; // scrollbar thickness
constexpr double kTailSeconds = 30.0; // room after the last clip

Color mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// "0:41", "2:56", "1:02:03": the ruler's and the corner's time.
String clockText(double seconds) {
    const long total = long(std::floor(std::max(0.0, seconds) + 1e-6));
    char buffer[32];
    if (total >= 3600) {
        std::snprintf(buffer, sizeof buffer, "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
    } else {
        std::snprintf(buffer, sizeof buffer, "%ld:%02ld", total / 60, total % 60);
    }
    return buffer;
}

void roundedFill(Painter &painter, const RectF &r, float radius, const Brush &brush) {
    PainterPath p;
    p.addRoundedRect(r, radius, radius);
    painter.fillPath(p, brush);
}

void roundedStroke(Painter &painter, const RectF &r, float radius, Color color, float width = 1.0f) {
    PainterPath p;
    const float h = width / 2.0f;
    p.addRoundedRect({r.x + h, r.y + h, r.width - width, r.height - width}, radius, radius);
    painter.strokePath(p, Pen(color, width));
}

} // namespace

Timeline::Timeline() {
    setRole(Role::Group);
    setAccessibleName("Timeline");
    setClipsChildren(true);
    m_nameEditor = &add<TextField>();
    m_nameEditor->setVisible(false);
    m_nameEditor->setAccessibleName("Track name");
    static_cast<void>(m_nameEditor->submitted.connect([this](const String &) { finishRename(true); }));
    static_cast<void>(m_nameEditor->focusLost.connect([this] { finishRename(true); }));
}

// ---- Renaming tracks ----------------------------------------------------------------

RectF Timeline::trackNameRect(std::size_t track) const {
    if (track >= m_tracks.size()) {
        return {};
    }
    const float h = std::min(24.0f, m_tracks[track].height - 4.0f);
    const float top = trackTop(track) + (m_tracks[track].height - 1.0f - h) / 2.0f;
    return {rect().x + 33.0f, top, std::max(40.0f, m_headerWidth - 33.0f - 60.0f), h};
}

void Timeline::renameTrack(std::size_t track) {
    if (track >= m_tracks.size()) {
        return;
    }
    m_renaming = track;
    m_nameEditor->setText(m_tracks[track].name);
    m_nameEditor->setVisible(true);
    m_nameEditor->arrange(trackNameRect(track));
    m_nameEditor->selectAll();
    if (Surface *s = surface()) {
        s->setFocus(m_nameEditor);
    }
    invalidatePaint();
}

void Timeline::finishRename(bool keep) {
    if (!m_renaming) {
        return;
    }
    const std::size_t track = *m_renaming;
    m_renaming.reset(); // first: losing focus below calls here again
    String name = m_nameEditor->text();
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    while (!name.empty() && name.front() == ' ') {
        name.erase(0, 1);
    }
    m_nameEditor->setVisible(false);
    if (Surface *s = surface(); s && s->focus() == m_nameEditor) {
        s->setFocus(nullptr);
    }
    invalidatePaint();
    if (keep && !name.empty() && track < m_tracks.size() && name != m_tracks[track].name) {
        m_tracks[track].name = name;
        trackRenamed.emit(track, name);
    }
}

bool Timeline::onKey(const KeyEvent &event) {
    // Keys the name field leaves (it bubbles them here).
    if (m_renaming && event.type == KeyEvent::Type::Press && event.key == Key::Escape) {
        finishRename(false);
        return true;
    }
    return false;
}

// ---- Content ------------------------------------------------------------------------

void Timeline::setTracks(std::vector<Track> tracks) {
    m_tracks = std::move(tracks);
    if (m_renaming && *m_renaming >= m_tracks.size()) {
        finishRename(false);
    }
    clampScroll();
    invalidatePaint();
}

void Timeline::setClips(std::vector<Clip> clips) {
    m_clips = std::move(clips);
    m_drag = Drag::None;
    std::erase_if(m_selection, [&](std::uint64_t id) { return !clip(id); });
    if (m_selected && !clip(*m_selected)) {
        m_selected.reset();
    }
    invalidatePaint();
}

const Timeline::Clip *Timeline::clip(std::uint64_t id) const {
    for (const Clip &c : m_clips) {
        if (c.id == id) {
            return &c;
        }
    }
    return nullptr;
}

void Timeline::setMarkers(std::vector<Marker> markers) {
    m_markers = std::move(markers);
    invalidatePaint();
}

void Timeline::setBadges(std::vector<Badge> badges) {
    m_badges = std::move(badges);
    invalidatePaint();
}

void Timeline::setIcons(Icons icons) {
    m_icons = std::move(icons);
    invalidatePaint();
}

// ---- View ---------------------------------------------------------------------------

void Timeline::setPlayhead(double seconds) {
    seconds = std::max(0.0, seconds);
    if (seconds == m_playhead) {
        return;
    }
    // Only the line's old and new places (with the marker on top) and the
    // time in the corner change.
    const RectF r = rect();
    auto strip = [&](double t) {
        const float x = std::round(xAt(t));
        invalidatePaint({x - 8.0f, r.y, 17.0f, r.height});
    };
    strip(m_playhead);
    m_playhead = seconds;
    strip(m_playhead);
    invalidatePaint({r.x, r.y, m_headerWidth, kRulerHeight});
}

void Timeline::setZoom(double pixelsPerSecond) {
    const double z = std::clamp(pixelsPerSecond, kMinimumZoom, kMaximumZoom);
    if (z != m_zoom) {
        m_zoom = z;
        clampScroll();
        invalidatePaint();
    }
}

double Timeline::fitZoom() const {
    const double width = std::max(1.0f, rect().width - m_headerWidth - 30.0f);
    return std::clamp(std::floor(width / (duration() + 10.0) * 2.0) / 2.0, kMinimumZoom, kMaximumZoom);
}

void Timeline::setTool(Tool tool) {
    m_tool = tool;
    invalidatePaint();
}

void Timeline::setSelectedClip(std::optional<std::uint64_t> id) {
    std::vector<std::uint64_t> only;
    if (id) {
        only.push_back(*id);
    }
    m_selection = only;
    if (id != m_selected) {
        m_selected = id;
        invalidatePaint();
    }
}

void Timeline::setSelectedClips(std::vector<std::uint64_t> ids) {
    m_selection = std::move(ids);
    if (m_selected && !isSelected(*m_selected)) {
        m_selected.reset();
    }
    if (!m_selected && !m_selection.empty()) {
        m_selected = m_selection.back();
    }
    invalidatePaint();
}

bool Timeline::isSelected(std::uint64_t id) const {
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void Timeline::selectionDidChange() {
    invalidatePaint();
    selectionChanged.emit(m_selection);
}

void Timeline::boxSelect() {
    const RectF box{std::min(m_boxFrom.x, m_boxTo.x), std::min(m_boxFrom.y, m_boxTo.y), std::abs(m_boxTo.x - m_boxFrom.x),
                    std::abs(m_boxTo.y - m_boxFrom.y)};
    std::vector<std::uint64_t> next = m_boxBase;
    for (const Clip &c : m_clips) {
        if (c.track < m_tracks.size() && !m_tracks[c.track].locked && clipRect(c).intersects(box) &&
            std::find(next.begin(), next.end(), c.id) == next.end()) {
            next.push_back(c.id);
        }
    }
    if (next != m_selection) {
        m_selection = std::move(next);
        m_selected = m_selection.empty() ? std::nullopt : std::optional<std::uint64_t>(m_selection.back());
        selectionDidChange();
    }
    invalidatePaint();
}

void Timeline::setSelectedBadge(std::optional<std::uint64_t> id) {
    if (id != m_selectedBadge) {
        m_selectedBadge = id;
        invalidatePaint();
    }
}

void Timeline::setHeaderWidth(float width) {
    m_headerWidth = std::max(0.0f, width);
    invalidatePaint();
}

double Timeline::duration() const {
    double end = 10.0;
    for (const Clip &c : m_clips) {
        end = std::max(end, c.start + c.duration);
    }
    return end;
}

void Timeline::setScrollOffset(Vec2 offset) {
    m_scroll = offset;
    clampScroll();
    invalidatePaint();
}

void Timeline::ensurePlayheadVisible() {
    const RectF lanes = lanesRect();
    const float x = float(m_playhead * m_zoom);
    if (x > m_scroll.x + lanes.width - 40.0f || x < m_scroll.x) {
        setScrollOffset({x - 60.0f, m_scroll.y});
    }
}

double Timeline::timeAt(float x) const { return (x - (rect().x + m_headerWidth) + m_scroll.x) / m_zoom; }

float Timeline::xAt(double time) const { return rect().x + m_headerWidth - m_scroll.x + float(time * m_zoom); }

float Timeline::trackTop(std::size_t track) const {
    float y = rect().y + kRulerHeight - m_scroll.y;
    for (std::size_t i = 0; i < track && i < m_tracks.size(); ++i) {
        y += m_tracks[i].height;
    }
    return y;
}

float Timeline::tracksHeight() const {
    float h = 0.0f;
    for (const Track &t : m_tracks) {
        h += t.height;
    }
    return h;
}

RectF Timeline::lanesRect() const {
    const RectF r = rect();
    return {r.x + m_headerWidth, r.y + kRulerHeight, std::max(0.0f, r.width - m_headerWidth),
            std::max(0.0f, r.height - kRulerHeight)};
}

Vec2 Timeline::maxScroll() const {
    const RectF lanes = lanesRect();
    const float contentW = float((duration() + kTailSeconds) * m_zoom);
    return {std::max(0.0f, contentW - lanes.width), std::max(0.0f, tracksHeight() - lanes.height)};
}

void Timeline::clampScroll() {
    const Vec2 m = maxScroll();
    m_scroll.x = std::clamp(m_scroll.x, 0.0f, m.x);
    m_scroll.y = std::clamp(m_scroll.y, 0.0f, m.y);
}

RectF Timeline::clipRect(const Clip &c) const {
    if (c.track >= m_tracks.size()) {
        return {};
    }
    const float top = trackTop(c.track);
    const float x = xAt(c.start);
    return {x, top + 3.0f, std::max(4.0f, float(c.duration * m_zoom)), m_tracks[c.track].height - 7.0f};
}

RectF Timeline::flagRect(std::size_t track, TrackFlag flag) const {
    const RectF r = rect();
    const float top = trackTop(track);
    const float cy = top + (m_tracks[track].height - 1.0f) / 2.0f;
    const float right = r.x + m_headerWidth - 1.0f - 6.0f;
    const float x = flag == TrackFlag::Locked ? right - kFlagSize : right - kFlagSize * 2.0f - 6.0f;
    return {x, cy - kFlagSize / 2.0f, kFlagSize, kFlagSize};
}

Vec2 Timeline::measureContent(Vec2 available) {
    return {available.x, std::min(available.y, kRulerHeight + tracksHeight())};
}

void Timeline::arrangeContent(const RectF &) {
    clampScroll();
    if (m_renaming) {
        m_nameEditor->arrange(trackNameRect(*m_renaming));
    }
}

// ---- Hit testing --------------------------------------------------------------------

Timeline::Hit Timeline::hitTest(Vec2 p) const {
    const RectF r = rect();
    Hit hit;
    if (!r.contains(p)) {
        return hit;
    }
    if (p.y < r.y + kRulerHeight) {
        hit.part = p.x >= r.x + m_headerWidth ? Part::Ruler : Part::None;
        return hit;
    }
    // Which track.
    std::optional<std::size_t> track;
    for (std::size_t i = 0; i < m_tracks.size(); ++i) {
        const float top = trackTop(i);
        if (p.y >= top && p.y < top + m_tracks[i].height) {
            track = i;
            break;
        }
    }
    if (!track) {
        return hit;
    }
    hit.track = *track;
    if (p.x < r.x + m_headerWidth) {
        hit.part = Part::Header;
        const bool hasIcons = !m_icons.locked.isNull();
        if (hasIcons) {
            for (TrackFlag f : {TrackFlag::Locked, m_tracks[*track].audio ? TrackFlag::Muted : TrackFlag::Hidden}) {
                if (flagRect(*track, f).contains(p)) {
                    hit.part = Part::FlagButton;
                    hit.flag = f;
                }
            }
        }
        return hit;
    }
    // Badges sit above clips.
    for (std::size_t i = 0; i < m_badges.size(); ++i) {
        const Badge &b = m_badges[i];
        if (b.track != *track) {
            continue;
        }
        const float cx = xAt(b.time);
        const float cy = trackTop(b.track) + (m_tracks[b.track].height - 1.0f) / 2.0f;
        if (std::abs(p.x - cx) <= kBadgeSize / 2.0f && std::abs(p.y - cy) <= kBadgeSize / 2.0f) {
            hit.part = Part::Badge;
            hit.badge = i;
            return hit;
        }
    }
    // The selected clip is drawn on top, so it wins; then the last drawn.
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < m_clips.size(); ++i) {
        const Clip &c = m_clips[i];
        if (c.track != *track || !clipRect(c).contains(p)) {
            continue;
        }
        if (!found || (m_selected && c.id == *m_selected)) {
            found = i;
        } else if (!(m_selected && m_clips[*found].id == *m_selected)) {
            found = i;
        }
    }
    if (found) {
        const RectF cr = clipRect(m_clips[*found]);
        hit.clip = *found;
        hit.part = p.x < cr.x + kHandle ? Part::LeftHandle : p.x >= cr.right() - kHandle ? Part::RightHandle : Part::Clip;
        if (cr.width < kHandle * 3.0f) {
            hit.part = Part::Clip; // too small to trim by its ends: moving it is what one means
        }
        return hit;
    }
    hit.part = Part::Lane;
    return hit;
}

std::optional<Cursor> Timeline::cursorAt(Vec2 position) const {
    if (m_drag == Drag::TrimLeft || m_drag == Drag::TrimRight) {
        return Cursor::SizeHorizontal;
    }
    const Hit hit = hitTest(position);
    switch (hit.part) {
    case Part::Ruler:
    case Part::Badge:
    case Part::FlagButton: return Cursor::Hand;
    case Part::LeftHandle:
    case Part::RightHandle:
        if (m_tool == Tool::Select) {
            return Cursor::SizeHorizontal;
        }
        return Cursor::Crosshair;
    case Part::Clip:
    case Part::Lane:
        if (m_tool == Tool::Razor) {
            return Cursor::Crosshair;
        }
        return Cursor::Arrow;
    default: return Cursor::Arrow;
    }
}

// ---- Input --------------------------------------------------------------------------

double Timeline::snap(double time, double pps) const {
    if (!m_snapping) {
        return time;
    }
    double best = time;
    double bestDistance = kSnapPixels / pps;
    auto consider = [&](double q) {
        const double d = std::abs(q - time);
        if (d < bestDistance) {
            bestDistance = d;
            best = q;
        }
    };
    consider(0.0);
    consider(m_playhead);
    for (std::size_t i = 0; i < m_clips.size(); ++i) {
        const bool dragged = std::any_of(m_original.begin(), m_original.end(),
                                         [i](const Original &o) { return o.index == i; });
        if (!dragged) {
            consider(m_clips[i].start);
            consider(m_clips[i].start + m_clips[i].duration);
        }
    }
    return best;
}

void Timeline::drag(Vec2 position) {
    if (m_drag == Drag::Seek) {
        const double t = std::clamp(timeAt(position.x), 0.0, duration());
        setPlayhead(t);
        seeked.emit(t);
        return;
    }
    if (m_drag == Drag::Box) {
        m_boxTo = position;
        boxSelect();
        return;
    }
    if (m_original.empty()) {
        return;
    }
    if (!m_moved) {
        if (std::abs(position.x - m_pressX) < 3.0f) {
            return;
        }
        editStarted.emit();
        m_moved = true;
    }
    const double pps = m_zoom;
    const double dx = (position.x - m_pressX) / pps;
    const Original &o = m_original.front();
    Clip &c = m_clips[o.index];
    if (m_drag == Drag::Move) {
        double ns = std::max(0.0, o.start + dx);
        const double a = snap(ns, pps);
        const double b = snap(ns + c.duration, pps) - c.duration;
        // Snap whichever edge is nearer a snap point (an edge that found none
        // does not count).
        const bool snapA = std::abs(a - ns) > 1e-9, snapB = std::abs(b - ns) > 1e-9;
        if (snapA && (!snapB || std::abs(a - ns) <= std::abs(b - ns))) {
            ns = a;
        } else if (snapB) {
            ns = b;
        }
        // Everything dragged keeps its distance; the earliest stops at 0.
        double earliest = o.start;
        for (const Original &x : m_original) {
            earliest = std::min(earliest, x.start);
        }
        c.start = std::max(o.start - earliest, std::round(ns * 100.0) / 100.0);
        for (std::size_t i = 1; i < m_original.size(); ++i) {
            m_clips[m_original[i].index].start = std::max(0.0, m_original[i].start + (c.start - o.start));
        }
    } else if (m_drag == Drag::TrimLeft) {
        const double ns = std::clamp(snap(o.start + dx, pps), 0.0, o.start + o.duration - 0.2);
        c.start = ns;
        c.duration = o.duration - (ns - o.start);
        for (std::size_t i = 1; i < m_original.size(); ++i) {
            m_clips[m_original[i].index].start = ns;
            m_clips[m_original[i].index].duration = c.duration;
        }
    } else if (m_drag == Drag::TrimRight) {
        const double ne = std::max(c.start + 0.2, snap(o.start + o.duration + dx, pps));
        c.duration = ne - c.start;
        for (std::size_t i = 1; i < m_original.size(); ++i) {
            m_clips[m_original[i].index].duration = c.duration;
        }
    }
    invalidatePaint();
    clipChanged.emit(c.id);
}

bool Timeline::onPointer(const PointerEvent &event) {
    using Type = PointerEvent::Type;
    switch (event.type) {
    case Type::Wheel: {
        finishRename(true); // the name would scroll away from its field
        if (hasModifier(event.modifiers, Modifier::Control)) {
            // Zoom around the pointer: the time under it stays put.
            const double t = timeAt(event.position.x);
            const double factor = event.wheelDelta.y > 0 ? 1.25 : 0.8;
            const double z = std::clamp(std::round(m_zoom * factor * 2.0) / 2.0, kMinimumZoom, kMaximumZoom);
            if (z != m_zoom) {
                m_zoom = z;
                m_scroll.x = float(t * m_zoom) - (event.position.x - (rect().x + m_headerWidth));
                clampScroll();
                invalidatePaint();
                zoomChanged.emit(m_zoom);
            }
            return true;
        }
        Vec2 d = event.wheelDelta;
        if (hasModifier(event.modifiers, Modifier::Shift) && d.x == 0.0f) {
            d = {d.y, 0.0f};
        }
        const Vec2 before = m_scroll;
        m_scroll.x -= d.x;
        m_scroll.y -= d.y;
        clampScroll();
        if (m_scroll != before) {
            invalidatePaint();
            return true;
        }
        return false;
    }
    case Type::Press: {
        finishRename(true); // a press anywhere else on the timeline keeps the name
        const Hit hit = hitTest(event.position);
        if (event.button == PointerButton::Right) {
            if (hit.part == Part::Clip || hit.part == Part::LeftHandle || hit.part == Part::RightHandle) {
                const Clip &c = m_clips[hit.clip];
                m_selected = c.id;
                m_selectedBadge.reset();
                invalidatePaint();
                clipSelected.emit(c.id);
                clipContextMenuRequested.emit(c.id, event.position);
                return true;
            }
            return false;
        }
        if (event.button != PointerButton::Left) {
            return false;
        }
        switch (hit.part) {
        case Part::Ruler:
            m_drag = Drag::Seek;
            drag(event.position);
            return true;
        case Part::FlagButton: {
            Track &t = m_tracks[hit.track];
            bool &flag = hit.flag == TrackFlag::Locked ? t.locked : hit.flag == TrackFlag::Muted ? t.muted : t.hidden;
            flag = !flag;
            invalidatePaint();
            trackFlagToggled.emit(hit.track, hit.flag);
            return true;
        }
        case Part::Badge:
            m_selectedBadge = m_badges[hit.badge].id;
            m_selected.reset();
            invalidatePaint();
            badgeSelected.emit(m_badges[hit.badge].id);
            return true;
        case Part::Clip:
        case Part::LeftHandle:
        case Part::RightHandle: {
            const Clip &c = m_clips[hit.clip];
            if (m_tracks[c.track].locked) {
                lockedClipPressed.emit(c.id);
                return true;
            }
            if (m_tool == Tool::Razor) {
                razor.emit(c.id, timeAt(event.position.x));
                return true;
            }
            // Shift or Ctrl: add it to the selection, or take it out.
            if (hasModifier(event.modifiers, Modifier::Shift) || hasModifier(event.modifiers, Modifier::Control)) {
                if (isSelected(c.id)) {
                    std::erase(m_selection, c.id);
                    m_selected = m_selection.empty() ? std::nullopt : std::optional<std::uint64_t>(m_selection.back());
                } else {
                    m_selection.push_back(c.id);
                    m_selected = c.id;
                }
                m_selectedBadge.reset();
                clipSelected.emit(m_selected);
                selectionDidChange();
                return true;
            }
            const bool many = isSelected(c.id) && m_selection.size() > 1 && hit.part == Part::Clip;
            m_pressedInMany = many; // a click without a drag then selects only this one
            if (!many) {
                m_selection = {c.id};
            }
            m_selected = c.id;
            m_selectedBadge.reset();
            invalidatePaint();
            clipSelected.emit(c.id);
            selectionChanged.emit(m_selection);
            m_drag = hit.part == Part::LeftHandle    ? Drag::TrimLeft
                     : hit.part == Part::RightHandle ? Drag::TrimRight
                                                     : Drag::Move;
            m_pressX = event.position.x;
            m_moved = false;
            m_original.clear();
            m_original.push_back({hit.clip, c.start, c.duration});
            // What moves along: its group, and (moving a selected clip) the
            // other selected clips and their groups.
            for (std::size_t i = 0; i < m_clips.size(); ++i) {
                const Clip &x = m_clips[i];
                const bool sameGroup = c.group != 0 && x.group == c.group;
                bool selectedToo = false;
                if (many) {
                    for (std::uint64_t id : m_selection) {
                        const Clip *s = clip(id);
                        selectedToo = selectedToo || x.id == id || (s && s->group != 0 && x.group == s->group);
                    }
                }
                if (i != hit.clip && (sameGroup || selectedToo) && !m_tracks[x.track].locked) {
                    m_original.push_back({i, x.start, x.duration});
                }
            }
            return true;
        }
        case Part::Header:
            if (m_namesEditable && event.clickCount == 2 && trackNameRect(hit.track).contains(event.position)) {
                renameTrack(hit.track);
                return true;
            }
            return false;
        case Part::Lane: {
            // A box drag from here selects what it touches (Shift: adds).
            const bool adding = hasModifier(event.modifiers, Modifier::Shift) || hasModifier(event.modifiers, Modifier::Control);
            m_boxBase = adding ? m_selection : std::vector<std::uint64_t>{};
            m_boxFrom = m_boxTo = event.position;
            m_drag = Drag::Box;
            if (!adding && (m_selected || m_selectedBadge || !m_selection.empty())) {
                m_selected.reset();
                m_selection.clear();
                m_selectedBadge.reset();
                invalidatePaint();
                clipSelected.emit(std::nullopt);
                selectionChanged.emit(m_selection);
            }
            return true;
        }
        default: return false;
        }
    }
    case Type::Move: {
        if (m_drag != Drag::None) {
            drag(event.position);
            return true;
        }
        // The tooltip follows what is under the pointer.
        const Hit hit = hitTest(event.position);
        String tip;
        if (hit.part == Part::Clip || hit.part == Part::LeftHandle || hit.part == Part::RightHandle) {
            tip = m_clips[hit.clip].label;
        } else if (hit.part == Part::Badge) {
            tip = m_badges[hit.badge].toolTip;
        } else if (hit.part == Part::Ruler) {
            for (const Marker &m : m_markers) {
                if (std::abs(xAt(m.time) - event.position.x) <= 6.0f && event.position.y < rect().y + 14.0f) {
                    tip = m.label;
                }
            }
        }
        setToolTip(std::move(tip));
        return false;
    }
    case Type::Release:
    case Type::Cancel: {
        const Drag was = m_drag;
        m_drag = Drag::None;
        if (was == Drag::Box) {
            invalidatePaint(); // the box goes
            // A click (not a drag) on an empty spot moves the playhead there.
            if (event.type == Type::Release && std::abs(m_boxTo.x - m_boxFrom.x) < 3.0f &&
                std::abs(m_boxTo.y - m_boxFrom.y) < 3.0f) {
                const double t = std::clamp(timeAt(m_boxFrom.x), 0.0, duration());
                setPlayhead(t);
                seeked.emit(t);
            }
            return true;
        }
        if (was == Drag::Move && !m_moved && m_pressedInMany && !m_original.empty() && event.type == Type::Release) {
            // A click on one of several selected clips (not a drag of them
            // all): just that one is selected now.
            m_selection = {m_clips[m_original.front().index].id};
            m_selected = m_selection.front();
            selectionDidChange();
        }
        m_pressedInMany = false;
        if ((was == Drag::Move || was == Drag::TrimLeft || was == Drag::TrimRight) && m_moved && !m_original.empty()) {
            const std::uint64_t id = m_clips[m_original.front().index].id;
            m_original.clear();
            m_moved = false;
            editFinished.emit(id);
        }
        m_original.clear();
        return was != Drag::None;
    }
    default: return false;
    }
}

void Timeline::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    node.value = clockText(m_playhead) + " of " + clockText(duration()) + ", " + std::to_string(m_clips.size()) +
                 " clips";
}

// ---- Painting -----------------------------------------------------------------------

const TextLayout &Timeline::textLayout(StringView text, const std::shared_ptr<const FontFace> &font, float size,
                                       const Theme &theme) {
    std::string key(text.data(), text.size());
    key += '\x1f';
    key += std::to_string(reinterpret_cast<std::uintptr_t>(font.get()));
    key += '/';
    key += std::to_string(int(size * 10.0f));
    auto it = m_text.find(key);
    if (it == m_text.end()) {
        if (m_text.size() > 512) {
            m_text.clear();
        }
        TextLayout layout;
        TextStyle style;
        style.font = font ? font : theme.font;
        style.pixelSize = size;
        style.fallback = theme.fonts.get();
        TextLayoutOptions options;
        options.wrap = false;
        layout.setText(text);
        layout.layout(style, options);
        it = m_text.emplace(std::move(key), std::move(layout)).first;
    }
    return it->second;
}

void Timeline::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    if (r.width <= 0.0f || r.height <= 0.0f) {
        return;
    }
    painter.save();
    painter.clipRect(r);
    painter.fillRect(r, theme.panel);
    const RectF lanes = lanesRect();
    const Color line = mix(theme.panel, theme.text, 0.08f);
    const std::shared_ptr<const FontFace> strong = theme.strongFont ? theme.strongFont : theme.font;

    // ---- Lanes ----
    painter.save();
    painter.clipRect(lanes);
    const double t0 = std::max(0.0, timeAt(lanes.x));
    const double t1 = timeAt(lanes.right());
    for (std::size_t i = 0; i < m_tracks.size(); ++i) {
        const float top = trackTop(i);
        const float h = m_tracks[i].height;
        if (top > lanes.bottom() || top + h < lanes.y) {
            continue;
        }
        // A faint line every five seconds.
        for (double t = std::floor(t0 / 5.0) * 5.0 + 5.0; t <= t1; t += 5.0) {
            painter.fillRect({std::round(xAt(t)) - 1.0f, top, 1.0f, h - 1.0f}, Color{1, 1, 1, 0.025f});
        }
        painter.fillRect({lanes.x, top + h - 1.0f, lanes.width, 1.0f}, line);
    }
    for (const Clip &c : m_clips) {
        if (isSelected(c.id) || (m_selected && c.id == *m_selected)) {
            continue; // on top
        }
        if (c.track < m_tracks.size()) {
            const Track &t = m_tracks[c.track];
            paintClip(painter, theme, c, t.muted || t.hidden);
        }
    }
    for (std::uint64_t id : m_selection) {
        if (const Clip *c = clip(id); c && c->track < m_tracks.size() && !(m_selected && id == *m_selected)) {
            const Track &t = m_tracks[c->track];
            paintClip(painter, theme, *c, t.muted || t.hidden);
        }
    }
    if (m_selected) {
        if (const Clip *c = clip(*m_selected); c && c->track < m_tracks.size()) {
            const Track &t = m_tracks[c->track];
            paintClip(painter, theme, *c, t.muted || t.hidden);
        }
    }
    if (m_drag == Drag::Box) {
        // The selection box.
        const RectF box{std::min(m_boxFrom.x, m_boxTo.x), std::min(m_boxFrom.y, m_boxTo.y),
                        std::abs(m_boxTo.x - m_boxFrom.x), std::abs(m_boxTo.y - m_boxFrom.y)};
        painter.fillRect(box, Color{1, 1, 1, 0.06f});
        painter.strokeRect(box, Pen(Color{1, 1, 1, 0.45f}, 1.0f));
    }
    for (const Badge &b : m_badges) {
        if (b.track >= m_tracks.size()) {
            continue;
        }
        const float cx = xAt(b.time);
        const float cy = trackTop(b.track) + (m_tracks[b.track].height - 1.0f) / 2.0f;
        const RectF br{std::round(cx - kBadgeSize / 2.0f), std::round(cy - kBadgeSize / 2.0f), kBadgeSize, kBadgeSize};
        const bool on = m_selectedBadge && *m_selectedBadge == b.id;
        roundedFill(painter, br, 5.0f, on ? theme.accent : theme.controlRaised);
        roundedStroke(painter, br, 5.0f, on ? theme.error : Color::fromRgba8(0x5a, 0x5a, 0x66));
        if (!b.icon.isNull()) {
            b.icon.paint(painter, {br.x + 4.0f, br.y + 4.0f, 14.0f, 14.0f}, theme.text);
        }
    }
    painter.restore();

    // ---- Track headers ----
    painter.save();
    const RectF headers{r.x, lanes.y, m_headerWidth, lanes.height};
    painter.clipRect(headers);
    const Color headerBg = mix(theme.window, theme.text, 0.035f);
    for (std::size_t i = 0; i < m_tracks.size(); ++i) {
        const Track &t = m_tracks[i];
        const float top = trackTop(i);
        if (top > headers.bottom() || top + t.height < headers.y) {
            continue;
        }
        painter.fillRect({r.x, top, m_headerWidth, t.height - 1.0f}, headerBg);
        painter.fillRect({r.x, top + t.height - 1.0f, m_headerWidth, 1.0f}, line);
        const float cy = top + (t.height - 1.0f) / 2.0f;
        const TextLayout &code = textLayout(t.shortName, theme.monoFont, 10.0f, theme);
        if (!code.lines().empty()) {
            const auto &ln = code.lines()[0];
            painter.drawText(code, {r.x + 10.0f, std::round(cy - (ln.ascent + ln.descent) / 2.0f - ln.top)},
                             mix(theme.textMuted, theme.window, 0.18f));
        }
        const TextLayout &name = textLayout(t.name, strong, 12.0f, theme);
        if (!name.lines().empty() && m_renaming != i) { // the name field covers it while renaming
            const auto &ln = name.lines()[0];
            painter.save();
            painter.clipRect({r.x + 37.0f, top, m_headerWidth - 37.0f - 64.0f, t.height});
            painter.drawText(name, {r.x + 37.0f, std::round(cy - (ln.ascent + ln.descent) / 2.0f - ln.top)},
                             theme.text);
            painter.restore();
        }
        if (!m_icons.locked.isNull()) {
            const TrackFlag second = t.audio ? TrackFlag::Muted : TrackFlag::Hidden;
            const bool secondOn = t.audio ? t.muted : t.hidden;
            const Icon &secondIcon =
                t.audio ? (t.muted ? m_icons.muted : m_icons.audible) : (t.hidden ? m_icons.hidden : m_icons.visible);
            const RectF a = flagRect(i, second);
            secondIcon.paint(painter, {a.x + 4.0f, a.y + 4.0f, 14.0f, 14.0f}, secondOn ? theme.error : theme.textMuted);
            const RectF l = flagRect(i, TrackFlag::Locked);
            (t.locked ? m_icons.locked : m_icons.unlocked)
                .paint(painter, {l.x + 4.0f, l.y + 4.0f, 14.0f, 14.0f}, t.locked ? theme.error : theme.textMuted);
        }
    }
    painter.fillRect({r.x + m_headerWidth - 1.0f, lanes.y, 1.0f, lanes.height}, line);
    painter.restore();

    // ---- Ruler, playhead, scrollbars ----
    paintRuler(painter, theme, lanes);
    const float px = std::round(xAt(m_playhead));
    if (px >= lanes.x - 0.5f && px <= lanes.right()) {
        painter.fillRect({px, r.y, 1.0f, r.height}, theme.error);
        PainterPath tri;
        tri.moveTo({px - 5.5f, r.y});
        tri.lineTo({px + 6.5f, r.y});
        tri.lineTo({px + 0.5f, r.y + 8.0f});
        tri.close();
        painter.fillPath(tri, theme.error);
    }
    const Vec2 m = maxScroll();
    if (m.x > 0.0f) {
        const float contentW = lanes.width + m.x;
        const float w = std::max(30.0f, lanes.width * lanes.width / contentW);
        const float x = lanes.x + (lanes.width - w) * (m_scroll.x / m.x);
        roundedFill(painter, {x, r.bottom() - kBar - 2.0f, w, kBar}, kBar / 2.0f, Color{1, 1, 1, 0.16f});
    }
    if (m.y > 0.0f) {
        const float contentH = lanes.height + m.y;
        const float h = std::max(24.0f, lanes.height * lanes.height / contentH);
        const float y = lanes.y + (lanes.height - h) * (m_scroll.y / m.y);
        roundedFill(painter, {r.right() - kBar - 2.0f, y, kBar, h}, kBar / 2.0f, Color{1, 1, 1, 0.16f});
    }
    painter.restore();
}

void Timeline::paintRuler(Painter &painter, const Theme &theme, const RectF &lanes) {
    const RectF r = rect();
    const Color line = mix(theme.panel, theme.text, 0.08f);
    const RectF ruler{r.x, r.y, r.width, kRulerHeight};
    painter.fillRect(ruler, theme.panel);
    painter.fillRect({r.x, ruler.bottom() - 1.0f, r.width, 1.0f}, line);
    painter.save();
    painter.clipRect({lanes.x, r.y, lanes.width, kRulerHeight});
    // Major ticks at least 84 px apart, from a list of friendly steps.
    const double pps = m_zoom;
    constexpr std::array<double, 10> kSteps{1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
    double major = 600;
    for (double s : kSteps) {
        if (s * pps >= 84.0) {
            major = s;
            break;
        }
    }
    const double minor = major / (major >= 10 ? 5.0 : major >= 2 ? 2.0 : 4.0);
    const double end = duration() + kTailSeconds;
    const double first = std::max(0.0, std::floor(timeAt(lanes.x) / minor) * minor);
    const double last = std::min(end, timeAt(lanes.right()) + major);
    for (double t = first; t <= last + 1e-9; t += minor) {
        const double q = std::round(t * 1000.0) / 1000.0;
        const bool isMajor = std::abs(q / major - std::round(q / major)) < 1e-6;
        const float x = std::round(xAt(q));
        const float h = isMajor ? 12.0f : 6.0f;
        painter.fillRect({x, ruler.bottom() - 1.0f - h, 1.0f, h},
                         isMajor ? Color::fromRgba8(0x4a, 0x4a, 0x54) : theme.border);
        if (isMajor) {
            const TextLayout &label = textLayout(clockText(q), theme.monoFont, 10.0f, theme);
            if (!label.lines().empty()) {
                const auto &ln = label.lines()[0];
                painter.drawText(label, {x + 4.0f, ruler.bottom() - 10.0f - (ln.ascent + ln.descent) - ln.top},
                                 mix(theme.textMuted, theme.window, 0.18f));
            }
        }
    }
    for (const Marker &mk : m_markers) {
        const float x = xAt(mk.time);
        painter.save();
        painter.translate(x, r.y + 7.0f);
        painter.rotate(45.0f);
        roundedFill(painter, {-5.0f, -5.0f, 10.0f, 10.0f}, 2.0f, mk.color);
        painter.restore();
    }
    painter.restore();
    // The corner: the playhead's time.
    const RectF corner{r.x, r.y, m_headerWidth, kRulerHeight - 1.0f};
    painter.fillRect(corner, theme.panel);
    painter.fillRect({corner.right() - 1.0f, r.y, 1.0f, kRulerHeight}, line);
    const TextLayout &tc = textLayout(clockText(m_playhead), theme.monoFont, 11.0f, theme);
    if (!tc.lines().empty()) {
        const auto &ln = tc.lines()[0];
        painter.drawText(tc, {r.x + 10.0f, std::round(r.y + (kRulerHeight - (ln.ascent + ln.descent)) / 2.0f - ln.top)},
                         mix(theme.textMuted, theme.window, 0.18f));
    }
}

void Timeline::paintClip(Painter &painter, const Theme &theme, const Clip &c, bool dimmed) {
    const RectF cr = clipRect(c);
    const RectF lanes = lanesRect();
    if (cr.right() < lanes.x || cr.x > lanes.right() || cr.bottom() < lanes.y || cr.y > lanes.bottom()) {
        return;
    }
    const bool selected = isSelected(c.id) || (m_selected && *m_selected == c.id);
    const Track &track = m_tracks[c.track];
    painter.save();
    if (dimmed) {
        painter.setOpacity(0.4f);
    }
    if (selected) {
        // A red glow ring around a white outline.
        roundedFill(painter, {cr.x - 4.0f, cr.y - 4.0f, cr.width + 8.0f, cr.height + 8.0f}, 9.0f,
                    Color{227 / 255.0f, 40 / 255.0f, 63 / 255.0f, 0.35f});
        roundedFill(painter, {cr.x - 1.0f, cr.y - 1.0f, cr.width + 2.0f, cr.height + 2.0f}, 6.0f, Color{1, 1, 1, 1});
    }
    PainterPath shape;
    shape.addRoundedRect(cr, 5.0f, 5.0f);
    painter.fillPath(shape, c.color.value_or(track.color));
    painter.save();
    painter.clipPath(shape);
    if (paintClipBody) {
        paintClipBody(painter, c, {cr.x, cr.y + 16.0f, cr.width, std::max(0.0f, cr.height - 16.0f)}, theme);
    }
    if (c.accent) {
        painter.fillRect({cr.x, cr.y, cr.width, 2.0f}, theme.error);
    }
    // The name, with a shadow so it reads over frames.
    float x = cr.x + 7.0f;
    const float cy = cr.y + 3.0f + 7.5f;
    painter.clipRect({cr.x, cr.y, cr.width - 2.0f, cr.height});
    if (!c.icon.isNull()) {
        c.icon.paint(painter, {x, std::round(cy - 6.0f), 12.0f, 12.0f}, theme.text);
        x += 16.0f;
    }
    const std::shared_ptr<const FontFace> strong = theme.strongFont ? theme.strongFont : theme.font;
    const TextLayout &name = textLayout(c.label, strong, 11.0f, theme);
    if (!name.lines().empty()) {
        const auto &ln = name.lines()[0];
        const Vec2 at{x, std::round(cy - (ln.ascent + ln.descent) / 2.0f - ln.top)};
        painter.drawText(name, {at.x, at.y + 1.0f}, Color{0, 0, 0, 0.6f});
        painter.drawText(name, at, theme.text);
    }
    painter.restore();
    if (selected) {
        painter.fillRect({cr.x, cr.y, kHandle, cr.height}, Color{1, 1, 1, 0.22f});
        painter.fillRect({cr.right() - kHandle, cr.y, kHandle, cr.height}, Color{1, 1, 1, 0.22f});
    } else {
        roundedStroke(painter, cr, 5.0f, Color{1, 1, 1, 0.12f});
    }
    painter.restore();
}

} // namespace cfw
