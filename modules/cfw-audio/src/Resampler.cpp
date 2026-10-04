#include "cfw/audio/Resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "cfw/core/Contract.h"

namespace cfw {

namespace {

// The zeroth-order modified Bessel function, by its series.
double bessel0(double x) noexcept {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-16) {
            break;
        }
    }
    return sum;
}

// A table of low-pass filters, one row per fraction of an input sample:
// row p holds the 2 * half weights of the input frames around an output
// that falls p / phases of the way from frame half - 1 to frame half. Each
// is a sinc cut off at `cutoff` (in units of the input's Nyquist frequency)
// under a Kaiser window, scaled to sum to one so a constant stays constant.
std::vector<float> filterTable(int phases, int half, double cutoff, double beta) {
    const int taps = half * 2;
    std::vector<float> table(std::size_t(phases + 1) * std::size_t(taps));
    const double scale = 1.0 / bessel0(beta);
    std::vector<double> row(static_cast<std::size_t>(taps), 0.0);
    for (int p = 0; p <= phases; ++p) {
        const double fraction = double(p) / double(phases);
        double sum = 0.0;
        for (int j = 0; j < taps; ++j) {
            const double distance = double(j - (half - 1)) - fraction;
            const double x = distance * cutoff * std::numbers::pi;
            const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
            const double u = distance / double(half);
            const double window = std::abs(u) >= 1.0 ? 0.0 : bessel0(beta * std::sqrt(1.0 - u * u)) * scale;
            row[std::size_t(j)] = sinc * window;
            sum += row[std::size_t(j)];
        }
        for (int j = 0; j < taps; ++j) {
            table[std::size_t(p) * std::size_t(taps) + std::size_t(j)] = float(row[std::size_t(j)] / sum);
        }
    }
    return table;
}

struct QualityShape {
    int half;       // taps each side, when not lowering the rate
    double rolloff; // where the cutoff sits below the Nyquist frequency
    double beta;    // the Kaiser window's shape: the stop band's depth
};

constexpr QualityShape shapeOf(ResampleQuality quality) noexcept {
    switch (quality) {
    case ResampleQuality::Fast: return {16, 0.865, 6.76};
    case ResampleQuality::Best: return {256, 0.985, 12.27};
    case ResampleQuality::Good: break;
    }
    return {64, 0.95, 10.06};
}

} // namespace

Result<AudioBuffer> resample(const AudioBuffer &input, int sampleRate, ResampleQuality quality,
                             std::size_t maxOutputBytes) {
    if (input.sampleRate <= 0 || input.channels <= 0 || sampleRate <= 0) {
        return Error(ErrorCode::InvalidArgument, "resample: rates and channel count must be positive");
    }
    if (input.sampleRate == sampleRate) {
        return input;
    }
    const double ratio = double(sampleRate) / double(input.sampleRate);
    if (ratio > 256.0 || ratio < 1.0 / 256.0) {
        return Error(ErrorCode::InvalidArgument, "resample: the rates are too far apart");
    }
    const std::uint64_t from = std::uint64_t(input.sampleRate);
    const std::uint64_t to = std::uint64_t(sampleRate);
    const std::size_t channels = std::size_t(input.channels);
    const std::uint64_t inFrames = input.frames();
    const std::uint64_t outFrames = (inFrames * to + from / 2) / from;
    if (outFrames > maxOutputBytes / sizeof(float) / channels) {
        return Error(ErrorCode::LimitExceeded, "resample: the result would pass the size limit")
            .with("frames", std::to_string(outFrames));
    }

    // Lowering the rate needs the cutoff at the new, lower Nyquist frequency,
    // and a proportionally longer filter to keep it as sharp.
    const QualityShape shape = shapeOf(quality);
    const double lower = std::min(1.0, ratio);
    const int half = int(std::ceil(double(shape.half) / lower));
    const int taps = half * 2;
    constexpr int kPhases = 1024;
    const std::vector<float> table = filterTable(kPhases, half, lower * shape.rolloff, shape.beta);

    AudioBuffer output;
    output.sampleRate = sampleRate;
    output.channels = input.channels;
    output.samples.assign(std::size_t(outFrames) * channels, 0.0f);
    std::vector<float> weights(static_cast<std::size_t>(taps), 0.0f);
    const float *in = input.samples.data();
    float *out = output.samples.data();
    for (std::uint64_t n = 0; n < outFrames; ++n) {
        // Output n sits at input time n * from / to: a whole frame and a fraction.
        const std::uint64_t scaled = n * from;
        const std::int64_t whole = std::int64_t(scaled / to);
        const double phase = double(scaled % to) / double(to) * kPhases;
        const int row = int(phase);
        const float blend = float(phase - double(row));
        const float *a = table.data() + std::size_t(row) * std::size_t(taps);
        const float *b = a + taps;
        for (int j = 0; j < taps; ++j) {
            weights[std::size_t(j)] = a[j] + (b[j] - a[j]) * blend;
        }
        // Frames before the start and after the end count as silence.
        const std::int64_t first = whole - (half - 1);
        const int begin = int(std::max<std::int64_t>(0, -first));
        const int end = int(std::min<std::int64_t>(taps, std::int64_t(inFrames) - first));
        for (std::size_t c = 0; c < channels; ++c) {
            float sum = 0.0f;
            for (int j = begin; j < end; ++j) {
                sum += weights[std::size_t(j)] * in[std::size_t(first + j) * channels + c];
            }
            out[std::size_t(n) * channels + c] = sum;
        }
    }
    return output;
}

