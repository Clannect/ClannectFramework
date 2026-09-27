#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "cfw/core/Clock.h"
#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/core/Url.h"
#include "cfw/net/TcpConnection.h"

namespace cfw {

class EventLoop;
class Executor;

struct HttpRequest {
    String method = "GET";
    Url url;
    // Extra headers. Host, Content-Length, Transfer-Encoding and Connection
    // are set by the client and may not be given here.
    std::vector<std::pair<String, String>> headers;
    std::vector<std::byte> body;
};

struct HttpResponse {
    int status = 0;
    String reason;
    std::vector<std::pair<String, String>> headers;
    // Empty when the body was streamed to HttpOptions::onBodyData.
    std::vector<std::byte> body;
    // The URL that produced this response (after redirects).
    Url url;
    int redirects = 0;

    [[nodiscard]] const String *header(StringView name) const noexcept;
    [[nodiscard]] StringView bodyText() const noexcept {
        return {reinterpret_cast<const char *>(body.data()), body.size()};
    }
};

struct HttpOptions {
    Duration connectTimeout = std::chrono::seconds(10);
    // No bytes received for this long: give up.
    Duration idleTimeout = std::chrono::seconds(30);
    // The whole exchange, redirects included.
    Duration totalTimeout = std::chrono::minutes(2);
    std::size_t maxBodyBytes = 64u * 1024u * 1024u;
    std::size_t maxHeadBytes = 64u * 1024u;
    // 0 disables following redirects (the 3xx response is returned as is).
    int maxRedirects = 5;
    // Streaming: if set, body bytes of the final response go here as they
    // arrive instead of into HttpResponse::body. Return false to cancel.
    std::function<bool(Span<const std::byte>)> onBodyData;
};

// An HTTP/1.1 client (RFC 9110/9112) on an EventLoop. Each request uses its own
// connection. The callback receives the response, which may be any status:
// a 404 is a successful exchange. It receives an Error for network failures,
// timeouts, malformed or oversized responses, and redirect policy violations.
//
// Redirect policy (301, 302, 303, 307, 308):
//  - at most maxRedirects hops;
//  - never from https to http (a downgrade);
//  - Authorization, Cookie and Proxy-Authorization are dropped when the
//    redirect leaves the original origin;
//  - 303, and 301/302 after a POST, continue as GET without a body; 307/308
//    keep the method and body.
//
// https:// currently fails with Unsupported: TLS is not implemented yet.
//
// Threads: the loop thread only. Allocates: per request, bounded by the limits.
class HttpClient {
public:
    using Callback = std::function<void(Result<HttpResponse>)>;

    HttpClient(EventLoop &loop, Executor &resolver) noexcept : m_loop(loop), m_resolver(resolver) {}

    // Starts the request. The returned handle cancels it when destroyed (the
    // callback then never runs); keep it alive until the callback fires.
    [[nodiscard]] ConnectRequest send(HttpRequest request, Callback callback, HttpOptions options);
    [[nodiscard]] ConnectRequest send(HttpRequest request, Callback callback) {
        return send(std::move(request), std::move(callback), HttpOptions{});
    }
    [[nodiscard]] ConnectRequest get(const Url &url, Callback callback, HttpOptions options = {}) {
        HttpRequest request;
        request.url = url;
        return send(std::move(request), std::move(callback), std::move(options));
    }

private:
    EventLoop &m_loop;
    Executor &m_resolver;
};

} // namespace cfw
