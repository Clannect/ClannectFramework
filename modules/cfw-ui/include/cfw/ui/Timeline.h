#pragma once

// A multi-track editing timeline (a video editor's, an audio sequencer's):
// a time ruler with markers and a playhead, track headers with mute / hide /
// lock buttons, and clips that are moved and trimmed by dragging, with
// snapping to the playhead and to other clips' edges.
//
//     Timeline &tl = root.add<Timeline>();
//     tl.setTracks({{"V1", "Gameplay", Color::fromRgba8(0x2f, 0x3e, 0x5c), 54.0f}});
//     tl.setClips({{1, 0, 0.0, 38.0, "Round 4"}});
//     tl.setZoom(6.0);                    // pixels per second
//     tl.seeked.connect([&](double t) { player.seek(t); });
//     tl.editFinished.connect([&](std::uint64_t) { document.save(); });
//
// The timeline holds its own copy of the tracks and clips and edits that
// copy as the user drags; it tells you through signals:
//
//   editStarted   before the first change of a gesture (take an undo
//                 snapshot here), then clipChanged during it and
//                 editFinished at the end. A press that does not move does
//                 none of the three.
//   clipSelected  a press on a clip, or nothing on an empty lane.
//   razor         a press with Tool::Razor: where to split (the timeline
//                 does not split; the owner does and calls setClips).
//
// Clips in the same `group` (a video clip and its own audio) move and trim
// together. A clip on a locked track does not move: lockedClipPressed tells
// you, so you can say why.
//
// What a clip shows under its name (frames, a waveform) is the owner's: set
// paintClipBody. Everything else is drawn from the theme and the colours
// given here.
//
// Input: the wheel scrolls the tracks, Shift + wheel (or a horizontal wheel)
// scrolls time, Ctrl + wheel zooms around the pointer.
//
// Threads: the surface's thread.

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "cfw/core/Color.h"
#include "cfw/core/Signal.h"
#include "cfw/text/TextLayout.h"
#include "cfw/ui/Element.h"
#include "cfw/ui/Icon.h"

namespace cfw {

class Timeline : public Element {
public:
    struct Track {
        String shortName; // "V1", drawn small before the name
        String name;      // "Gameplay"
        Color color;      // its clips' colour
        float height = 46.0f;
        bool audio = false; // a mute button instead of hide
        bool muted = false;
        bool hidden = false;
        bool locked = false;
    };
    struct Clip {
        std::uint64_t id = 0;
        std::size_t track = 0; // index into tracks()
        double start = 0.0;    // seconds
        double duration = 0.0;
        String label;
        Icon icon;              // before the label (optional)
        std::uint64_t group = 0; // non-zero: moves and trims with its group
        bool accent = false;     // a 2 px accent line on top (a suggestion)
    };
    struct Marker {
        double time = 0.0;
        Color color;
        String label; // its tooltip
    };
    // A small button on a track at a time (a transition between two clips).
    struct Badge {
        std::uint64_t id = 0;
        std::size_t track = 0;
        double time = 0.0;
        Icon icon;
        String toolTip;
    };
    enum class Tool : std::uint8_t { Select, Razor };
    enum class TrackFlag : std::uint8_t { Muted, Hidden, Locked };
    // The header buttons' icons (none set: the buttons are not drawn).
    struct Icons {
        Icon audible, muted, visible, hidden, locked, unlocked;
    };

    Timeline();

    // ---- Content (setters do not emit) ----
    void setTracks(std::vector<Track> tracks);
    [[nodiscard]] const std::vector<Track> &tracks() const noexcept { return m_tracks; }
    void setClips(std::vector<Clip> clips);
    [[nodiscard]] const std::vector<Clip> &clips() const noexcept { return m_clips; }
    [[nodiscard]] const Clip *clip(std::uint64_t id) const;
    void setMarkers(std::vector<Marker> markers);
    void setBadges(std::vector<Badge> badges);
    void setIcons(Icons icons);
    // Draws what a clip shows below its name (frames, a waveform) in `body`.
    std::function<void(Painter &, const Clip &, const RectF &body, const Theme &)> paintClipBody;

