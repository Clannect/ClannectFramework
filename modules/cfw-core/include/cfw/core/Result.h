#pragma once

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "cfw/core/Contract.h"
#include "cfw/core/Error.h"

namespace cfw {

// The outcome of a fallible operation: a T, or the Error explaining why not.
// [[nodiscard]] on the type means ignoring any returned Result is a warning
// (an error under CFW's build flags).
//
// Reading value() of a failed Result, or error() of a successful one, is a
// contract violation and aborts in every build.
//
// Threads: a value type. Allocates: whatever T and Error allocate.
template <class T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported; return a pointer or Result<T*>");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is ambiguous");

public:
    // Success, from anything T can be built from.
    template <class U = T>
        requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, Result> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Error>)
    Result(U &&value) : m_storage(std::in_place_index<0>, std::forward<U>(value)) {}

    // Failure.
    Result(Error error) : m_storage(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return m_storage.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T &value() & {
        require(ok(), "Result::value() called on a failed Result");
        return *std::get_if<0>(&m_storage);
    }
    [[nodiscard]] const T &value() const & {
        require(ok(), "Result::value() called on a failed Result");
        return *std::get_if<0>(&m_storage);
    }
    [[nodiscard]] T &&value() && {
        require(ok(), "Result::value() called on a failed Result");
        return std::move(*std::get_if<0>(&m_storage));
    }

    [[nodiscard]] const Error &error() const & {
        require(!ok(), "Result::error() called on a successful Result");
        return *std::get_if<1>(&m_storage);
    }
    [[nodiscard]] Error &&error() && {
        require(!ok(), "Result::error() called on a successful Result");
        return std::move(*std::get_if<1>(&m_storage));
    }

    // The value, or `fallback` if this failed. The error is dropped on purpose.
    template <class U = T>
    [[nodiscard]] T valueOr(U &&fallback) const & {
        return ok() ? *std::get_if<0>(&m_storage) : static_cast<T>(std::forward<U>(fallback));
    }
    template <class U = T>
    [[nodiscard]] T valueOr(U &&fallback) && {
        return ok() ? std::move(*std::get_if<0>(&m_storage)) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> m_storage;
};

// Success or an Error, with no value.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return !m_error.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error &error() const & {
        require(!ok(), "Result::error() called on a successful Result");
        return *m_error;
    }
    [[nodiscard]] Error &&error() && {
        require(!ok(), "Result::error() called on a successful Result");
        return std::move(*m_error);
    }

private:
    std::optional<Error> m_error;
};

// A successful Result<void>, for readability at return sites.
[[nodiscard]] inline Result<void> success() { return {}; }

} // namespace cfw
