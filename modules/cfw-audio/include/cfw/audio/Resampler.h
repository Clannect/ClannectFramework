#pragma once

// Sample rate conversion, in two forms.
//
// resample() converts a whole clip once, at load time, with a long
// Kaiser-windowed sinc filter: the stop band is below -100 dB at the default
// quality, so a 44.1 kHz clip brought to 48 kHz is indistinguishable from the
// original. It is exact about length (round(frames * to / from)) and has no
// delay: output sample n is the input at time n / to.
//
// StreamResampler converts a continuous signal a block at a time, for an
// output device whose rate is not the mixer's or for streamed music. It uses
// a shorter filter (24 taps, about -70 dB) and allocates nothing after construction,
// so it may run on the audio thread.
//
// Threads: resample() is pure. A StreamResampler belongs to one thread at a
// time. Allocates: resample() the result and its filter table;
// StreamResampler in its constructor only.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cfw/audio/AudioBuffer.h"
#include "cfw/core/Result.h"

namespace cfw {

enum class ResampleQuality : std::uint8_t {
    Fast, // 32 taps: previews; flat to about 0.8 of the lower rate's half
    Good, // 128 taps: the default; flat to 0.9, stop band under -100 dB
    Best, // 512 taps: flat to 0.97, under -120 dB, about 4x the time
};

// `input` at `sampleRate`. A buffer already at that rate is returned as it
// is. Fails with InvalidArgument for a rate or channel count that is not
// positive or a ratio beyond 256 to 1 either way, and LimitExceeded when the
// result would not fit `maxOutputBytes`.
[[nodiscard]] Result<AudioBuffer> resample(const AudioBuffer &input, int sampleRate,
                                           ResampleQuality quality = ResampleQuality::Good,
                                           std::size_t maxOutputBytes = 1024u * 1024u * 1024u);

class StreamResampler {
public:
    // Converts `channels` interleaved channels from `fromRate` to `toRate`,
    // in blocks of at most `maxBlockFrames` output frames. All four must be
    // positive (a contract).
    StreamResampler(int channels, int fromRate, int toRate, int maxBlockFrames = 4096);

    [[nodiscard]] int channels() const noexcept { return m_channels; }
    [[nodiscard]] bool isPassThrough() const noexcept { return m_fromRate == m_toRate; }
    // The filter's delay, in input frames (0 when passing through): output
    // frame n is the input at time n * from / to - delayFrames().
    [[nodiscard]] int delayFrames() const noexcept { return isPassThrough() ? 0 : m_half + 1; }

    // How many input frames the next process() call needs to produce
    // `outputFrames` (never more than outputFrames * from / to + 1).
    [[nodiscard]] int inputFramesFor(int outputFrames) const noexcept;
    // Consumes exactly inputFramesFor(outputFrames) frames from `input` and
    // writes `outputFrames` frames (at most maxBlockFrames) to `output`.
    void process(const float *input, float *output, int outputFrames) noexcept;
    // Forgets the signal so far (after a seek or a device change).
    void reset() noexcept;

private:
    static constexpr int kPhases = 256;

    int m_channels;
    int m_fromRate;
    int m_toRate;
    int m_maxBlock;
    int m_half = 0;           // taps each side of the centre
    int m_taps = 0;
    double m_step = 1.0;      // input frames per output frame
    double m_position = 0.0;  // of the next output frame, in input frames from the history's start
    std::vector<float> m_table; // (kPhases + 1) rows of m_taps coefficients
    std::vector<float> m_work;  // the last m_taps input frames, then the block's input
};

} // namespace cfw
