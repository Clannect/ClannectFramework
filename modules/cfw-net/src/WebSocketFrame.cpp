#include "cfw/net/WebSocketFrame.h"

#include "cfw/core/Contract.h"
#include "cfw/core/Strings.h"
#include "cfw/core/Utf8.h"

namespace cfw {

namespace ws {

bool isSendableCloseCode(std::uint16_t code) noexcept {
    if (code >= 3000 && code <= 4999) {
        return true; // registered libraries (3xxx) and applications (4xxx)
    }
    switch (code) {
    case 1000: case 1001: case 1002: case 1003: case 1007: case 1008:
    case 1009: case 1010: case 1011: case 1012: case 1013: case 1014:
        return true;
    default:
        return false; // 1004-1006 and 1015 are reserved; the rest unassigned
    }
}

} // namespace ws

namespace {

bool isControl(WsOpcode op) noexcept { return static_cast<std::uint8_t>(op) >= 8; }

bool isKnown(std::uint8_t op) noexcept { return op <= 2 || (op >= 8 && op <= 10); }

} // namespace

std::vector<std::byte> encodeWsFrame(WsOpcode opcode, Span<const std::byte> payload, bool final,
                                     std::optional<std::array<std::byte, 4>> mask) {
    std::vector<std::byte> frame;
    frame.reserve(payload.size() + 14);
    appendWsFrame(frame, opcode, payload, final, mask);
    return frame;
}

void appendWsFrame(std::vector<std::byte> &frame, WsOpcode opcode, Span<const std::byte> payload, bool final,
                   std::optional<std::array<std::byte, 4>> mask) {
    require(!isControl(opcode) || (payload.size() <= 125 && final), "control frames are short and unfragmented");
    frame.push_back(static_cast<std::byte>((final ? 0x80u : 0u) | static_cast<std::uint8_t>(opcode)));
    const std::uint8_t maskBit = mask ? 0x80u : 0u;
    const std::uint64_t length = payload.size();
    if (length <= 125) {
        frame.push_back(static_cast<std::byte>(maskBit | length));
    } else if (length <= 0xFFFF) {
        frame.push_back(static_cast<std::byte>(maskBit | 126u));
        frame.push_back(static_cast<std::byte>(length >> 8));
        frame.push_back(static_cast<std::byte>(length & 0xFF));
    } else {
        frame.push_back(static_cast<std::byte>(maskBit | 127u));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<std::byte>((length >> shift) & 0xFF));
        }
    }
    if (mask) {
        frame.insert(frame.end(), mask->begin(), mask->end());
        const std::size_t start = frame.size();
        frame.insert(frame.end(), payload.begin(), payload.end());
        for (std::size_t i = 0; i < payload.size(); ++i) {
            frame[start + i] ^= (*mask)[i % 4];
        }
    } else {
        frame.insert(frame.end(), payload.begin(), payload.end());
    }
}

std::vector<std::byte> encodeWsClosePayload(std::uint16_t code, StringView reason) {
    // Cut the reason at a character boundary so it fits in 123 bytes.
    std::size_t length = std::min<std::size_t>(reason.size(), 123);
    while (length > 0 && length < reason.size() && (static_cast<unsigned char>(reason[length]) & 0xC0u) == 0x80u) {
        --length;
    }
    std::vector<std::byte> payload;
    payload.push_back(static_cast<std::byte>(code >> 8));
    payload.push_back(static_cast<std::byte>(code & 0xFF));
    const auto *text = reinterpret_cast<const std::byte *>(reason.data());
    payload.insert(payload.end(), text, text + length);
    return payload;
}

void WsDecoder::feed(Span<const std::byte> data) {
    if (m_failed) {
        return;
    }
    // Compact consumed bytes before growing.
    if (m_offset > 0 && m_offset * 2 >= m_buffer.size()) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_offset));
        m_offset = 0;
    }
    m_buffer.insert(m_buffer.end(), data.begin(), data.end());
}

Error WsDecoder::fail(std::uint16_t closeCode, const char *reason) {
    m_failed = true;
    return Error(ErrorCode::ParseError, reason).with("close-code", std::to_string(closeCode));
}

