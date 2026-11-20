// Timeline: geometry, seeking on the ruler, selecting, moving with snapping,
// trimming, groups, locked tracks, the razor tool, track flags, wheel zoom
// and painting, headless.

#include <cmath>
#include <cstdio>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Timeline.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

PointerEvent pointer(PointerEvent::Type type, Vec2 at, PointerButton button = PointerButton::Left) {
    PointerEvent e;
    e.type = type;
    e.position = at;
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : button;
    return e;
}

void dragTo(Surface &s, Vec2 from, Vec2 to) {
    s.dispatch(pointer(PointerEvent::Type::Move, from));
    s.dispatch(pointer(PointerEvent::Type::Press, from));
    s.dispatch(pointer(PointerEvent::Type::Move, {(from.x + to.x) / 2, to.y}));
    s.dispatch(pointer(PointerEvent::Type::Move, to));
    s.dispatch(pointer(PointerEvent::Type::Release, to));
}

void click(Surface &s, Vec2 at) {
    s.dispatch(pointer(PointerEvent::Type::Move, at));
    s.dispatch(pointer(PointerEvent::Type::Press, at));
    s.dispatch(pointer(PointerEvent::Type::Release, at));
}

struct Fixture {
    Surface surface;
    Timeline *tl = nullptr;
    Fixture() {
        surface.setSize({800, 300});
        tl = &surface.root().add<Timeline>();
        tl->setHeaderWidth(100);
        tl->setZoom(10); // 10 px per second
        tl->setTracks({{"V1", "Video", Color::fromRgba8(0x2f, 0x3e, 0x5c), 50.0f},
                       {"A1", "Audio", Color::fromRgba8(0x1b, 0x5a, 0x52), 40.0f, true}});
        tl->setClips({{1, 0, 0.0, 10.0, "one", {}, 7},
                      {2, 0, 20.0, 10.0, "two"},
                      {3, 1, 0.0, 10.0, "one audio", {}, 7}});
        surface.layout();
    }
    // A point inside clip `id`, `dx` px from its left edge.
    Vec2 in(std::uint64_t id, float dx = 30) const {
        const RectF r = tl->clipRect(*tl->clip(id));
        return {r.x + dx, r.y + r.height / 2};
    }
};

void geometry() {
    Fixture f;
    const RectF r = f.tl->rect();
    checkNear(f.tl->xAt(0), r.x + 100, 0.01, "time 0 is right of the headers");
    checkNear(f.tl->timeAt(r.x + 100 + 55), 5.5, 1e-6, "x back to time");
    const RectF c2 = f.tl->clipRect(*f.tl->clip(2));
    checkNear(c2.x, r.x + 300, 0.01, "a clip starts at start * zoom");
    checkNear(c2.width, 100, 0.01, "and is duration * zoom wide");
    checkNear(c2.y, r.y + Timeline::kRulerHeight + 3, 0.01, "inset 3 px in its track");
    checkNear(f.tl->duration(), 30.0, 1e-9, "duration is the last clip's end");
    checkNear(f.tl->fitZoom(), 16.5, 1e-9, "fit: the width over the duration plus 10 s, in halves");
}

void seekAndSelect() {
    Fixture f;
    double seeked = -1;
    ScopedConnection a = f.tl->seeked.connect([&](double t) { seeked = t; });
    std::optional<std::uint64_t> selected;
    int selections = 0;
    ScopedConnection b = f.tl->clipSelected.connect([&](std::optional<std::uint64_t> id) {
        selected = id;
        ++selections;
    });
    const RectF r = f.tl->rect();
    click(f.surface, {r.x + 100 + 125, r.y + 10});
    checkNear(seeked, 12.5, 1e-6, "a press on the ruler seeks");
    checkNear(f.tl->playhead(), 12.5, 1e-6, "and moves the playhead");
    dragTo(f.surface, {r.x + 100 + 125, r.y + 10}, {r.x + 100 + 200, r.y + 10});
    checkNear(seeked, 20.0, 1e-6, "dragging on the ruler scrubs");
    click(f.surface, f.in(2));
    check(selected == std::uint64_t(2), "a press on a clip selects it");
    check(f.tl->selectedClip() == std::uint64_t(2), "and the timeline shows it selected");
    click(f.surface, {r.x + 100 + 150, f.in(2).y});
    check(!selected.has_value() && !f.tl->selectedClip(), "a press on an empty lane deselects");
    checkEqual(selections, 2, "one signal per change");
}

