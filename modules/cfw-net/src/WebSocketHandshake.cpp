#include "cfw/net/WebSocketHandshake.h"

#include "cfw/core/Base64.h"
#include "cfw/core/Sha1.h"
#include "cfw/core/Strings.h"

namespace cfw {

namespace {

constexpr StringView kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

Error badRequest(const char *message) { return Error(ErrorCode::ParseError, message); }

// Splits the head into lines and fills the headers; `firstLine` receives the
// request or status line.
Result<std::optional<ParsedHead>> parseHead(StringView buffer, std::size_t maxBytes, StringView &firstLine) {
    const std::size_t end = buffer.find("\r\n\r\n");
    if (end == StringView::npos) {
        if (buffer.size() > maxBytes) {
            return Error(ErrorCode::LimitExceeded, "HTTP head too large");
        }
        return std::optional<ParsedHead>();
    }
    if (end + 4 > maxBytes) {
        return Error(ErrorCode::LimitExceeded, "HTTP head too large");
    }
    ParsedHead parsed;
    parsed.consumed = end + 4;
    const std::vector<StringView> lines = split(buffer.substr(0, end), "\r\n");
    firstLine = lines.front();
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const StringView line = lines[i];
        const std::size_t colon = line.find(':');
        if (colon == StringView::npos || colon == 0) {
            return badRequest("malformed HTTP header line");
        }
        const StringView name = line.substr(0, colon);
        if (name.find_first_of(" \t") != StringView::npos) {
            return badRequest("whitespace in HTTP header name");
        }
        parsed.head.headers.emplace_back(String(name), String(trim(line.substr(colon + 1))));
    }
    return std::optional<ParsedHead>(std::move(parsed));
}

} // namespace

const String *HttpHead::header(StringView name) const noexcept {
    for (const auto &[key, value] : headers) {
        if (equalsIgnoreCase(key, name)) {
            return &value;
        }
    }
    return nullptr;
}

bool HttpHead::headerHasToken(StringView name, StringView token) const noexcept {
    for (const auto &[key, value] : headers) {
        if (!equalsIgnoreCase(key, name)) {
            continue;
        }
        for (StringView part : split(value, ',')) {
            if (equalsIgnoreCase(trim(part), token)) {
                return true;
            }
        }
    }
    return false;
}

Result<std::optional<ParsedHead>> parseHttpRequestHead(StringView buffer, std::size_t maxBytes) {
    StringView first;
    Result<std::optional<ParsedHead>> parsed = parseHead(buffer, maxBytes, first);
    if (!parsed || !parsed.value()) {
        return parsed;
    }
    const std::vector<StringView> parts = split(first, ' ');
    if (parts.size() != 3 || !parts[2].starts_with("HTTP/1.")) {
        return badRequest("malformed HTTP request line");
    }
    HttpHead &head = parsed.value()->head;
    head.method = String(parts[0]);
    head.target = String(parts[1]);
    return parsed;
}

Result<std::optional<ParsedHead>> parseHttpResponseHead(StringView buffer, std::size_t maxBytes) {
    StringView first;
    Result<std::optional<ParsedHead>> parsed = parseHead(buffer, maxBytes, first);
    if (!parsed || !parsed.value()) {
        return parsed;
    }
    if (!first.starts_with("HTTP/1.") || first.size() < 12 || first[8] != ' ') {
        return badRequest("malformed HTTP status line");
    }
    const Result<std::int64_t> status = parseInt(first.substr(9, 3));
    if (!status) {
        return badRequest("malformed HTTP status code");
    }
    HttpHead &head = parsed.value()->head;
    head.status = static_cast<int>(status.value());
    head.reason = first.size() > 13 ? String(first.substr(13)) : String();
    return parsed;
}

String wsAcceptKey(StringView clientKey) {
    Sha1 sha;
    sha.update(clientKey);
    sha.update(kGuid);
    const Sha1::Digest digest = sha.finish();
    return base64Encode(Span<const std::byte>(reinterpret_cast<const std::byte *>(digest.data()), digest.size()));
}

Result<String> wsServerResponse(const HttpHead &request) {
    if (request.method != "GET") {
        return badRequest("WebSocket upgrade must be a GET request");
    }
    if (!request.headerHasToken("Upgrade", "websocket")) {
        return badRequest("missing 'Upgrade: websocket'");
    }
    if (!request.headerHasToken("Connection", "Upgrade")) {
        return badRequest("missing 'Connection: Upgrade'");
    }
    const String *version = request.header("Sec-WebSocket-Version");
    if (version == nullptr || *version != "13") {
        return badRequest("unsupported Sec-WebSocket-Version (13 required)");
    }
    const String *key = request.header("Sec-WebSocket-Key");
    const Result<std::vector<std::byte>> nonce = key ? base64Decode(*key) : Result<std::vector<std::byte>>(badRequest(""));
    if (!nonce || nonce.value().size() != 16) {
        return badRequest("Sec-WebSocket-Key must be 16 base64-encoded bytes");
    }
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
           wsAcceptKey(*key) + "\r\n\r\n";
}

String httpErrorResponse(int status, StringView reason) {
    const String body = String(reason) + "\n";
    return "HTTP/1.1 " + std::to_string(status) + " " + (status == 400 ? "Bad Request" : "Error") +
           "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

String wsClientRequest(StringView host, std::uint16_t port, StringView target, StringView key) {
    String hostHeader(host);
    if (hostHeader.find(':') != String::npos && hostHeader.front() != '[') {
        hostHeader = "[" + hostHeader + "]"; // IPv6 literal
    }
    return "GET " + String(target.empty() ? "/" : target) + " HTTP/1.1\r\nHost: " + hostHeader + ":" +
           std::to_string(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + String(key) +
           "\r\nSec-WebSocket-Version: 13\r\n\r\n";
}

Result<void> wsCheckServerResponse(const HttpHead &response, StringView key) {
    if (response.status != 101) {
        return Error(ErrorCode::NetworkError, "server refused the WebSocket upgrade")
            .with("status", std::to_string(response.status))
            .with("reason", response.reason);
    }
    if (!response.headerHasToken("Upgrade", "websocket") || !response.headerHasToken("Connection", "Upgrade")) {
        return Error(ErrorCode::NetworkError, "server response is not a WebSocket upgrade");
    }
    const String *accept = response.header("Sec-WebSocket-Accept");
    if (accept == nullptr || *accept != wsAcceptKey(key)) {
        return Error(ErrorCode::NetworkError, "server sent a wrong Sec-WebSocket-Accept");
    }
    if (response.header("Sec-WebSocket-Extensions") != nullptr || response.header("Sec-WebSocket-Protocol") != nullptr) {
        return Error(ErrorCode::NetworkError, "server selected an extension or subprotocol that was not offered");
    }
    return success();
}

} // namespace cfw
