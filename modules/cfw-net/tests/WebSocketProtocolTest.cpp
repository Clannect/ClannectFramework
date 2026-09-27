// RFC 6455 wire format and handshake, without sockets: the RFC's own examples
// byte for byte, every validation rule, and incremental (byte-at-a-time)
// decoding.

#include "cfw/net/WebSocketFrame.h"
#include "cfw/net/WebSocketHandshake.h"

#include <initializer_list>

#include "cfw/core/Strings.h"

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

Span<const std::byte> text(StringView s) { return {reinterpret_cast<const std::byte *>(s.data()), s.size()}; }

// Feeds everything, returns all messages, or the close code of the first failure.
struct Decoded {
    std::vector<WsMessage> messages;
    std::uint16_t failure = 0;
};
Decoded decodeAll(WsRole role, const std::vector<std::byte> &data, bool byteByByte = false, WsLimits limits = {}) {
    WsDecoder decoder(role, limits);
    Decoded result;
    const auto drain = [&] {
        while (true) {
            auto next = decoder.next();
            if (!next) {
                result.failure = wsCloseCodeOf(next.error());
                return false;
            }
            if (!next.value()) {
                return true;
            }
            result.messages.push_back(std::move(*next.value()));
        }
    };
    if (byteByByte) {
        for (std::byte b : data) {
            decoder.feed(Span<const std::byte>(&b, 1));
            if (!drain()) {
                break;
            }
        }
    } else {
        decoder.feed(data);
        drain();
    }
    return result;
}

void rfcExamplesEncode() {
    // RFC 6455 §5.7.
    check(encodeWsFrame(WsOpcode::Text, text("Hello"), true, std::nullopt) ==
              bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}),
          "single-frame unmasked text");
    const std::array<std::byte, 4> key{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};
    check(encodeWsFrame(WsOpcode::Text, text("Hello"), true, key) ==
              bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}),
          "single-frame masked text");
    check(encodeWsFrame(WsOpcode::Ping, text("Hello"), true, std::nullopt) ==
              bytes({0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}),
          "unmasked ping");
    const std::vector<std::byte> binary256(256, std::byte{0});
    const auto frame256 = encodeWsFrame(WsOpcode::Binary, binary256, true, std::nullopt);
    check(frame256.size() == 260 && frame256[1] == std::byte{0x7E} && frame256[2] == std::byte{0x01} &&
              frame256[3] == std::byte{0x00},
          "256 bytes: 16-bit length");
    const std::vector<std::byte> binary64k(65536, std::byte{0});
    const auto frame64k = encodeWsFrame(WsOpcode::Binary, binary64k, true, std::nullopt);
    check(frame64k.size() == 65546 && frame64k[1] == std::byte{0x7F} && frame64k[7] == std::byte{0x01},
          "64 KiB: 64-bit length");
}

void rfcExamplesDecode() {
    const auto masked = decodeAll(WsRole::Server, bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}));
    check(masked.failure == 0 && masked.messages.size() == 1 && masked.messages[0].text() == "Hello",
          "server decodes the masked example");

    // Fragmented "Hel" + "lo", with a ping in between (allowed mid-message).
    const auto fragmented = decodeAll(WsRole::Client, bytes({0x01, 0x03, 0x48, 0x65, 0x6c,   // "Hel", not final
                                                             0x89, 0x01, 0x21,               // ping "!"
                                                             0x80, 0x02, 0x6c, 0x6f}),       // "lo", final
                                      true);
    check(fragmented.failure == 0 && fragmented.messages.size() == 2, "ping delivered, then the whole message");
    check(fragmented.messages.size() == 2 && fragmented.messages[0].kind == WsMessage::Kind::Ping,
          "the control frame comes out first");
    check(fragmented.messages.size() == 2 && fragmented.messages[1].text() == "Hello", "fragments reassembled");
}

void roundTripsEverySizeClass() {
    for (std::size_t size : {0u, 1u, 125u, 126u, 65535u, 65536u, 300000u}) {
        std::vector<std::byte> payload(size);
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<std::byte>(i * 31);
        }
        const std::array<std::byte, 4> key{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        const auto decoded = decodeAll(WsRole::Server, encodeWsFrame(WsOpcode::Binary, payload, true, key));
        check(decoded.failure == 0 && decoded.messages.size() == 1 && decoded.messages[0].payload == payload,
              "masked binary round-trips at every length encoding");
    }
}

