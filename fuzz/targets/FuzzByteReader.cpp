// ByteReader: the engine's binary wire format, read from other players.
// The first byte picks a read sequence so the fuzzer can steer field order.

#include "cfw/io/ByteReader.h"

#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    cfw::ByteReader reader(cfw::Span<const std::byte>(reinterpret_cast<const std::byte *>(data), size));
    // Reads keep going after a failure on purpose: failure must be sticky and
    // every later read must stay in bounds.
    for (int step = 0; step < 256 && reader.remaining() > 0; ++step) {
        switch (reader.u8() % 8) {
        case 0: (void)reader.u16(); break;
        case 1: (void)reader.u32(); break;
        case 2: (void)reader.u64(); break;
        case 3: (void)reader.f32(); break;
        case 4: (void)reader.f64(); break;
        case 5: (void)reader.string(1024); break;
        case 6: (void)reader.bytes(reader.u8()); break;
        default: (void)reader.i8(); break;
        }
    }
    (void)reader.finished();
    return 0;
}
