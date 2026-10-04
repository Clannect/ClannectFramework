#pragma once

// Shared by the sound decoder targets: limits tight enough that no input can
// make a target slow or large, and the checks every decoded sound must pass.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include "cfw/audio/AudioDecoder.h"

inline cfw::AudioLimits fuzzAudioLimits() {
    cfw::AudioLimits limits;
    limits.maxInputBytes = 1u << 20;
    limits.maxDecodedBytes = 1u << 20; // a quarter of a million samples
    limits.maxChannels = 8;
    limits.maxSampleRate = 384000;
    return limits;
}

inline void checkDecoded(const cfw::AudioBuffer &sound, const cfw::AudioLimits &limits) {
    if (sound.sampleRate < 1 || sound.sampleRate > limits.maxSampleRate || sound.channels < 1 ||
        sound.channels > limits.maxChannels || sound.samples.size() % std::size_t(sound.channels) != 0 ||
        sound.samples.size() * sizeof(float) > limits.maxDecodedBytes) {
        std::abort();
    }
    for (const float sample : sound.samples) {
        if (!std::isfinite(sample)) {
            std::abort();
        }
    }
}

// Decodes `data` whole with `decode`, then opens it as a stream and reads
// and seeks about in it, steered by the input's own bytes: positions must
// stay inside the stream, a read must never give more than it was asked for,
// and a seek followed by a read must end.
template <class Decode> void fuzzSound(const std::uint8_t *data, std::size_t size, cfw::AudioFormat format, Decode decode) {
    const cfw::AudioLimits limits = fuzzAudioLimits();
    const cfw::Span<const std::byte> bytes(reinterpret_cast<const std::byte *>(data), size);
    const cfw::Result<cfw::AudioBuffer> sound = decode(bytes, limits);
    if (sound) {
        checkDecoded(sound.value(), limits);
    }
    if (format != cfw::AudioFormat::Unknown && cfw::detectAudioFormat(bytes) != format) {
        return; // the stream would be another decoder's
    }
    cfw::Result<std::unique_ptr<cfw::AudioStream>> opened = cfw::AudioStream::open(bytes, limits);
    if (!opened) {
        return;
    }
    cfw::AudioStream &stream = *opened.value();
    if (stream.sampleRate() < 1 || stream.channels() < 1 || stream.channels() > limits.maxChannels) {
        std::abort();
    }
    const std::size_t channels = std::size_t(stream.channels());
    const std::uint64_t total = stream.totalFrames().value_or(1u << 20);
    std::vector<float> block(257 * channels);
    std::uint64_t budget = 20000; // frames: bounds the time one input takes
    for (std::size_t step = 0; step < 6 && budget > 0; ++step) {
        std::uint64_t pick = step;
        for (std::size_t i = 0; i < 8 && size > 0; ++i) {
            pick = pick << 8 | data[(step * 7919 + i * 31) % size];
        }
        if (step % 3 != 0 && stream.seekToFrame(pick % (total + 2)).ok() && stream.totalFrames() &&
            stream.position() > *stream.totalFrames()) {
            std::abort();
        }
        for (int reads = 0; reads < 1 + int(pick % 5) && budget > 0; ++reads) {
            const cfw::Result<std::size_t> got = stream.read(block);
            if (!got || got.value() == 0) {
                break;
            }
            if (got.value() > block.size() / channels) {
                std::abort();
            }
            for (std::size_t i = 0; i < got.value() * channels; ++i) {
                if (!std::isfinite(block[i])) {
                    std::abort();
                }
            }
            budget -= std::min<std::uint64_t>(budget, got.value());
        }
    }
}