void closeFrames() {
    const auto normal = decodeAll(WsRole::Client, encodeWsFrame(WsOpcode::Close, encodeWsClosePayload(4001, "kicked"), true, std::nullopt));
    check(normal.messages.size() == 1 && normal.messages[0].closeCode == 4001 && normal.messages[0].text() == "kicked",
          "close code and reason");
    const auto empty = decodeAll(WsRole::Client, bytes({0x88, 0x00}));
    check(empty.messages.size() == 1 && empty.messages[0].closeCode == ws::kNoStatus, "no payload: 1005");
    checkEqual(decodeAll(WsRole::Client, bytes({0x88, 0x01, 0x03})).failure, ws::kProtocolError, "1-byte close payload");
    checkEqual(decodeAll(WsRole::Client, bytes({0x88, 0x02, 0x03, 0xED})).failure, ws::kProtocolError,
               "reserved close code 1005 on the wire");
    checkEqual(decodeAll(WsRole::Client, bytes({0x88, 0x02, 0x03, 0xE7})).failure, ws::kProtocolError,
               "unassigned close code 999");
    checkEqual(decodeAll(WsRole::Client, bytes({0x88, 0x04, 0x03, 0xE8, 0xC3, 0x28})).failure, ws::kInvalidPayload,
               "close reason must be UTF-8");
    const String longReason(200, 'x');
    checkEqual(encodeWsClosePayload(1000, longReason).size(), std::size_t(125), "reason cut to fit 125 bytes");
    const String accents(100, 'a');
    const auto cut = encodeWsClosePayload(1000, accents + "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
    const auto recoded = decodeAll(WsRole::Client, encodeWsFrame(WsOpcode::Close, cut, true, std::nullopt));
    check(recoded.failure == 0, "a cut reason is still valid UTF-8");
}

void protocolViolations() {
    checkEqual(decodeAll(WsRole::Server, bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f})).failure, ws::kProtocolError,
               "server rejects unmasked client frames");
    checkEqual(decodeAll(WsRole::Client, bytes({0x81, 0x85, 1, 2, 3, 4, 0, 0, 0, 0, 0})).failure, ws::kProtocolError,
               "client rejects masked server frames");
    checkEqual(decodeAll(WsRole::Client, bytes({0xC1, 0x00})).failure, ws::kProtocolError, "RSV1 without extension");
    checkEqual(decodeAll(WsRole::Client, bytes({0x83, 0x00})).failure, ws::kProtocolError, "reserved opcode 3");
    checkEqual(decodeAll(WsRole::Client, bytes({0x09, 0x00})).failure, ws::kProtocolError, "fragmented ping");
    checkEqual(decodeAll(WsRole::Client, bytes({0x89, 0x7E, 0x00, 0x7E})).failure, ws::kProtocolError,
               "ping over 125 bytes");
    checkEqual(decodeAll(WsRole::Client, bytes({0x80, 0x00})).failure, ws::kProtocolError,
               "continuation with no message");
    checkEqual(decodeAll(WsRole::Client, bytes({0x01, 0x01, 0x61, 0x81, 0x01, 0x62})).failure, ws::kProtocolError,
               "new message inside a fragmented one");
    checkEqual(decodeAll(WsRole::Client, bytes({0x82, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 1})).failure, ws::kProtocolError,
               "64-bit length with the top bit set");
    checkEqual(decodeAll(WsRole::Client, bytes({0x81, 0x02, 0xC3, 0x28})).failure, ws::kInvalidPayload,
               "text must be UTF-8");
    // A text message split inside a UTF-8 character is fine once reassembled.
    const auto split = decodeAll(WsRole::Client, bytes({0x01, 0x01, 0xC3, 0x80, 0x01, 0xA9}));
    check(split.failure == 0 && split.messages.size() == 1 && split.messages[0].text() == "\xC3\xA9",
          "UTF-8 is checked on the whole message, not per fragment");
}

void limitsAreEnforcedFromTheHeader() {
    WsLimits limits;
    limits.maxFrameBytes = 1000;
    limits.maxMessageBytes = 1500;
    // A header claiming 1 GiB: rejected before any payload is buffered.
    const auto huge = decodeAll(WsRole::Client, bytes({0x82, 0x7F, 0, 0, 0, 0, 0x40, 0, 0, 0}), false, limits);
    checkEqual(huge.failure, ws::kMessageTooBig, "oversized frame header rejected immediately");

    // A single frame over the frame limit but under the message limit: only
    // the frame limit can catch it.
    const std::vector<std::byte> payload1200(1200, std::byte{'f'});
    checkEqual(decodeAll(WsRole::Client, encodeWsFrame(WsOpcode::Binary, payload1200, true, std::nullopt), false, limits).failure,
               ws::kMessageTooBig, "frame over the frame limit (under the message limit)");

    std::vector<std::byte> fragments;
    const std::vector<std::byte> chunk(800, std::byte{'a'});
    auto first = encodeWsFrame(WsOpcode::Binary, chunk, false, std::nullopt);
    auto second = encodeWsFrame(WsOpcode::Continuation, chunk, true, std::nullopt);
    fragments.insert(fragments.end(), first.begin(), first.end());
    fragments.insert(fragments.end(), second.begin(), second.end());
    checkEqual(decodeAll(WsRole::Client, fragments, false, limits).failure, ws::kMessageTooBig,
               "fragments summing past the message limit");
}

void failureIsSticky() {
    WsDecoder decoder(WsRole::Client, {});
    decoder.feed(bytes({0x83, 0x00, 0x81, 0x01, 0x61}));
    check(!decoder.next().ok(), "first frame fails");
    check(!decoder.next().ok() && decoder.failed(), "and the decoder stays failed");
}