void moveSnapAndGroups() {
    Fixture f;
    int started = 0, finished = 0, changes = 0;
    ScopedConnection a = f.tl->editStarted.connect([&] { ++started; });
    ScopedConnection b = f.tl->editFinished.connect([&](std::uint64_t) { ++finished; });
    ScopedConnection c = f.tl->clipChanged.connect([&](std::uint64_t) { ++changes; });
    click(f.surface, f.in(2));
    checkEqual(started, 0, "a press without movement edits nothing");
    // Drag clip 2 left by 94 px (9.4 s): its start 10.6 is within 8 px of
    // clip 1's end (10), so it snaps there.
    const Vec2 p = f.in(2);
    dragTo(f.surface, p, {p.x - 94, p.y});
    checkNear(f.tl->clip(2)->start, 10.0, 1e-9, "a moved clip snaps to a neighbour's edge");
    checkEqual(started, 1, "editStarted once");
    checkEqual(finished, 1, "editFinished once");
    check(changes >= 1, "clipChanged during the drag");
    // Without snapping it lands where dropped.
    f.tl->setSnapping(false);
    const Vec2 q = f.in(2);
    dragTo(f.surface, q, {q.x + 33, q.y});
    checkNear(f.tl->clip(2)->start, 13.3, 1e-9, "no snapping: where it was dropped");
    // Clip 1 and its audio (group 7) move together.
    const Vec2 g = f.in(1);
    dragTo(f.surface, g, {g.x + 50, g.y});
    checkNear(f.tl->clip(1)->start, 5.0, 1e-9, "the dragged clip moves");
    checkNear(f.tl->clip(3)->start, 5.0, 1e-9, "and its group with it");
}

void trim() {
    Fixture f;
    f.tl->setSnapping(false);
    const RectF r = f.tl->clipRect(*f.tl->clip(2));
    const float y = r.y + r.height / 2;
    dragTo(f.surface, {r.right() - 2, y}, {r.right() + 48, y});
    checkNear(f.tl->clip(2)->duration, 15.0, 1e-9, "the right handle trims the end");
    checkNear(f.tl->clip(2)->start, 20.0, 1e-9, "and keeps the start");
    const RectF r2 = f.tl->clipRect(*f.tl->clip(2));
    dragTo(f.surface, {r2.x + 2, y}, {r2.x + 32, y});
    checkNear(f.tl->clip(2)->start, 23.0, 1e-9, "the left handle moves the start");
    checkNear(f.tl->clip(2)->duration, 12.0, 1e-9, "and keeps the end");
    const RectF r3 = f.tl->clipRect(*f.tl->clip(2));
    dragTo(f.surface, {r3.x + 2, y}, {r3.x + 400, y});
    checkNear(f.tl->clip(2)->duration, 0.2, 1e-9, "a clip never trims below 0.2 s");
}

void lockedRazorFlags() {
    Fixture f;
    // Lock the audio track from its header button (the icons make them).
    Icon dot = Icon::fromSvg(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><circle cx="12" cy="12" r="6"/></svg>)");
    f.tl->setIcons({dot, dot, dot, dot, dot, dot});
    std::size_t flagTrack = 99;
    Timeline::TrackFlag flag = Timeline::TrackFlag::Hidden;
    ScopedConnection a = f.tl->trackFlagToggled.connect([&](std::size_t t, Timeline::TrackFlag fl) {
        flagTrack = t;
        flag = fl;
    });
    const RectF r = f.tl->rect();
    const float audioMid = r.y + Timeline::kRulerHeight + 50 + 19.5f;
    // The lock is the rightmost button: 6 px in from the header's edge.
    click(f.surface, {r.x + 100 - 1 - 6 - 11, audioMid});
    checkEqual(flagTrack, std::size_t(1), "the header button names its track");
    check(flag == Timeline::TrackFlag::Locked, "the rightmost button is the lock");
    check(f.tl->tracks()[1].locked, "and the track is now locked");
    click(f.surface, {r.x + 100 - 1 - 6 - 22 - 6 - 11, audioMid});
    check(f.tl->tracks()[1].muted, "an audio track's other button mutes");

    int lockedPresses = 0;
    ScopedConnection b = f.tl->lockedClipPressed.connect([&](std::uint64_t) { ++lockedPresses; });
    const Vec2 p = f.in(3);
    dragTo(f.surface, p, {p.x + 60, p.y});
    checkEqual(lockedPresses, 1, "a press on a locked track's clip is reported");
    checkNear(f.tl->clip(3)->start, 0.0, 1e-9, "and the clip stays");

    std::uint64_t cut = 0;
    double at = 0;
    ScopedConnection c = f.tl->razor.connect([&](std::uint64_t id, double t) {
        cut = id;
        at = t;
    });
    f.tl->setTool(Timeline::Tool::Razor);
    click(f.surface, f.in(2, 45));
    checkEqual(cut, std::uint64_t(2), "the razor names the clip");
    checkNear(at, 24.5, 1e-6, "and the time it was pressed at");
    checkNear(f.tl->clip(2)->duration, 10.0, 1e-9, "the timeline leaves the split to its owner");
}

