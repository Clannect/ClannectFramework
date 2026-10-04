#pragma once

// Decoded sound: interleaved 32-bit float samples, nominally -1 to 1, with
// the sample rate and channel count they were stored with. A "frame" is one
// sample for every channel, so samples.size() == frames() * channels.
//
// Threads: plain value types. Allocates: the sample vector.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cfw {

enum class AudioFormat : std::uint8_t { Unknown, Wav, OggVorbis, Mp3, Flac };

// "WAV", "Ogg Vorbis"... Never null.
[[nodiscard]] const char *audioFormatName(AudioFormat format) noexcept;

struct AudioBuffer {
    int sampleRate = 0; // frames per second
    int channels = 0;
    std::vector<float> samples; // interleaved: frame 0's channels, then frame 1's...

    [[nodiscard]] std::size_t frames() const noexcept {
        return channels > 0 ? samples.size() / std::size_t(channels) : 0;
    }
    [[nodiscard]] double seconds() const noexcept {
        return sampleRate > 0 ? double(frames()) / double(sampleRate) : 0.0;
    }
};

// Hard limits every decoder checks before it allocates. Sound comes from
// other creators, so a header claiming a week of 32-channel audio must fail
// at once, not after trying to allocate it.
//
// The defaults suit an asset cache: a 256 MB file, and 256 MB of decoded
// samples (about 11 minutes of 48 kHz stereo). Longer music is streamed with
// AudioStream, which decodes as it is read and is not bound by
// maxDecodedBytes.
struct AudioLimits {
    std::size_t maxInputBytes = 256u * 1024u * 1024u;
    // Of the float result of a whole-file decode (decodeAudio and friends):
    // frames * channels * 4. A file that decodes to more fails with
    // LimitExceeded; nothing is truncated.
    std::size_t maxDecodedBytes = 256u * 1024u * 1024u;
    int maxChannels = 32;
    int maxSampleRate = 768000;
};

} // namespace cfw
