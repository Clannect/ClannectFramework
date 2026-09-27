#pragma once

// Precondition checks. A broken contract is a bug in the caller, never an
// expected failure (those return Result). Release behaviour is always defined:
// require() aborts, debugCheck() is skipped. Neither is ever undefined
// behaviour. Both are constexpr: a violation during constant evaluation is a
// compile error.
//
// Threads: any. Allocates: nothing. On failure: prints to stderr and aborts.

#include <source_location>

#include "cfw/core/Config.h"

namespace cfw {

// Prints the violated contract and its location, then aborts. Never returns.
[[noreturn]] void contractViolation(const char *what, std::source_location where) noexcept;

// Checked in every build. Use where continuing would corrupt data or memory.
constexpr void require(bool condition, const char *what,
                    std::source_location where = std::source_location::current()) noexcept {
    if (!condition) [[unlikely]] {
        contractViolation(what, where);
    }
}

// Checked in debug builds only. The condition is still evaluated in release,
// so it must be cheap and free of side effects.
constexpr void debugCheck(bool condition, const char *what,
                       std::source_location where = std::source_location::current()) noexcept {
    if constexpr (kDebugChecks) {
        if (!condition) [[unlikely]] {
            contractViolation(what, where);
        }
    } else {
        (void)condition;
        (void)what;
        (void)where;
    }
}

} // namespace cfw
