// The resamplers: a tone keeps its frequency, level and phase at the new
// rate; what the new rate cannot hold is filtered out, not folded back; and
// the streaming form gives one continuous signal however it is cut in blocks.

#include "cfw/audio/Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

AudioBuffer tone(int rate, int channels, double hertz, double seconds, double level = 0.5) {
    AudioBuffer buffer;
    buffer.sampleRate = rate;
    buffer.channels = channels;
    const auto frames = std::size_t(seconds * rate);
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            // Each channel a slightly different tone, to catch crossed channels.
            buffer.samples.push_back(float(level * std::sin(kTwoPi * hertz * (c + 1) * double(i) / rate)));
        }
    }
    return buffer;
}

// The largest difference from the same tone computed at the buffer's rate,
// away from the edges (where the filter sees the silence outside the clip).
double errorAgainstTone(const AudioBuffer &buffer, double hertz, double level, std::size_t margin) {
    double worst = 0.0;
    const std::size_t channels = std::size_t(buffer.channels);
    for (std::size_t i = margin; i + margin < buffer.frames(); ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            const double expected = level * std::sin(kTwoPi * hertz * double(c + 1) * double(i) / buffer.sampleRate);
            worst = std::max(worst, std::abs(double(buffer.samples[i * channels + c]) - expected));
        }
    }
    return worst;
}

double rms(const AudioBuffer &buffer, std::size_t margin) {
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = margin * std::size_t(buffer.channels); i + margin * std::size_t(buffer.channels) < buffer.samples.size();
         ++i) {
        sum += double(buffer.samples[i]) * double(buffer.samples[i]);
        ++count;
    }
    return count ? std::sqrt(sum / double(count)) : 0.0;
}

void clipsKeepTheirSound() {
    // The engine's case: a 44.1 kHz clip brought to 48 kHz once.
    const AudioBuffer source = tone(44100, 2, 1000.0, 0.25);
    const auto up = resample(source, 48000);
    check(up.ok(), "44.1 kHz to 48 kHz");
    if (up) {
        checkEqual(up.value().sampleRate, 48000, "the new rate");
        checkEqual(up.value().channels, 2, "the channels are kept");
        checkEqual(up.value().frames(), std::size_t(12000), "the length is exact: 11025 * 48000 / 44100");
        check(errorAgainstTone(up.value(), 1000.0, 0.5, 200) < 2e-5, "a 1 kHz and a 2 kHz tone, in place and in phase");
    }
    const auto best = resample(source, 48000, ResampleQuality::Best);
    check(best.ok() && errorAgainstTone(best.value(), 1000.0, 0.5, 600) < 2e-6, "the best quality is closer still");
    const auto fast = resample(source, 48000, ResampleQuality::Fast);
    check(fast.ok() && errorAgainstTone(fast.value(), 1000.0, 0.5, 100) < 2e-3, "the fast quality is close enough");

    // A high tone survives going up: 18 kHz is inside the default quality's pass band.
    const auto high = resample(tone(44100, 1, 18000.0, 0.1), 48000);
    check(high.ok() && errorAgainstTone(high.value(), 18000.0, 0.5, 300) < 1e-3, "18 kHz at 44.1 kHz comes through");

    // Going down: what fits stays, what does not is removed rather than
    // folding back as a false tone.
    const auto down = resample(tone(48000, 1, 1000.0, 0.25), 8000);
    check(down.ok() && down.value().frames() == 2000 && errorAgainstTone(down.value(), 1000.0, 0.5, 100) < 1e-4,
          "1 kHz survives 48 kHz to 8 kHz");
    const auto gone = resample(tone(48000, 1, 6000.0, 0.25), 8000);
    check(gone.ok() && rms(gone.value(), 100) < 0.5 * 1e-4, "6 kHz, above the new limit, is 80 dB down or more");

    // A constant stays exactly as loud.
    AudioBuffer flat;
    flat.sampleRate = 22050;
    flat.channels = 1;
    flat.samples.assign(4000, 0.75f);
    const auto level = resample(flat, 48000);
    check(level.ok(), "a constant");
    if (level) {
        checkNear(level.value().samples[level.value().samples.size() / 2], 0.75, 1e-6, "keeps its level");
    }

    const auto same = resample(source, 44100);
    check(same.ok() && same.value().samples == source.samples, "the same rate is returned untouched");
    check(resample(source, 0).error().code() == ErrorCode::InvalidArgument, "a zero rate is refused");
    check(resample(AudioBuffer{}, 48000).error().code() == ErrorCode::InvalidArgument, "an empty buffer's rate too");
    check(resample(source, 48000, ResampleQuality::Good, 1000).error().code() == ErrorCode::LimitExceeded,
          "the cap on the result");
    AudioBuffer nothing;
    nothing.sampleRate = 44100;
    nothing.channels = 2;
    check(resample(nothing, 48000).ok() && resample(nothing, 48000).value().frames() == 0, "no frames in, none out");
}