StreamResampler::StreamResampler(int channels, int fromRate, int toRate, int maxBlockFrames)
    : m_channels(channels), m_fromRate(fromRate), m_toRate(toRate), m_maxBlock(maxBlockFrames) {
    require(channels > 0 && fromRate > 0 && toRate > 0 && maxBlockFrames > 0,
            "StreamResampler: channels, rates and block size must be positive");
    if (isPassThrough()) {
        return;
    }
    const double ratio = double(toRate) / double(fromRate);
    const double lower = std::min(1.0, ratio);
    m_half = std::min(96, int(std::ceil(12.0 / lower)));
    m_taps = m_half * 2;
    m_step = double(fromRate) / double(toRate);
    m_table = filterTable(kPhases, m_half, lower * 0.91, 6.76);
    const std::size_t maxInput = std::size_t(std::ceil(double(maxBlockFrames) * m_step)) + 2;
    m_work.assign((std::size_t(m_taps) + maxInput) * std::size_t(channels), 0.0f);
}

int StreamResampler::inputFramesFor(int outputFrames) const noexcept {
    if (outputFrames <= 0) {
        return 0;
    }
    if (isPassThrough()) {
        return outputFrames;
    }
    return int(std::floor(m_position + double(outputFrames - 1) * m_step));
}

void StreamResampler::process(const float *input, float *output, int outputFrames) noexcept {
    if (outputFrames <= 0) {
        return;
    }
    const std::size_t channels = std::size_t(m_channels);
    if (isPassThrough()) {
        std::copy_n(input, std::size_t(outputFrames) * channels, output);
        return;
    }
    debugCheck(outputFrames <= m_maxBlock, "StreamResampler::process: block larger than maxBlockFrames");
    const int consumed = inputFramesFor(outputFrames);
    float *work = m_work.data();
    const std::size_t taps = std::size_t(m_taps);
    std::copy_n(input, std::size_t(consumed) * channels, work + taps * channels);
    for (int k = 0; k < outputFrames; ++k) {
        const double position = m_position + double(k) * m_step;
        const double whole = std::floor(position);
        const double phase = (position - whole) * kPhases;
        const int row = int(phase);
        const float blend = float(phase - double(row));
        const float *a = m_table.data() + std::size_t(row) * taps;
        const float *b = a + taps;
        const float *frames = work + std::size_t(whole) * channels;
        for (std::size_t c = 0; c < channels; ++c) {
            float sum = 0.0f;
            for (std::size_t j = 0; j < taps; ++j) {
                sum += (a[j] + (b[j] - a[j]) * blend) * frames[j * channels + c];
            }
            output[std::size_t(k) * channels + c] = sum;
        }
    }
    m_position += double(outputFrames) * m_step - double(consumed);
    // The newest frames become the history the next block starts from.
    std::copy_n(work + std::size_t(consumed) * channels, taps * channels, work);
}

void StreamResampler::reset() noexcept {
    m_position = 0.0;
    std::fill(m_work.begin(), m_work.end(), 0.0f);
}

} // namespace cfw
