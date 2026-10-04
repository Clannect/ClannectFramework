// The one entry point that detects a sound file's format from its bytes: any
// input at all, including one that looks like one format and is another,
// must decode within the limits or fail cleanly. What decodes is then brought
// to the mixer's rate, as the engine does when it loads a clip.

#include "cfw/audio/Resampler.h"

#include "FuzzAudioChecks.h"
#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    fuzzSound(data, size, cfw::AudioFormat::Unknown, [](cfw::Span<const std::byte> bytes, const cfw::AudioLimits &limits) {
        cfw::Result<cfw::AudioBuffer> sound = cfw::decodeAudio(bytes, limits);
        if (sound && sound.value().frames() <= 20000) {
            const cfw::Result<cfw::AudioBuffer> converted =
                cfw::resample(sound.value(), 48000, cfw::ResampleQuality::Fast, limits.maxDecodedBytes);
            if (converted) {
                cfw::AudioLimits atMixerRate = limits;
                atMixerRate.maxSampleRate = 48000;
                checkDecoded(converted.value(), atMixerRate);
            }
        }
        return sound;
    });
    return 0;
}
