#pragma once

// Capturing the screen through the system's consent dialog: on Linux the
// freedesktop desktop portal (org.freedesktop.portal.ScreenCast), which
// works on Wayland and X11 alike. The user picks a monitor or a window in
// the desktop's own dialog; the result is a PipeWire stream that a recorder
// (GStreamer's pipewiresrc, OBS, ...) reads from the descriptor given here.
// With a restore token from an earlier session the desktop can reuse that
// choice without asking again.
//
//     ScreenCastOptions o;
//     o.restoreToken = settings.getString("cast.token");
//     auto cast = ScreenCast::start(o);           // off the UI thread: it waits for the user
//     if (cast) {
//         settings.set("cast.token", cast.value()->restoreToken());
//         recorder.start(cast.value()->pipeWireFd(), cast.value()->nodeId());
//     }
//
// The stream lasts as long as the ScreenCast object. takeScreenshot asks the
// portal for one still image instead.
//
// Other platforms report Unsupported: Windows applications capture with the
// system's own APIs (ffmpeg's gdigrab / ddagrab, Windows.Graphics.Capture).
//
// Threads: start and takeScreenshot block until the desktop answers; call
// them from a worker thread. A ScreenCast is used from one thread at a time.

#include <chrono>
#include <cstdint>
#include <memory>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/core/Vec2.h"
#include "cfw/io/Path.h"

namespace cfw {

struct ScreenCastOptions {
    bool monitors = true;
    bool windows = true;
    bool cursor = true;       // drawn into the frames when the desktop can
    String restoreToken;      // from an earlier ScreenCast: reuse its choice
    std::chrono::seconds timeout{300}; // for the user to choose
};

class ScreenCast {
public:
    // Whether this desktop offers screen casting (the portal is there).
    [[nodiscard]] static bool available();
    // Asks the user (or reuses a remembered choice) and starts the stream.
    // Cancelled when the user declines.
    [[nodiscard]] static Result<std::unique_ptr<ScreenCast>> start(const ScreenCastOptions &options);
    ~ScreenCast();
    ScreenCast(const ScreenCast &) = delete;
    ScreenCast &operator=(const ScreenCast &) = delete;

    // A connection to PipeWire for this stream (owned here; pass a copy or
    // the descriptor itself to a child process).
    [[nodiscard]] int pipeWireFd() const noexcept;
    // The stream's PipeWire node.
    [[nodiscard]] std::uint32_t nodeId() const noexcept;
    // The source's size in pixels, when the desktop says (else 0, 0).
    [[nodiscard]] Vec2i size() const noexcept;
    // Pass to the next start() to skip the dialog (empty when the desktop
    // does not remember).
    [[nodiscard]] const String &restoreToken() const noexcept;

    struct Impl;

private:
    explicit ScreenCast(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

// One screenshot of the whole screen, saved where the desktop chooses (its
// path is returned); Cancelled when the user declines.
[[nodiscard]] Result<Path> takeScreenshot(std::chrono::seconds timeout = std::chrono::seconds(60));

} // namespace cfw
