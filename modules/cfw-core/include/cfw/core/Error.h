#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "cfw/core/String.h"

namespace cfw {

// Stable error codes. They are logged and may be persisted or sent over the
// network, so values never change: append new codes, never renumber.
enum class ErrorCode : std::uint32_t {
    Unknown = 0,
    InvalidArgument = 1,
    OutOfRange = 2,
    ParseError = 3,
    TypeMismatch = 4,
    LimitExceeded = 5,
    NotFound = 6,
    AlreadyExists = 7,
    PermissionDenied = 8,
    IoError = 9,
    Unsupported = 10,
    Cancelled = 11,
    Timeout = 12,
    OutOfMemory = 13,
    Corrupt = 14,
    NetworkError = 15,
};

// The code's name, e.g. "ParseError". Never null.
[[nodiscard]] const char *errorCodeName(ErrorCode code) noexcept;

// Why an operation failed: a stable code, a message for humans, and optional
// key/value context (file name, line, byte offset...).
//
// Threads: a value type; any thread may use its own copy.
// Allocates: the message and context strings.
class Error {
public:
    Error(ErrorCode code, String message) : m_code(code), m_message(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }
    [[nodiscard]] const String &message() const noexcept { return m_message; }
    [[nodiscard]] const std::vector<std::pair<String, String>> &context() const noexcept { return m_context; }

    // Adds context and returns the error, so it can be chained on return:
    //   return Error(ErrorCode::ParseError, "bad token").with("line", "3");
    Error &with(String key, String value) & {
        m_context.emplace_back(std::move(key), std::move(value));
        return *this;
    }
    Error &&with(String key, String value) && {
        m_context.emplace_back(std::move(key), std::move(value));
        return std::move(*this);
    }

    // "ParseError: bad token (line=3, column=7)"
    [[nodiscard]] String describe() const;

private:
    ErrorCode m_code;
    String m_message;
    std::vector<std::pair<String, String>> m_context;
};

} // namespace cfw
