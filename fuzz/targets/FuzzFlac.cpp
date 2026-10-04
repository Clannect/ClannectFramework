// FLAC decoder: frames with hostile predictors, residuals and block sizes.
// Whatever decodes must respect the limits and hold only finite samples;
// whatever does not must fail cleanly; and the streaming form must survive
// reading and seeking anywhere.

#include "FuzzAudioChecks.h"
#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    fuzzSound(data, size, cfw::AudioFormat::Flac,
              [](cfw::Span<const std::byte> bytes, const cfw::AudioLimits &limits) { return cfw::decodeFlac(bytes, limits); });
    return 0;
}
