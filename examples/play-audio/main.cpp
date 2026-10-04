// Plays a sound file through the system's default output.
//
//     cfw-play-audio <file> [--stream] [--null]
//
// By default the file is decoded whole and brought to the device's 48 kHz
// once, as a game does with a sound effect. With --stream it is decoded a
// piece at a time while it plays, as a game does with music: a StreamResampler
// converts the rate and the callback only copies from a queue. --null plays
// into the null device (no sound), which is how the program runs unattended.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "cfw/audio/AudioDecoder.h"
#include "cfw/audio/AudioDevice.h"
#include "cfw/audio/Resampler.h"
#include "cfw/io/MappedFile.h"

using namespace cfw;

namespace {

// Any channel count to stereo: mono to both sides, more than two by taking
// the first two (front left and right in every common layout).
void toStereo(const float *in, int channels, std::size_t frames, float *out) {
    for (std::size_t i = 0; i < frames; ++i) {
        out[i * 2] = in[i * std::size_t(channels)];
        out[i * 2 + 1] = in[i * std::size_t(channels) + (channels > 1 ? 1 : 0)];
    }
}

// 48 kHz stereo frames between the decoding thread and the audio callback.
// The callback takes the lock only with try_lock, so it never waits on the
// decoder: if the lock is busy it plays a block of silence instead.
class Queue {
public:
    explicit Queue(std::size_t frames) : m_samples(frames * 2) {}

    std::size_t space() {
        const std::lock_guard lock(m_mutex);
        return m_samples.size() / 2 - m_count;
    }
    void push(const float *stereo, std::size_t frames) {
        const std::lock_guard lock(m_mutex);
        for (std::size_t i = 0; i < frames * 2; ++i) {
            m_samples[(m_write + i) % m_samples.size()] = stereo[i];
        }
        m_write = (m_write + frames * 2) % m_samples.size();
        m_count += frames;
    }
    std::size_t pop(float *stereo, std::size_t frames) {
        const std::unique_lock lock(m_mutex, std::try_to_lock);
        if (!lock) {
            return 0;
        }
        const std::size_t take = std::min(frames, m_count);
        for (std::size_t i = 0; i < take * 2; ++i) {
            stereo[i] = m_samples[(m_read + i) % m_samples.size()];
        }
        m_read = (m_read + take * 2) % m_samples.size();
        m_count -= take;
        return take;
    }

private:
    std::mutex m_mutex;
    std::vector<float> m_samples;
    std::size_t m_read = 0, m_write = 0, m_count = 0;
};

std::unique_ptr<AudioDevice> openDevice(AudioCallback callback, bool useNull) {
    if (!useNull) {
        auto device = AudioDevice::open(callback);
        if (device) {
            return std::move(device).value();
        }
        std::printf("no sound output (%s); playing into the null device\n", device.error().describe().c_str());
    }
    return AudioDevice::openNull(std::move(callback));
}

void describe(const AudioDevice &device) {
    std::printf("output: %s, %d Hz, %d channels, latency %.1f ms\n", device.deviceName().c_str(),
                device.deviceSampleRate(), device.deviceChannels(), toMilliseconds(device.latency()));
}

int playWhole(Span<const std::byte> bytes, bool useNull) {
    auto decoded = decodeAudio(bytes);
    if (!decoded) {
        std::fprintf(stderr, "cannot decode: %s\n", decoded.error().describe().c_str());
        return 1;
    }
    auto atDeviceRate = resample(decoded.value(), AudioDevice::kSampleRate);
    if (!atDeviceRate) {
        std::fprintf(stderr, "cannot resample: %s\n", atDeviceRate.error().describe().c_str());
        return 1;
    }
    const AudioBuffer &clip = atDeviceRate.value();
    std::vector<float> stereo(clip.frames() * 2);
    toStereo(clip.samples.data(), clip.channels, clip.frames(), stereo.data());

    std::atomic<std::size_t> played{0};
    const std::size_t total = clip.frames();
    auto device = openDevice(
        [&](float *out, int frames) {
            const std::size_t at = played.load(std::memory_order_relaxed);
            const std::size_t take = std::min(std::size_t(frames), total - at);
            std::memcpy(out, stereo.data() + at * 2, take * 2 * sizeof(float));
            std::memset(out + take * 2, 0, (std::size_t(frames) - take) * 2 * sizeof(float));
            played.store(at + take, std::memory_order_relaxed);
        },
        useNull);
    describe(*device);
    if (Result<void> started = device->start(); !started) {
        std::fprintf(stderr, "cannot start: %s\n", started.error().describe().c_str());
        return 1;
    }
    while (played.load() < total) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::printf("\r%6.2f / %.2f s", double(played.load()) / AudioDevice::kSampleRate, clip.seconds());
        std::fflush(stdout);
    }
    std::this_thread::sleep_for(device->latency()); // let the last buffer out
    device->stop();
    std::printf("\ndone: %zu frames played\n", played.load());
    return 0;
}

