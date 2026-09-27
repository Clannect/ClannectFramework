#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "cfw/core/Clock.h"
#include "cfw/core/Result.h"

namespace cfw {

namespace detail {
struct EventLoopImpl;
}

using TimerId = std::uint64_t;

// A single-threaded event loop: socket readiness, timers, and tasks posted
// from other threads. Every cfw-net object belongs to one loop and is used
// only on that loop's thread; callbacks run on it.
//
// The loop is idle when there is nothing to do: it blocks in the OS until a
// socket is ready, a timer is due, or post()/stop() wakes it. No polling
// interval, no busy wait (spec §7: ~0% CPU when idle).
//
// Threads: run(), timers and socket objects on the loop thread only;
// post() and stop() from any thread.
// Allocates: per timer, per posted task, per watched socket.
class EventLoop {
public:
    [[nodiscard]] static Result<std::unique_ptr<EventLoop>> create();
    ~EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    // Runs until stop(). The thread that calls run() becomes the loop thread.
    void run();
    // Runs until `done()` is true or `timeout` passes; returns done(). For
    // tests and for bounded waits during shutdown.
    bool runUntil(const std::function<bool()> &done, Duration timeout);
    // Processes whatever is ready, waiting at most `maxWait`.
    void runOnce(Duration maxWait);

    // Makes run() return after the current iteration. Any thread.
    void stop();
    // Runs `task` on the loop thread, in posting order. Any thread.
    void post(std::function<void()> task);

    // One-shot and repeating timers. The callback may cancel any timer,
    // including its own.
    TimerId startTimer(Duration delay, std::function<void()> callback);
    TimerId startRepeatingTimer(Duration interval, std::function<void()> callback);
    void cancelTimer(TimerId id) noexcept;

    [[nodiscard]] bool isLoopThread() const noexcept;

    // Internal to cfw-net: socket readiness registration.
    detail::EventLoopImpl &impl() noexcept { return *m_impl; }

private:
    explicit EventLoop(std::unique_ptr<detail::EventLoopImpl> impl);
    std::unique_ptr<detail::EventLoopImpl> m_impl;
};

} // namespace cfw