    // ---- View ----
    void setPlayhead(double seconds);
    [[nodiscard]] double playhead() const noexcept { return m_playhead; }
    // Pixels per second, within [minimumZoom, maximumZoom].
    void setZoom(double pixelsPerSecond);
    [[nodiscard]] double zoom() const noexcept { return m_zoom; }
    static constexpr double kMinimumZoom = 0.5;
    static constexpr double kMaximumZoom = 60.0;
    // The zoom that shows everything in the current width.
    [[nodiscard]] double fitZoom() const;
    void setSnapping(bool snapping) noexcept { m_snapping = snapping; }
    [[nodiscard]] bool snapping() const noexcept { return m_snapping; }
    void setTool(Tool tool);
    [[nodiscard]] Tool tool() const noexcept { return m_tool; }
    void setSelectedClip(std::optional<std::uint64_t> id);
    [[nodiscard]] std::optional<std::uint64_t> selectedClip() const noexcept { return m_selected; }
    void setSelectedBadge(std::optional<std::uint64_t> id);
    void setHeaderWidth(float width);
    [[nodiscard]] float headerWidth() const noexcept { return m_headerWidth; }
    static constexpr float kRulerHeight = 26.0f;
    // The end of the last clip (at least 10 s).
    [[nodiscard]] double duration() const;
    // Scroll position: x in pixels of time, y in pixels of tracks.
    [[nodiscard]] Vec2 scrollOffset() const noexcept { return m_scroll; }
    void setScrollOffset(Vec2 offset);
    // Scrolls so the playhead is in view (playback follows it).
    void ensurePlayheadVisible();
    // Time at an x in surface coordinates, and back.
    [[nodiscard]] double timeAt(float x) const;
    [[nodiscard]] float xAt(double time) const;
    // A clip's rectangle in surface coordinates (empty when not shown).
    [[nodiscard]] RectF clipRect(const Clip &clip) const;

    // ---- Signals ----
    Signal<double> seeked;                                // the user moved the playhead
    Signal<std::optional<std::uint64_t>> clipSelected;    // a press on a clip / on an empty lane
    Signal<std::uint64_t> badgeSelected;
    Signal<> editStarted;                                 // a drag is about to change clips
    Signal<std::uint64_t> clipChanged;                    // during the drag
    Signal<std::uint64_t> editFinished;                   // the drag ended
    Signal<std::uint64_t, double> razor;                  // Tool::Razor pressed a clip at a time
    Signal<std::uint64_t> lockedClipPressed;
    Signal<std::size_t, TrackFlag> trackFlagToggled;      // after the flag changed
    Signal<std::uint64_t, Vec2> clipContextMenuRequested; // surface position
    Signal<double> zoomChanged;                           // Ctrl + wheel

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    [[nodiscard]] std::optional<Cursor> cursorAt(Vec2 position) const override;
    void describeAccessible(AccessibleNode &node) const override;

    static constexpr float kHandle = 7.0f;   // trim handles at each end
    static constexpr float kSnapPixels = 8.0f;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    enum class Part : std::uint8_t { None, Ruler, Header, Lane, Clip, LeftHandle, RightHandle, Badge, FlagButton };
    struct Hit {
        Part part = Part::None;
        std::size_t track = 0;
        std::size_t clip = 0; // index into m_clips
        std::size_t badge = 0;
        TrackFlag flag = TrackFlag::Locked;
    };
    [[nodiscard]] Hit hitTest(Vec2 position) const;
    [[nodiscard]] float trackTop(std::size_t track) const; // surface y, scrolled
    [[nodiscard]] float tracksHeight() const;
    [[nodiscard]] RectF lanesRect() const;
    [[nodiscard]] RectF flagRect(std::size_t track, TrackFlag flag) const;
    [[nodiscard]] Vec2 maxScroll() const;
    [[nodiscard]] double snap(double time, double pps) const;
    void drag(Vec2 position);
    void clampScroll();
    const TextLayout &textLayout(StringView text, const std::shared_ptr<const FontFace> &font, float size,
                                 const Theme &theme);
    void paintRuler(Painter &painter, const Theme &theme, const RectF &lanes);
    void paintClip(Painter &painter, const Theme &theme, const Clip &clip, bool dimmed);

    std::vector<Track> m_tracks;
    std::vector<Clip> m_clips;
    std::vector<Marker> m_markers;
    std::vector<Badge> m_badges;
    Icons m_icons;
    double m_playhead = 0.0;
    double m_zoom = 6.0;
    bool m_snapping = true;
    Tool m_tool = Tool::Select;
    std::optional<std::uint64_t> m_selected;
    std::optional<std::uint64_t> m_selectedBadge;
    float m_headerWidth = 176.0f;
    Vec2 m_scroll;

    // The gesture in progress.
    enum class Drag : std::uint8_t { None, Seek, Move, TrimLeft, TrimRight };
    Drag m_drag = Drag::None;
    std::size_t m_dragClip = 0;
    float m_pressX = 0.0f;
    bool m_moved = false;
    struct Original {
        std::size_t index;
        double start, duration;
    };
    std::vector<Original> m_original; // the dragged clip first, then its group

    std::unordered_map<std::string, TextLayout> m_text;
};

} // namespace cfw
