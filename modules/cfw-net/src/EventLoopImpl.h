#pragma once

// Private to cfw-net: the event loop's state and its socket-watch interface.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Socket.h"
#include "cfw/core/Clock.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/TcpConnection.h"

namespace cfw::detail {

using WatchId = std::uint64_t;

struct Readiness {
    bool readable = false;
    bool writable = false;
    bool failed = false;
};

struct EventLoopImpl {
    struct Watch {
        SocketHandle socket = kInvalidSocket;
        bool read = false;
        bool write = false;
        std::function<void(Readiness)> callback;
    };
    struct TimerEntry {
        TimePoint deadline;
        TimerId id;
        bool operator>(const TimerEntry &other) const noexcept {
            return deadline != other.deadline ? deadline > other.deadline : id > other.id;
        }
    };
    struct Timer {
        Duration interval{}; // zero: one-shot
        std::function<void()> callback;
    };

    explicit EventLoopImpl(NetLibrary library, WakePair wake) : library(std::move(library)), wake(wake) {}
    ~EventLoopImpl() {
        closeSocket(wake.readEnd);
        closeSocket(wake.writeEnd);
    }

    WatchId addWatch(SocketHandle socket, bool read, bool write, std::function<void(Readiness)> callback) {
        const WatchId id = nextWatchId++;
        watches.emplace(id, Watch{socket, read, write, std::move(callback)});
        return id;
    }
    void updateWatch(WatchId id, bool read, bool write) {
        if (const auto it = watches.find(id); it != watches.end()) {
            it->second.read = read;
            it->second.write = write;
        }
    }
    void removeWatch(WatchId id) noexcept { watches.erase(id); }

    void iterate(Duration maxWait);

    // Write coalescing: a connection with new outgoing data registers here
    // once; the loop flushes it before it next blocks, so every message queued
    // during one pass goes out in a single send() per socket.
    void scheduleFlush(DeferredFlush *target) { pendingFlush.push_back(target); }
    void cancelFlush(DeferredFlush *target) noexcept {
        for (DeferredFlush *&entry : pendingFlush) {
            if (entry == target) {
                entry = nullptr;
            }
        }
        for (DeferredFlush *&entry : flushing) {
            if (entry == target) {
                entry = nullptr;
            }
        }
    }
    void runDeferredFlushes() {
        // Flushing may schedule more, or destroy connections (which cancel
        // themselves in both lists), so work from a swapped-out list.
        while (!pendingFlush.empty()) {
            flushing.swap(pendingFlush);
            for (std::size_t i = 0; i < flushing.size(); ++i) {
                if (DeferredFlush *target = flushing[i]) {
                    flushing[i] = nullptr;
                    target->flushDeferred();
                }
            }
            flushing.clear();
        }
    }

    NetLibrary library;
    WakePair wake;
    std::thread::id loopThread = std::this_thread::get_id();
    std::atomic<bool> stopRequested{false};

    std::map<WatchId, Watch> watches; // ordered: dispatch order is registration order
    WatchId nextWatchId = 1;

    std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>> timerQueue;
    std::unordered_map<TimerId, Timer> timers;
    TimerId nextTimerId = 1;

    std::mutex postedMutex;
    std::vector<std::function<void()>> posted;
    std::vector<PollEntry> pollScratch;
    std::vector<WatchId> pollIds;
    std::vector<std::byte> pollBuffer;
    std::vector<DeferredFlush *> pendingFlush;
    std::vector<DeferredFlush *> flushing;
};

} // namespace cfw::detail
