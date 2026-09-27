#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cfw {

// A fixed pool of worker threads for blocking work that must not run on an
// event-loop thread: DNS lookups, file compression, image decoding. Results
// go back to a loop with EventLoop::post().
//
// Destruction is deterministic: queued tasks that have not started are
// dropped, running ones finish, and every thread is joined before the
// destructor returns.
//
// Threads: submit() from any thread. Allocates: per task.
class Executor {
public:
    explicit Executor(std::size_t threads = 2);
    ~Executor();
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;

    void submit(std::function<void()> task);
    [[nodiscard]] std::size_t threadCount() const noexcept { return m_threads.size(); }

private:
    void work();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_queue;
    bool m_stopping = false;
    std::vector<std::thread> m_threads;
};

} // namespace cfw