void streamsAreContinuous() {
    // 48 kHz to 44.1 kHz (a device at the CD rate), in blocks of awkward sizes.
    StreamResampler resampler(2, 48000, 44100, 512);
    check(!resampler.isPassThrough(), "different rates convert");
    std::vector<float> output;
    std::vector<float> input;
    std::vector<float> block(512 * 2);
    std::uint64_t consumed = 0;
    const int sizes[] = {1, 480, 37, 512, 2, 441, 300, 511};
    for (int round = 0; round < 60; ++round) {
        const int frames = sizes[round % 8];
        const int needed = resampler.inputFramesFor(frames);
        check(needed <= frames * 48000 / 44100 + 1, "never asks for more input than the ratio allows");
        input.resize(std::size_t(needed) * 2);
        for (int i = 0; i < needed; ++i) {
            const double t = double(consumed + std::uint64_t(i)) / 48000.0;
            input[std::size_t(i) * 2] = float(0.5 * std::sin(kTwoPi * 1000.0 * t));
            input[std::size_t(i) * 2 + 1] = float(0.25 * std::sin(kTwoPi * 440.0 * t));
        }
        resampler.process(input.data(), block.data(), frames);
        consumed += std::uint64_t(needed);
        output.insert(output.end(), block.begin(), block.begin() + frames * 2);
    }
    const std::size_t frames = output.size() / 2;
    checkNear(double(consumed) / double(frames), 48000.0 / 44100.0, 0.01, "input and output keep the ratio");
    // The output is the input delayed by the filter: the same tones at 44.1 kHz.
    const double delay = double(resampler.delayFrames()) / 48000.0;
    double worst = 0.0;
    for (std::size_t i = 200; i < frames; ++i) {
        const double t = double(i) / 44100.0 - delay;
        worst = std::max(worst, std::abs(double(output[i * 2]) - 0.5 * std::sin(kTwoPi * 1000.0 * t)));
        worst = std::max(worst, std::abs(double(output[i * 2 + 1]) - 0.25 * std::sin(kTwoPi * 440.0 * t)));
    }
    check(worst < 2e-3, "one continuous signal across the block boundaries");
    if (!(worst < 2e-3)) {
        std::printf("      largest difference %g\n", worst);
    }

    StreamResampler same(2, 48000, 48000);
    check(same.isPassThrough() && same.inputFramesFor(100) == 100 && same.delayFrames() == 0, "equal rates pass through");
    const float in[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    float out[4] = {};
    same.process(in, out, 2);
    check(std::equal(in, in + 4, out), "untouched");

    // Upwards, and after reset() the history is silence again.
    StreamResampler up(1, 8000, 48000, 256);
    std::vector<float> ones(64, 1.0f), result(256);
    for (int round = 0; round < 20; ++round) {
        up.process(ones.data(), result.data(), 256);
    }
    checkNear(result[200], 1.0, 1e-3, "a constant settles at its level");
    up.reset();
    std::vector<float> zeros(64, 0.0f);
    up.process(zeros.data(), result.data(), 100);
    check(std::all_of(result.begin(), result.begin() + 100, [](float v) { return v == 0.0f; }),
          "reset forgets the signal before");
}

} // namespace

int main() {
    clipsKeepTheirSound();
    streamsAreContinuous();
    return cfw::test::finish("ResamplerTest");
}
