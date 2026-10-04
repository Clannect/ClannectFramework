#pragma once

// What every output backend does between the application's callback and
// the hardware: pull 48 kHz stereo float, bring it to the device's rate,
// spread it over the device's channels and write the device's sample format.
// Built when a device is opened; render() allocates nothing and is called on
// the device's thread.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "cfw/audio/AudioDevice.h"
#include "cfw/audio/Resampler.h"

namespace cfw::detail {

enum class DeviceSampleFormat : std::uint8_t { Float32, Int16, Int24, Int32 };

inline std::size_t bytesPerSample(DeviceSampleFormat format) noexcept {
    switch (format) {
    case DeviceSampleFormat::Int16: return 2;
    case DeviceSampleFormat::Int24: return 3;
    case DeviceSampleFormat::Float32:
    case DeviceSampleFormat::Int32: break;
    }
    return 4;
}

class DeviceRenderer {
public:
    explicit DeviceRenderer(const AudioCallback &callback) : m_callback(callback) {
        m_source.assign(std::size_t(AudioDevice::kMaxCallbackFrames) * 2, 0.0f);
    }

    // The device's own format. Allocates; not for the audio thread while it renders.
    void configure(int deviceRate, int deviceChannels, DeviceSampleFormat format) {
        m_rate = deviceRate;
        m_channels = deviceChannels;
        m_format = format;
        // Blocks small enough that one never needs more than the callback's
        // largest request.
        m_block = int(std::clamp<std::int64_t>(std::int64_t(AudioDevice::kMaxCallbackFrames - 8) * deviceRate /
                                                   AudioDevice::kSampleRate,
                                               1, 1024));
        m_resampler = std::make_unique<StreamResampler>(2, AudioDevice::kSampleRate, deviceRate, m_block);
        m_converted.assign(std::size_t(m_block) * 2, 0.0f);
    }

    [[nodiscard]] int deviceRate() const noexcept { return m_rate; }
    [[nodiscard]] int deviceChannels() const noexcept { return m_channels; }
    [[nodiscard]] std::size_t frameBytes() const noexcept { return bytesPerSample(m_format) * std::size_t(m_channels); }

    // Fills `frames` device frames at `out`.
    void render(void *out, int frames) noexcept {
        auto *to = static_cast<std::uint8_t *>(out);
        while (frames > 0) {
            const int block = std::min(frames, m_block);
            const int needed = m_resampler->inputFramesFor(block);
            if (needed > 0) {
                pull(m_source.data(), needed);
            }
            m_resampler->process(m_source.data(), m_converted.data(), block);
            write(to, block);
            to += std::size_t(block) * frameBytes();
            frames -= block;
        }
    }

    // Pulls `frames` frames of 48 kHz through the callback and drops them:
    // what keeps the application's clock running while there is no device.
    void discard(int frames) noexcept {
        while (frames > 0) {
            const int block = std::min(frames, AudioDevice::kMaxCallbackFrames);
            pull(m_source.data(), block);
            frames -= block;
        }
    }

private:
    void pull(float *stereo, int frames) noexcept {
        std::fill_n(stereo, std::size_t(frames) * 2, 0.0f);
        m_callback(stereo, frames);
        // Whatever the application wrote, the device gets numbers.
        for (int i = 0; i < frames * 2; ++i) {
            if (!std::isfinite(stereo[i])) {
                stereo[i] = 0.0f;
            }
        }
    }

    // m_converted (stereo, device rate) to the device's channels and format.
    void write(std::uint8_t *to, int frames) noexcept {
        const float *from = m_converted.data();
        const std::size_t sample = bytesPerSample(m_format);
        for (int i = 0; i < frames; ++i) {
            const float left = from[i * 2];
            const float right = from[i * 2 + 1];
            for (int c = 0; c < m_channels; ++c) {
                const float value = m_channels == 1 ? (left + right) * 0.5f : c == 0 ? left : c == 1 ? right : 0.0f;
                put(to, value);
                to += sample;
            }
        }
    }

    void put(std::uint8_t *to, float value) const noexcept {
        if (m_format == DeviceSampleFormat::Float32) {
            std::memcpy(to, &value, sizeof value);
            return;
        }
        const double clamped = std::clamp(double(value), -1.0, 1.0);
        if (m_format == DeviceSampleFormat::Int16) {
            const auto fixed = std::int16_t(std::lround(clamped * 32767.0));
            std::memcpy(to, &fixed, sizeof fixed);
        } else if (m_format == DeviceSampleFormat::Int32) {
            const auto fixed = std::int32_t(std::llround(clamped * 2147483647.0));
            std::memcpy(to, &fixed, sizeof fixed);
        } else {
            const auto fixed = std::uint32_t(std::int32_t(std::lround(clamped * 8388607.0)));
            to[0] = std::uint8_t(fixed);
            to[1] = std::uint8_t(fixed >> 8);
            to[2] = std::uint8_t(fixed >> 16);
        }
    }

    const AudioCallback &m_callback;
    int m_rate = AudioDevice::kSampleRate;
    int m_channels = 2;
    DeviceSampleFormat m_format = DeviceSampleFormat::Float32;
    int m_block = 1024;
    std::unique_ptr<StreamResampler> m_resampler;
    std::vector<float> m_source;    // from the callback
    std::vector<float> m_converted; // at the device's rate, still stereo
};

} // namespace cfw::detail
