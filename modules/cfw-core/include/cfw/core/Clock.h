#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "cfw/core/String.h"

namespace cfw {

// Monotonic time for measuring intervals: never jumps when the wall clock is
// changed. Use it for frame timing, timeouts and benchmarks.
using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using TimePoint = Clock::time_point;

[[nodiscard]] constexpr double toSeconds(Duration d) noexcept { return std::chrono::duration<double>(d).count(); }
[[nodiscard]] constexpr double toMilliseconds(Duration d) noexcept {
    return std::chrono::duration<double, std::milli>(d).count();
}

// Wall-clock time in milliseconds since the Unix epoch (UTC), for timestamps
// that leave the process (logs, saved files, network). Not for intervals.
[[nodiscard]] inline std::int64_t unixTimeMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Wall-clock seconds since the Unix epoch (UTC).
[[nodiscard]] inline std::int64_t unixTimeSeconds() noexcept { return unixTimeMilliseconds() / 1000; }

// "2026-09-27T19:38:55Z": a Unix time in UTC, in ISO 8601 as
// QDateTime::toString(Qt::ISODate) writes UTC times (whole seconds).
[[nodiscard]] String formatIsoUtc(std::int64_t unixSeconds);

// Measures elapsed monotonic time. Starts running on construction.
//
// Threads: one instance per thread. Allocates: nothing.
class Stopwatch {
public:
    Stopwatch() noexcept : m_start(Clock::now()) {}

    [[nodiscard]] Duration elapsed() const noexcept { return Clock::now() - m_start; }
    [[nodiscard]] double elapsedSeconds() const noexcept { return toSeconds(elapsed()); }
    [[nodiscard]] double elapsedMilliseconds() const noexcept { return toMilliseconds(elapsed()); }
    // Returns the elapsed time and starts again from now.
    Duration restart() noexcept {
        const TimePoint now = Clock::now();
        const Duration elapsed = now - m_start;
        m_start = now;
        return elapsed;
    }

private:
    TimePoint m_start;
};

// One frame's timing, as the frame loop hands it to simulation and animation.
struct FrameTime {
    // Seconds since the previous frame, clamped to FrameTimer::kMaxDelta.
    double delta = 0.0;
    // Seconds of clamped deltas since the timer started.
    double total = 0.0;
    std::uint64_t index = 0;
    // Exponentially smoothed frames per second, for display.
    double smoothedFps = 0.0;
};

// Produces FrameTime from successive tick() calls. The time source is passed
// in, so tests and replays can drive it deterministically.
//
// A long stall (a breakpoint, a window drag on Windows, a sleeping laptop)
// would otherwise hand the simulation one enormous step; the delta is clamped
// so that never happens.
//
// Threads: one instance per thread (the frame loop's). Allocates: nothing.
class FrameTimer {
public:
    static constexpr double kMaxDelta = 0.25;

    // First call returns a zero delta.
    FrameTime tick(TimePoint now) noexcept {
        FrameTime frame;
        if (m_started) {
            frame.delta = std::min(toSeconds(now - m_last), kMaxDelta);
            if (frame.delta < 0.0) {
                frame.delta = 0.0;
            }
            m_total += frame.delta;
            ++m_index;
            if (frame.delta > 0.0) {
                const double fps = 1.0 / frame.delta;
                m_smoothedFps = m_smoothedFps == 0.0 ? fps : m_smoothedFps * 0.9 + fps * 0.1;
            }
        }
        m_started = true;
        m_last = now;
        frame.total = m_total;
        frame.index = m_index;
        frame.smoothedFps = m_smoothedFps;
        return frame;
    }
    FrameTime tick() noexcept { return tick(Clock::now()); }

private:
    TimePoint m_last{};
    double m_total = 0.0;
    double m_smoothedFps = 0.0;
    std::uint64_t m_index = 0;
    bool m_started = false;
};

} // namespace cfw
