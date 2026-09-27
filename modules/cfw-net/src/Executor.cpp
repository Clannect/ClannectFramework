#include "cfw/net/Executor.h"

#include <algorithm>

namespace cfw {

Executor::Executor(std::size_t threads) {
    const std::size_t count = std::max<std::size_t>(threads, 1);
    m_threads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        m_threads.emplace_back([this] { work(); });
    }
}

Executor::~Executor() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_queue.clear();
    }
    m_wake.notify_all();
    for (std::thread &thread : m_threads) {
        thread.join();
    }
}

void Executor::submit(std::function<void()> task) {
    {
        const std::lock_guard lock(m_mutex);
        if (m_stopping) {
            return;
        }
        m_queue.push_back(std::move(task));
    }
    m_wake.notify_one();
}

void Executor::work() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping) {
                return;
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

} // namespace cfw