void renaming() {
    Fixture f;
    std::size_t renamed = 99;
    String to;
    ScopedConnection a = f.tl->trackRenamed.connect([&](std::size_t t, const String &n) {
        renamed = t;
        to = n;
    });
    auto doubleClick = [&](Vec2 at) {
        f.surface.dispatch(pointer(PointerEvent::Type::Move, at));
        PointerEvent press = pointer(PointerEvent::Type::Press, at);
        f.surface.dispatch(press);
        f.surface.dispatch(pointer(PointerEvent::Type::Release, at));
        press.clickCount = 2;
        f.surface.dispatch(press);
        f.surface.dispatch(pointer(PointerEvent::Type::Release, at));
    };
    const RectF name = f.tl->trackNameRect(1);
    doubleClick({name.x + 10, name.y + name.height / 2});
    check(!f.tl->renamingTrack(), "names are not editable unless asked for");

    f.tl->setTrackNamesEditable(true);
    doubleClick({name.x + 10, name.y + name.height / 2});
    check(f.tl->renamingTrack() == std::optional<std::size_t>(1), "a double click on a name edits it");
    TextEvent typed;
    typed.text = "Audio - Game";
    f.surface.dispatch(typed); // replaces the selected old name
    KeyEvent enter;
    enter.key = Key::Enter;
    f.surface.dispatch(enter);
    check(!f.tl->renamingTrack(), "Enter finishes");
    checkEqual(renamed, std::size_t(1), "trackRenamed names the track");
    checkEqual(to, String("Audio - Game"), "and the new name");
    checkEqual(f.tl->tracks()[1].name, String("Audio - Game"), "which the track now has");

    renamed = 99;
    f.tl->renameTrack(0);
    TextEvent other;
    other.text = "Nope";
    f.surface.dispatch(other);
    KeyEvent escape;
    escape.key = Key::Escape;
    f.surface.dispatch(escape);
    check(!f.tl->renamingTrack(), "Escape cancels");
    checkEqual(f.tl->tracks()[0].name, String("Video"), "keeping the old name");
    checkEqual(renamed, std::size_t(99), "without a rename");

    f.tl->renameTrack(0);
    TextEvent third;
    third.text = "Video - Screen";
    f.surface.dispatch(third);
    click(f.surface, f.in(2)); // a click elsewhere keeps it
    checkEqual(f.tl->tracks()[0].name, String("Video - Screen"), "a click elsewhere keeps the name");
}

