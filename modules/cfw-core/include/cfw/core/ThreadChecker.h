#pragma once

#include <source_location>
#include <thread>

#include "cfw/core/Contract.h"

namespace cfw {

// Records the thread that created an object and, in debug builds, checks that
// later calls come from the same thread. Embed one in any class documented as
// single-thread (all UI objects) and call check() at the top of public
// methods. In release builds check() compiles to nothing.
//
// Threads: construct on the owning thread; check() from anywhere.
// Allocates: nothing.
class ThreadChecker {
public:
    ThreadChecker() noexcept : m_owner(std::this_thread::get_id()) {}

    void check(std::source_location where = std::source_location::current()) const noexcept {
        if constexpr (kDebugChecks) {
            debugCheck(std::this_thread::get_id() == m_owner, "called from a thread that does not own this object",
                       where);
        } else {
            (void)where;
        }
    }

    [[nodiscard]] bool isOwnerThread() const noexcept { return std::this_thread::get_id() == m_owner; }

    // Hands the object to the calling thread (e.g. after building it on a
    // worker and moving it to the main thread).
    void rebind() noexcept { m_owner = std::this_thread::get_id(); }

private:
    std::thread::id m_owner;
};

} // namespace cfw
