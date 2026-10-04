// Ogg Vorbis decoder: a stream that brings its own code books, floors,
// residues and mappings, every one of them sized by the input. Whatever
// decodes must respect the limits and hold only finite samples; whatever does
// not must fail cleanly; and the streaming form must survive reading and
// seeking anywhere.

#include "FuzzAudioChecks.h"
#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    fuzzSound(data, size, cfw::AudioFormat::OggVorbis, [](cfw::Span<const std::byte> bytes, const cfw::AudioLimits &limits) {
        return cfw::decodeOggVorbis(bytes, limits);
    });
    return 0;
}