int playStreamed(Span<const std::byte> bytes, bool useNull) {
    auto opened = AudioStream::open(bytes);
    if (!opened) {
        std::fprintf(stderr, "cannot open: %s\n", opened.error().describe().c_str());
        return 1;
    }
    AudioStream &stream = *opened.value();
    constexpr int kBlock = 1024; // output frames a round
    StreamResampler resampler(2, stream.sampleRate(), AudioDevice::kSampleRate, kBlock);
    Queue queue(AudioDevice::kSampleRate / 2);
    std::atomic<std::uint64_t> played{0};
    auto device = openDevice(
        [&](float *out, int frames) {
            const std::size_t got = queue.pop(out, std::size_t(frames));
            std::memset(out + got * 2, 0, (std::size_t(frames) - got) * 2 * sizeof(float));
            played += got;
        },
        useNull);
    describe(*device);
    if (Result<void> started = device->start(); !started) {
        std::fprintf(stderr, "cannot start: %s\n", started.error().describe().c_str());
        return 1;
    }
    const std::size_t channels = std::size_t(stream.channels());
    std::vector<float> decoded, source, converted(std::size_t(kBlock) * 2);
    std::uint64_t queued = 0;
    for (bool ended = false; !ended;) {
        if (queue.space() < std::size_t(kBlock)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const std::size_t needed = std::size_t(resampler.inputFramesFor(kBlock));
        decoded.assign(needed * channels, 0.0f);
        source.assign(needed * 2, 0.0f);
        std::size_t got = 0;
        while (got < needed) {
            auto read = stream.read(Span<float>(decoded.data() + got * channels, (needed - got) * channels));
            if (!read || read.value() == 0) {
                ended = true; // the rest of this block is silence
                break;
            }
            got += read.value();
        }
        toStereo(decoded.data(), stream.channels(), needed, source.data());
        resampler.process(source.data(), converted.data(), kBlock);
        queue.push(converted.data(), std::size_t(kBlock));
        queued += kBlock;
        std::printf("\r%6.2f s", double(stream.position()) / stream.sampleRate());
        std::fflush(stdout);
    }
    while (played.load() < queued) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(device->latency());
    device->stop();
    std::printf("\ndone: %llu frames played\n", static_cast<unsigned long long>(played.load()));
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    const char *file = nullptr;
    bool streamed = false, useNull = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stream") == 0) {
            streamed = true;
        } else if (std::strcmp(argv[i], "--null") == 0) {
            useNull = true;
        } else {
            file = argv[i];
        }
    }
    if (!file) {
        std::fprintf(stderr, "usage: cfw-play-audio <file.wav|.ogg|.mp3|.flac> [--stream] [--null]\n");
        return 2;
    }
    auto mapped = MappedFile::open(Path(file));
    if (!mapped) {
        std::fprintf(stderr, "cannot read %s: %s\n", file, mapped.error().describe().c_str());
        return 1;
    }
    const Span<const std::byte> bytes = mapped.value().bytes();
    const AudioFormat format = detectAudioFormat(bytes);
    std::printf("%s: %s\n", file, audioFormatName(format));
    if (auto stream = AudioStream::open(bytes)) {
        std::printf("%d Hz, %d channels", stream.value()->sampleRate(), stream.value()->channels());
        if (const auto seconds = stream.value()->totalSeconds()) {
            std::printf(", %.2f s", *seconds);
        }
        std::printf("\n");
    }
    return streamed ? playStreamed(bytes, useNull) : playWhole(bytes, useNull);
}