Result<std::optional<WsMessage>> WsDecoder::next() {
    while (true) {
        if (m_failed) {
            return Error(ErrorCode::ParseError, "WebSocket stream already failed").with("close-code", "1002");
        }
        const Span<const std::byte> data = Span<const std::byte>(m_buffer).subspan(m_offset);
        if (data.size() < 2) {
            return std::optional<WsMessage>();
        }
        const auto b0 = std::to_integer<std::uint8_t>(data[0]);
        const auto b1 = std::to_integer<std::uint8_t>(data[1]);
        const bool final = (b0 & 0x80u) != 0;
        const std::uint8_t op = b0 & 0x0Fu;
        const bool masked = (b1 & 0x80u) != 0;

        if ((b0 & 0x70u) != 0) {
            return fail(ws::kProtocolError, "reserved bits set (no extension was negotiated)");
        }
        if (!isKnown(op)) {
            return fail(ws::kProtocolError, "unknown opcode");
        }
        const auto opcode = static_cast<WsOpcode>(op);
        if (m_role == WsRole::Server && !masked) {
            return fail(ws::kProtocolError, "client frames must be masked");
        }
        if (m_role == WsRole::Client && masked) {
            return fail(ws::kProtocolError, "server frames must not be masked");
        }

        std::size_t headerSize = 2;
        std::uint64_t length = b1 & 0x7Fu;
        if (length == 126) {
            if (data.size() < 4) {
                return std::optional<WsMessage>();
            }
            length = std::to_integer<std::uint64_t>(data[2]) << 8 | std::to_integer<std::uint64_t>(data[3]);
            headerSize = 4;
        } else if (length == 127) {
            if (data.size() < 10) {
                return std::optional<WsMessage>();
            }
            length = 0;
            for (std::size_t i = 2; i < 10; ++i) {
                length = length << 8 | std::to_integer<std::uint64_t>(data[i]);
            }
            if ((length >> 63) != 0) {
                return fail(ws::kProtocolError, "frame length has the top bit set");
            }
            headerSize = 10;
        }
        if (isControl(opcode) && (!final || length > 125)) {
            return fail(ws::kProtocolError, "control frames must be final and at most 125 bytes");
        }
        // Checked from the header alone: an oversized frame is never buffered.
        if (length > m_limits.maxFrameBytes) {
            return fail(ws::kMessageTooBig, "frame exceeds the size limit");
        }
        if (!isControl(opcode) && m_inMessage && m_message.size() + length > m_limits.maxMessageBytes) {
            return fail(ws::kMessageTooBig, "message exceeds the size limit");
        }
        if (!isControl(opcode) && !m_inMessage && length > m_limits.maxMessageBytes) {
            return fail(ws::kMessageTooBig, "message exceeds the size limit");
        }
        if (opcode == WsOpcode::Continuation && !m_inMessage) {
            return fail(ws::kProtocolError, "continuation frame without a message");
        }
        if ((opcode == WsOpcode::Text || opcode == WsOpcode::Binary) && m_inMessage) {
            return fail(ws::kProtocolError, "new message started inside a fragmented one");
        }

        const std::size_t maskSize = masked ? 4 : 0;
        const std::size_t total = headerSize + maskSize + static_cast<std::size_t>(length);
        if (data.size() < total) {
            return std::optional<WsMessage>(); // wait for the rest of the frame
        }
        std::array<std::byte, 4> key{};
        if (masked) {
            for (std::size_t i = 0; i < 4; ++i) {
                key[i] = data[headerSize + i];
            }
        }
        std::vector<std::byte> payload(data.begin() + static_cast<std::ptrdiff_t>(headerSize + maskSize),
                                       data.begin() + static_cast<std::ptrdiff_t>(total));
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= key[i % 4];
            }
        }
        m_offset += total;

        if (isControl(opcode)) {
            WsMessage message;
            if (opcode == WsOpcode::Ping) {
                message.kind = WsMessage::Kind::Ping;
                message.payload = std::move(payload);
            } else if (opcode == WsOpcode::Pong) {
                message.kind = WsMessage::Kind::Pong;
                message.payload = std::move(payload);
            } else {
                message.kind = WsMessage::Kind::Close;
                if (payload.size() == 1) {
                    return fail(ws::kProtocolError, "close frame with a one-byte payload");
                }
                if (payload.size() >= 2) {
                    message.closeCode = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(payload[0]) << 8 |
                                                                   std::to_integer<std::uint16_t>(payload[1]));
                    if (!ws::isSendableCloseCode(message.closeCode)) {
                        return fail(ws::kProtocolError, "invalid close code");
                    }
                    message.payload.assign(payload.begin() + 2, payload.end());
                    if (!isValidUtf8(message.text())) {
                        return fail(ws::kInvalidPayload, "close reason is not valid UTF-8");
                    }
                }
            }
            return std::optional<WsMessage>(std::move(message));
        }

        // Data frame: start or continue a message.
        if (opcode != WsOpcode::Continuation) {
            m_messageOpcode = opcode;
            m_message.clear();
        }
        if (m_message.empty()) {
            m_message = std::move(payload);
        } else {
            // resize + copy rather than insert: same effect, and it sidesteps a
            // GCC 13 -O3 -Wstringop-overflow false positive on vector::insert.
            const std::size_t old = m_message.size();
            m_message.resize(old + payload.size());
            std::copy(payload.begin(), payload.end(), m_message.begin() + static_cast<std::ptrdiff_t>(old));
        }
        m_inMessage = !final;
        if (!final) {
            continue; // need more fragments
        }
        WsMessage message;
        message.kind = m_messageOpcode == WsOpcode::Text ? WsMessage::Kind::Text : WsMessage::Kind::Binary;
        message.payload = std::move(m_message);
        m_message = {};
        if (message.kind == WsMessage::Kind::Text && !isValidUtf8(message.text())) {
            return fail(ws::kInvalidPayload, "text message is not valid UTF-8");
        }
        return std::optional<WsMessage>(std::move(message));
    }
}

std::uint16_t wsCloseCodeOf(const Error &error) noexcept {
    for (const auto &[key, value] : error.context()) {
        if (key == "close-code") {
            if (const Result<std::int64_t> code = parseInt(value); code && code.value() > 0 && code.value() < 65536) {
                return static_cast<std::uint16_t>(code.value());
            }
        }
    }
    return ws::kProtocolError;
}

} // namespace cfw