void handshakeServerSide() {
    const String request = "GET /chat HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\n"
                           "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                           "Origin: http://example.com\r\nSec-WebSocket-Version: 13\r\n\r\nextra";
    const auto parsed = parseHttpRequestHead(request);
    check(parsed.ok() && parsed.value().has_value(), "complete request parses");
    const HttpHead &head = parsed.value()->head;
    checkEqual(head.target, String("/chat"), "target");
    checkEqual(parsed.value()->consumed, request.size() - 5, "bytes after the head are left for the WebSocket");
    const Result<String> response = wsServerResponse(head);
    check(response.ok() && response.value().find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != String::npos,
          "RFC 6455 example accept key");

    check(parseHttpRequestHead("GET / HTTP/1.1\r\nHost: x\r\n").ok() &&
              !parseHttpRequestHead("GET / HTTP/1.1\r\nHost: x\r\n").value().has_value(),
          "incomplete head: wait for more");
    check(!parseHttpRequestHead(String(9000, 'a')).ok(), "head over 8 KB rejected");

    const auto without = [&](StringView drop) {
        String edited;
        for (StringView line : split(StringView(request).substr(0, request.size() - 5), "\r\n")) {
            if (!line.starts_with(drop)) {
                edited += String(line) + "\r\n";
            }
        }
        const auto p = parseHttpRequestHead(edited + "\r\n");
        return p.ok() && p.value() ? wsServerResponse(p.value()->head).ok() : false;
    };
    check(!without("Upgrade:"), "missing Upgrade rejected");
    check(!without("Connection:"), "missing Connection rejected");
    check(!without("Sec-WebSocket-Key"), "missing key rejected");
    check(!without("Sec-WebSocket-Version"), "missing version rejected");
}

void handshakeClientSide() {
    const String key = "dGhlIHNhbXBsZSBub25jZQ==";
    const String req = wsClientRequest("127.0.0.1", 8080, "/game?ticket=abc", key);
    check(req.starts_with("GET /game?ticket=abc HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"), "request line and host");
    const auto good = parseHttpResponseHead("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                            "Connection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n");
    check(good.ok() && good.value() && wsCheckServerResponse(good.value()->head, key).ok(), "valid 101 accepted");
    const auto wrong = parseHttpResponseHead("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                             "Connection: Upgrade\r\nSec-WebSocket-Accept: AAAA\r\n\r\n");
    check(!wsCheckServerResponse(wrong.value()->head, key).ok(), "wrong accept key rejected");
    const auto refused = parseHttpResponseHead("HTTP/1.1 403 Forbidden\r\n\r\n");
    check(!wsCheckServerResponse(refused.value()->head, key).ok(), "non-101 rejected");
    check(wsClientRequest("::1", 80, "/", key).find("Host: [::1]:80") != String::npos, "IPv6 host is bracketed");
}

// Hostile input: random bytes and mutated valid streams must decode, or fail
// with a close code, and never crash (runs under UBSan too).
void survivesFuzzedInput() {
    std::uint32_t seed = 12345;
    const auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    std::vector<std::byte> valid;
    const std::array<std::byte, 4> key{std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
    for (int i = 0; i < 20; ++i) {
        const String message = "message " + std::to_string(i) + String(static_cast<std::size_t>(i * 37), 'x');
        const auto frame = encodeWsFrame(i % 3 == 0 ? WsOpcode::Text : WsOpcode::Binary, text(message), true, key);
        valid.insert(valid.end(), frame.begin(), frame.end());
    }
    int failures = 0;
    int decodedOk = 0;
    for (int round = 0; round < 8000; ++round) {
        std::vector<std::byte> input = valid;
        const int edits = 1 + static_cast<int>(rnd() % 6);
        for (int e = 0; e < edits; ++e) {
            input[rnd() % input.size()] = static_cast<std::byte>(rnd() & 0xFF);
        }
        if (round % 4 == 0) {
            input.resize(rnd() % 64); // short random garbage
            for (std::byte &b : input) {
                b = static_cast<std::byte>(rnd() & 0xFF);
            }
        }
        WsLimits limits;
        limits.maxFrameBytes = 4096;
        limits.maxMessageBytes = 8192;
        const auto result = decodeAll(round % 2 ? WsRole::Server : WsRole::Client, input, round % 10 == 0, limits);
        if (result.failure != 0) {
            ++failures;
            check(result.failure == ws::kProtocolError || result.failure == ws::kMessageTooBig ||
                      result.failure == ws::kInvalidPayload,
                  "failures always carry a protocol close code");
        } else {
            ++decodedOk;
        }
        (void)parseHttpRequestHead(StringView(reinterpret_cast<const char *>(input.data()), input.size()));
        (void)parseHttpResponseHead(StringView(reinterpret_cast<const char *>(input.data()), input.size()));
    }
    check(failures > 0 && decodedOk > 0, "fuzzing exercised both outcomes");
}

} // namespace

int main() {
    survivesFuzzedInput();
    rfcExamplesEncode();
    rfcExamplesDecode();
    roundTripsEverySizeClass();
    closeFrames();
    protocolViolations();
    limitsAreEnforcedFromTheHeader();
    failureIsSticky();
    handshakeServerSide();
    handshakeClientSide();
    return cfw::test::finish("WebSocketProtocolTest");
}
