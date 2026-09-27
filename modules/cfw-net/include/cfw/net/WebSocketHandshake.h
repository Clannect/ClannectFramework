#pragma once

// The WebSocket opening handshake (RFC 6455 §4): an HTTP/1.1 Upgrade request
// and its 101 response. Pure text processing, no sockets.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw {

// The head of an HTTP/1.1 message: request line or status line, then headers.
// Header names are matched case-insensitively.
struct HttpHead {
    // Request: method, target, version. Response: version, status, reason.
    String method;
    String target;
    int status = 0;
    String reason;
    std::vector<std::pair<String, String>> headers;

    // The first header named `name` (case-insensitive), or null.
    [[nodiscard]] const String *header(StringView name) const noexcept;
    // True if header `name` holds `token` in its comma-separated list
    // (case-insensitive), e.g. "Connection: keep-alive, Upgrade".
    [[nodiscard]] bool headerHasToken(StringView name, StringView token) const noexcept;
};

// Parses an HTTP message head from the start of `buffer`. Returns nothing if
// the blank line ending the head has not arrived yet; fails if the head is
// malformed or longer than `maxBytes`. On success, also returns how many bytes
// the head used (bytes after it belong to the next protocol).
struct ParsedHead {
    HttpHead head;
    std::size_t consumed = 0;
};
[[nodiscard]] Result<std::optional<ParsedHead>> parseHttpRequestHead(StringView buffer, std::size_t maxBytes = 8192);
[[nodiscard]] Result<std::optional<ParsedHead>> parseHttpResponseHead(StringView buffer, std::size_t maxBytes = 8192);

// base64(SHA-1(key + RFC 6455 GUID)).
[[nodiscard]] String wsAcceptKey(StringView clientKey);

// Server side: checks an upgrade request. On success returns the 101 response
// to send; on failure an Error whose message suits a 400 response.
[[nodiscard]] Result<String> wsServerResponse(const HttpHead &request);
[[nodiscard]] String httpErrorResponse(int status, StringView reason);

// Client side: the upgrade request, and the check of the server's answer.
// `key` is 16 random bytes, base64-encoded.
[[nodiscard]] String wsClientRequest(StringView host, std::uint16_t port, StringView target, StringView key);
[[nodiscard]] Result<void> wsCheckServerResponse(const HttpHead &response, StringView key);

} // namespace cfw