void multiSelect() {
    Fixture f;
    std::vector<std::uint64_t> last;
    ScopedConnection a = f.tl->selectionChanged.connect([&](const std::vector<std::uint64_t> &ids) { last = ids; });
    // A box from an empty spot over clips 1 and 2 on the video track
    // (clip 1 spans 0-10 s, clip 2 20-30 s; 10 px per second after a 100 px header).
    const RectF r = f.tl->rect();
    const float videoMid = r.y + Timeline::kRulerHeight + 25;
    const Vec2 from{r.x + 100 + 350, videoMid - 20};
    dragTo(f.surface, from, {r.x + 100 + 5, videoMid + 5});
    check(f.tl->isSelected(1) && f.tl->isSelected(2) && !f.tl->isSelected(3), "a box selects the clips it touches");
    checkEqual(last.size(), std::size_t(2), "and says so");

    // Shift + click adds the audio clip; again takes it out.
    PointerEvent press = pointer(PointerEvent::Type::Press, f.in(3));
    press.modifiers = Modifier::Shift;
    f.surface.dispatch(pointer(PointerEvent::Type::Move, f.in(3)));
    f.surface.dispatch(press);
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.in(3)));
    check(f.tl->isSelected(3) && f.tl->selectedClips().size() == 3, "Shift + click adds a clip");
    f.surface.dispatch(press);
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.in(3)));
    check(!f.tl->isSelected(3) && f.tl->selectedClips().size() == 2, "and takes it out again");

    // A click (no drag) on one of the selected clips selects only it.
    {
        Fixture g;
        g.tl->setSelectedClips({1, 2, 3});
        click(g.surface, g.in(2));
        check(g.tl->selectedClips() == std::vector<std::uint64_t>{2}, "a click on one of several selects just it");
    }
    // Dragging one selected clip moves the others too.
    const Vec2 p = f.in(2);
    dragTo(f.surface, p, {p.x + 50, p.y});
    checkNear(f.tl->clip(2)->start, 25.0, 0.2, "the dragged clip moves");
    checkNear(f.tl->clip(1)->start, 5.0, 0.2, "and the other selected one with it");

    // A plain press on an empty spot clears the selection and seeks there.
    double seekedTo = -1;
    ScopedConnection b = f.tl->seeked.connect([&](double t) { seekedTo = t; });
    // (An empty spot on the audio track at 25 s: its clip moved to 5-15 s.)
    const Vec2 spot{r.x + 100 + 250, r.y + Timeline::kRulerHeight + 50 + 20};
    click(f.surface, spot);
    check(f.tl->selectedClips().empty(), "a press on an empty spot clears the selection");
    checkNear(seekedTo, f.tl->timeAt(spot.x), 0.05, "and moves the playhead there");
}

void wheelAndPaint() {
    Fixture f;
    double zoomed = 0;
    ScopedConnection a = f.tl->zoomChanged.connect([&](double z) { zoomed = z; });
    PointerEvent w = pointer(PointerEvent::Type::Wheel, {f.tl->xAt(10), f.tl->rect().y + 60});
    w.wheelDelta = {0, 1};
    w.modifiers = Modifier::Control;
    const float before = f.tl->xAt(10);
    f.surface.dispatch(w);
    checkNear(zoomed, 12.5, 1e-9, "Ctrl + wheel zooms in");
    checkNear(f.tl->xAt(10), before, 0.6, "around the pointer");

    f.tl->setSelectedClip(1);
    f.tl->setPlayhead(15.0);
    f.tl->setMarkers({{5.0, Color::fromRgba8(0xe3, 0x28, 0x3f), "Ace"}});
    f.tl->setBadges({{9, 0, 10.0, {}, "Crossfade"}});
    int bodies = 0;
    f.tl->paintClipBody = [&](Painter &, const Timeline::Clip &, const RectF &, const Theme &) { ++bodies; };
    Image image = Image::create(800, 300, AlphaMode::Premultiplied).value();
    {
        RasterPaintBackend backend(image);
        Painter painter(backend);
        f.surface.paint(painter);
    }
    checkEqual(bodies, 3, "every visible clip's body is painted by the owner");
    // The playhead is the error colour at the lanes' left edge.
    const std::size_t at = (std::uint32_t(f.tl->rect().y + 20) * image.width() + std::uint32_t(std::lround(f.tl->xAt(15.0)))) * 4u;
    check(image.pixels()[at] > image.pixels()[at + 2], "the playhead is drawn in the theme's red");

    std::uint64_t badge = 0;
    ScopedConnection b = f.tl->badgeSelected.connect([&](std::uint64_t id) { badge = id; });
    click(f.surface, {f.tl->xAt(10), f.tl->rect().y + Timeline::kRulerHeight + 24.5f});
    checkEqual(badge, std::uint64_t(9), "a press on a badge selects it");
    check(!f.tl->selectedClip(), "and deselects the clip");
}

} // namespace

int main() {
    geometry();
    seekAndSelect();
    moveSnapAndGroups();
    trim();
    lockedRazorFlags();
    renaming();
    multiSelect();
    wheelAndPaint();
    return cfw::test::finish("TimelineTest");
}
