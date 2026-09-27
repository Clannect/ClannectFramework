#include "cfw/net/EventLoop.h"

#include <algorithm>
#include <chrono>

#include "EventLoopImpl.h"
#include "cfw/core/Contract.h"

namespace cfw {

namespace detail {

void EventLoopImpl::iterate(Duration maxWait) {
    // 1. Posted tasks (swap out first: a task may post more, which run next time).
    std::vector<std::function<void()>> tasks;
    {
        const std::lock_guard lock(postedMutex);
        tasks.swap(posted);
    }
    for (auto &task : tasks) {
        task();
    }
    runDeferredFlushes();

    // 2. How long may we block? Until the next timer, capped by maxWait; not
    //    at all if work was posted meanwhile.
    Duration wait = maxWait;
    if (!timerQueue.empty()) {
        wait = std::min(wait, std::max(Duration::zero(), timerQueue.top().deadline - Clock::now()));
    }
    {
        const std::lock_guard lock(postedMutex);
        if (!posted.empty() || stopRequested.load() || !pendingFlush.empty()) {
            wait = Duration::zero();
        }
    }

    // 3. Wait for sockets (the wake socket is always entry 0).
    pollScratch.clear();
    pollIds.clear();
    pollScratch.push_back(PollEntry{wake.readEnd, true, false});
    pollIds.push_back(0);
    for (const auto &[id, watch] : watches) {
        if (watch.read || watch.write) {
            pollScratch.push_back(PollEntry{watch.socket, watch.read, watch.write});
            pollIds.push_back(id);
        }
    }
    // Round up so a timer due in 0.4 ms does not spin with timeout 0.
    const auto waitMs = std::chrono::ceil<std::chrono::milliseconds>(wait).count();
    const int timeoutMs = static_cast<int>(std::min<long long>(waitMs, 60'000));
    const Result<int> polled = pollSockets(pollScratch, timeoutMs, pollBuffer);
    if (polled && polled.value() > 0) {
        if (pollScratch[0].readable) {
            drainWake(wake.readEnd);
        }
        for (std::size_t i = 1; i < pollScratch.size(); ++i) {
            const PollEntry &entry = pollScratch[i];
            if (!entry.readable && !entry.writable && !entry.failed) {
                continue;
            }
            // Look the watch up again: an earlier callback may have removed it.
            const auto it = watches.find(pollIds[i]);
            if (it == watches.end()) {
                continue;
            }
            const std::function<void(Readiness)> callback = it->second.callback; // survives removal mid-call
            callback(Readiness{entry.readable, entry.writable, entry.failed});
        }
    }

    // 4. Due timers.
    const TimePoint now = Clock::now();
    while (!timerQueue.empty() && timerQueue.top().deadline <= now) {
        const TimerEntry entry = timerQueue.top();
        timerQueue.pop();
        const auto it = timers.find(entry.id);
        if (it == timers.end()) {
            continue; // cancelled
        }
        std::function<void()> callback = it->second.callback;
        if (it->second.interval > Duration::zero()) {
            timerQueue.push({entry.deadline + it->second.interval, entry.id});
        } else {
            timers.erase(it);
        }
        callback();
    }
    // Everything the callbacks above sent goes out before the loop blocks.
    runDeferredFlushes();
}

} // namespace detail

EventLoop::EventLoop(std::unique_ptr<detail::EventLoopImpl> impl) : m_impl(std::move(impl)) {}

EventLoop::~EventLoop() = default;

Result<std::unique_ptr<EventLoop>> EventLoop::create() {
    Result<detail::NetLibrary> library = detail::NetLibrary::acquire();
    if (!library) {
        return std::move(library).error();
    }
    Result<detail::WakePair> wake = detail::createWakePair();
    if (!wake) {
        return std::move(wake).error();
    }
    auto impl = std::make_unique<detail::EventLoopImpl>(std::move(library).value(), wake.value());
    return std::unique_ptr<EventLoop>(new EventLoop(std::move(impl)));
}

void EventLoop::run() {
    m_impl->loopThread = std::this_thread::get_id();
    m_impl->stopRequested = false;
    while (!m_impl->stopRequested.load()) {
        m_impl->iterate(std::chrono::hours(1));
    }
}

bool EventLoop::runUntil(const std::function<bool()> &done, Duration timeout) {
    m_impl->loopThread = std::this_thread::get_id();
    const TimePoint deadline = Clock::now() + timeout;
    while (!done()) {
        const TimePoint now = Clock::now();
        if (now >= deadline) {
            return false;
        }
        m_impl->iterate(std::min<Duration>(deadline - now, std::chrono::milliseconds(50)));
    }
    return true;
}

void EventLoop::runOnce(Duration maxWait) {
    m_impl->loopThread = std::this_thread::get_id();
    m_impl->iterate(maxWait);
}

void EventLoop::stop() {
    m_impl->stopRequested = true;
    detail::signalWake(m_impl->wake.writeEnd);
}

void EventLoop::post(std::function<void()> task) {
    {
        const std::lock_guard lock(m_impl->postedMutex);
        m_impl->posted.push_back(std::move(task));
    }
    detail::signalWake(m_impl->wake.writeEnd);
}

TimerId EventLoop::startTimer(Duration delay, std::function<void()> callback) {
    debugCheck(isLoopThread(), "timers belong to the loop thread");
    const TimerId id = m_impl->nextTimerId++;
    m_impl->timers.emplace(id, detail::EventLoopImpl::Timer{Duration::zero(), std::move(callback)});
    m_impl->timerQueue.push({Clock::now() + delay, id});
    return id;
}

TimerId EventLoop::startRepeatingTimer(Duration interval, std::function<void()> callback) {
    debugCheck(isLoopThread(), "timers belong to the loop thread");
    require(interval > Duration::zero(), "repeating timer needs a positive interval");
    const TimerId id = m_impl->nextTimerId++;
    m_impl->timers.emplace(id, detail::EventLoopImpl::Timer{interval, std::move(callback)});
    m_impl->timerQueue.push({Clock::now() + interval, id});
    return id;
}

void EventLoop::cancelTimer(TimerId id) noexcept { m_impl->timers.erase(id); }

bool EventLoop::isLoopThread() const noexcept { return std::this_thread::get_id() == m_impl->loopThread; }

} // namespace cfw
