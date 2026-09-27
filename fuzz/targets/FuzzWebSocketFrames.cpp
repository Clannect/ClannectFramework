// WebSocket frame decoder, both roles: frames arrive from any client (server
// role) or from any server (client role). Input is fed in pieces to exercise
// reassembly across reads.

#include "cfw/net/WebSocketFrame.h"

#include "FuzzTarget.h"

namespace {

void decode(cfw::WsRole role, cfw::Span<const std::byte> input) {
    cfw::WsLimits limits;
    limits.maxFrameBytes = 64 * 1024;
    limits.maxMessageBytes = 256 * 1024;
    cfw::WsDecoder decoder(role, limits);
    std::size_t offset = 0;
    std::size_t piece = 1;
    while (offset < input.size() && !decoder.failed()) {
        const std::size_t n = std::min(piece, input.size() - offset);
        decoder.feed(input.subspan(offset, n));
        offset += n;
        piece = piece * 3 + 1; // 1, 4, 13, 40, ...: splits headers and payloads unevenly
        for (;;) {
            cfw::Result<std::optional<cfw::WsMessage>> message = decoder.next();
            if (!message || !message.value()) {
                break;
            }
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::Span<const std::byte> input(reinterpret_cast<const std::byte *>(data), size);
    decode(cfw::WsRole::Server, input);
    decode(cfw::WsRole::Client, input);
    return 0;
}
