// HTTP/1.1 heads (the WebSocket server's handshake, the HTTP client's
// responses) and response bodies (Content-Length, chunked, until-close), all
// from untrusted peers.

#include "cfw/net/HttpBodyDecoder.h"
#include "cfw/net/WebSocketHandshake.h"

#include <cstdlib>

#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::StringView text(reinterpret_cast<const char *>(data), size);

    if (auto request = cfw::parseHttpRequestHead(text); request && request.value()) {
        (void)cfw::wsServerResponse(request.value()->head);
    }

    auto response = cfw::parseHttpResponseHead(text);
    if (!response || !response.value()) {
        return 0;
    }
    const cfw::ParsedHead &parsed = *response.value();
    if (parsed.consumed > size) {
        std::abort();
    }
    (void)cfw::wsCheckServerResponse(parsed.head, "dGhlIHNhbXBsZSBub25jZQ==");

    constexpr std::size_t kMaxBody = 64 * 1024;
    auto decoder = cfw::HttpBodyDecoder::forResponse(parsed.head, "GET", kMaxBody);
    if (!decoder) {
        return 0;
    }
    std::size_t delivered = 0;
    const cfw::Span<const std::byte> body(reinterpret_cast<const std::byte *>(data) + parsed.consumed,
                                          size - parsed.consumed);
    auto done = decoder.value().feed(body, [&](cfw::Span<const std::byte> bytes) { delivered += bytes.size(); });
    if (done && !done.value()) {
        (void)decoder.value().finishOnClose();
    }
    if (delivered > kMaxBody) {
        std::abort(); // the body limit must hold however the body is framed
    }
    return 0;
}
