// The output device, without hardware: a sine played through the null
// device, checking exactly what the callback was asked for; the paced form's
// rate and its start, stop and destruction; the conversion every real
// backend runs between the callback and the hardware; and, where the machine
// has an output, the real device opened, started and stopped (playing silence).

#include "cfw/audio/AudioDevice.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

#include "../src/DeviceRender.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// A 1 kHz sine on the left and 500 Hz on the right, continuous across calls,
// that records what it was asked for.
struct SineSource {
    std::uint64_t frames = 0;
    std::vector<int> requests;
    bool sawBadRequest = false;
    void operator()(float *stereo, int count) {
        requests.push_back(count);
        sawBadRequest = sawBadRequest || count < 1 || count > AudioDevice::kMaxCallbackFrames || stereo == nullptr;
        for (int i = 0; i < count; ++i) {
            const double t = double(frames + std::uint64_t(i)) / AudioDevice::kSampleRate;
            stereo[i * 2] = float(0.5 * std::sin(kTwoPi * 1000.0 * t));
            stereo[i * 2 + 1] = float(0.25 * std::sin(kTwoPi * 500.0 * t));
        }
        frames += std::uint64_t(count);
    }
};

void pulledByHand() {
    SineSource source;
    std::vector<float> heard;
    auto device = AudioDevice::openNull(
        [&](float *stereo, int frames) {
            source(stereo, frames);
            heard.insert(heard.end(), stereo, stereo + frames * 2);
        },
        false);
    check(!device->isRunning(), "a device opens stopped");
    checkEqual(device->deviceSampleRate(), 48000, "the null device is 48 kHz");
    checkEqual(device->deviceChannels(), 2, "stereo");
    checkEqual(device->deviceName(), String("Null output"), "and says what it is");
    device->render(480);
    check(source.requests == std::vector<int>{480}, "one request for exactly the frames asked for");
    device->render(10000);
    check(source.requests == std::vector<int>({480, 4096, 4096, 1808}),
          "a large pull arrives in requests of at most kMaxCallbackFrames");
    check(!source.sawBadRequest, "every request has a buffer and a frame count in range");
    checkEqual(device->framesRendered(), std::uint64_t(10480), "the device counts what it pulled");
    checkEqual(heard.size(), std::size_t(10480 * 2), "two floats a frame");
    // What came out is the sine, continuous across the requests.
    double worst = 0.0;
    for (std::size_t i = 0; i < heard.size() / 2; ++i) {
        worst = std::max(worst, std::abs(double(heard[i * 2]) - 0.5 * std::sin(kTwoPi * 1000.0 * double(i) / 48000.0)));
    }
    check(worst < 1e-6, "the sine is continuous across requests");
    check(device->start().ok() && device->isRunning(), "start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    checkEqual(device->framesRendered(), std::uint64_t(10480), "an unpaced device pulls nothing by itself");
    device->stop();
    check(!device->isRunning(), "stop");
}

void pacedLikeASoundCard() {
    std::mutex mutex;
    SineSource source;
    std::atomic<int> calls{0};
    std::thread::id callbackThread;
    auto device = AudioDevice::openNull([&](float *stereo, int frames) {
        const std::lock_guard lock(mutex);
        source(stereo, frames);
        callbackThread = std::this_thread::get_id();
        ++calls;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    checkEqual(calls.load(), 0, "nothing is pulled before start()");
    const auto begin = std::chrono::steady_clock::now();
    check(device->start().ok(), "start");
    check(device->start().ok(), "starting twice is fine");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    device->stop();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const int after = calls.load();
    {
        const std::lock_guard lock(mutex);
        check(callbackThread != std::this_thread::get_id(), "the callback runs on the device's own thread");
        check(std::all_of(source.requests.begin(), source.requests.end(), [](int n) { return n == 480; }),
              "in 10 ms blocks");
        // Loose bounds: CI machines stall, and Wine's timers are coarse. A
        // device running at the wrong rate would be far outside them.
        const double rate = double(source.frames) / seconds;
        check(rate > 48000 * 0.5 && rate < 48000 * 1.1, "at about 48 kHz");
        if (!(rate > 48000 * 0.5 && rate < 48000 * 1.1)) {
            std::printf("      %llu frames in %.3f s\n", static_cast<unsigned long long>(source.frames), seconds);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    checkEqual(calls.load(), after, "after stop() the callback is not called again");
    device->stop(); // stopping twice is fine

    check(device->start().ok(), "and it starts again");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // Stopped from another thread, then destroyed while that happens.
    std::thread other([&] { device->stop(); });
    other.join();
    check(calls.load() > after, "the second run pulled more");
    const int beforeDestroy = calls.load();
    check(device->start().ok(), "a third start");
    device.reset(); // destroying a running device stops it
    const int atDestroy = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    check(calls.load() == atDestroy && atDestroy >= beforeDestroy, "nothing runs after the device is destroyed");
}

// What sits between the callback and a real device.
void convertsForTheHardware() {
    SineSource source;
    const AudioCallback callback = [&](float *stereo, int frames) { source(stereo, frames); };

    // A 44.1 kHz, 16-bit mono device.
    {
        detail::DeviceRenderer renderer(callback);
        renderer.configure(44100, 1, detail::DeviceSampleFormat::Int16);
        checkEqual(renderer.frameBytes(), std::size_t(2), "one 16-bit sample a frame");
        std::vector<std::int16_t> out(44100);
        // In the odd-sized pieces a device asks for.
        std::size_t done = 0;
        for (const int piece : {441, 1, 3000, 1024, 39634}) {
            renderer.render(out.data() + done, piece);
            done += std::size_t(piece);
        }
        checkEqual(done, out.size(), "a second of device frames");
        check(!source.sawBadRequest, "the callback's requests stay in range");
        checkNear(double(source.frames), 48000.0, 60.0, "a second at 44.1 kHz takes a second of 48 kHz from the callback");
        // Mono is the average of left and right: both tones, each at half its level.
        double worst = 0.0;
        const double delay = double(StreamResampler(2, 48000, 44100).delayFrames()) / 48000.0; // the converter's filter
        for (std::size_t i = 2000; i < out.size(); ++i) {
            const double t = double(i) / 44100.0 - delay;
            const double expected = 0.25 * std::sin(kTwoPi * 1000.0 * t) + 0.125 * std::sin(kTwoPi * 500.0 * t);
            worst = std::max(worst, std::abs(double(out[i]) / 32767.0 - expected));
        }
        check(worst < 3e-3, "resampled to the device's rate, mixed to mono, written as 16-bit");
        if (!(worst < 3e-3)) {
            std::printf("      largest difference %g\n", worst);
        }
    }
    // A 48 kHz six-channel float device: no resampling, left and right in
    // the first two channels, silence in the rest.
    {
        source = {};
        detail::DeviceRenderer renderer(callback);
        renderer.configure(48000, 6, detail::DeviceSampleFormat::Float32);
        std::vector<float> out(480 * 6);
        renderer.render(out.data(), 480);
        checkEqual(source.frames, std::uint64_t(480), "at the device's own rate, frame for frame");
        bool ok = true;
        for (int i = 0; i < 480; ++i) {
            const double t = double(i) / 48000.0;
            ok = ok && std::abs(double(out[std::size_t(i) * 6]) - 0.5 * std::sin(kTwoPi * 1000.0 * t)) < 1e-6;
            ok = ok && std::abs(double(out[std::size_t(i) * 6 + 1]) - 0.25 * std::sin(kTwoPi * 500.0 * t)) < 1e-6;
            for (int c = 2; c < 6; ++c) {
                ok = ok && out[std::size_t(i) * 6 + std::size_t(c)] == 0.0f;
            }
        }
        check(ok, "left and right lead a surround device; the other channels are silent");
    }
    // Integer formats clamp, and a callback's NaN never reaches the device.
    {
        const AudioCallback loud = [](float *stereo, int frames) {
            for (int i = 0; i < frames; ++i) {
                stereo[i * 2] = i % 2 ? 3.0f : -3.0f;
                stereo[i * 2 + 1] = NAN;
            }
        };
        detail::DeviceRenderer renderer(loud);
        renderer.configure(48000, 2, detail::DeviceSampleFormat::Int32);
        std::vector<std::int32_t> out(8);
        renderer.render(out.data(), 4);
        check(out[0] == -2147483647 && out[2] == 2147483647, "out-of-range samples clamp to full scale");
        check(out[1] == 0 && out[3] == 0, "NaN from the callback becomes silence");
        renderer.configure(48000, 2, detail::DeviceSampleFormat::Int24);
        std::vector<std::uint8_t> packed(4 * 6);
        renderer.render(packed.data(), 4);
        check(packed[0] == 0x01 && packed[1] == 0x00 && packed[2] == 0x80, "24-bit samples are three bytes, low first");
        // With no device the callback is still pulled, and the sound dropped.
        std::atomic<int> pulled{0};
        const AudioCallback counting = [&](float *, int frames) { pulled += frames; };
        detail::DeviceRenderer idle(counting);
        idle.configure(48000, 2, detail::DeviceSampleFormat::Float32);
        idle.discard(10000);
        checkEqual(pulled.load(), 10000, "discard pulls the callback without a device");
    }
}

// The machine's own output, if it has one. Silence is played.
void theRealDevice() {
    std::atomic<std::uint64_t> frames{0};
    std::atomic<bool> badRequest{false};
    auto opened = AudioDevice::open([&](float *stereo, int count) {
        if (count < 1 || count > AudioDevice::kMaxCallbackFrames || stereo == nullptr) {
            badRequest = true;
            return;
        }
        std::memset(stereo, 0, std::size_t(count) * 2 * sizeof(float));
        frames += std::uint64_t(count);
    });
    if (!opened) {
        const ErrorCode code = opened.error().code();
        std::printf("AudioDeviceTest: no real output device (%s)\n", opened.error().describe().c_str());
        check(code == ErrorCode::NotFound || code == ErrorCode::Unsupported || code == ErrorCode::IoError,
              "no device is reported as NotFound, Unsupported or IoError");
        check(!opened.error().message().empty(), "with a message saying why");
        return;
    }
    AudioDevice &device = *opened.value();
    std::printf("AudioDeviceTest: \"%s\", %d Hz, %d channels, latency %.1f ms\n", device.deviceName().c_str(),
                device.deviceSampleRate(), device.deviceChannels(), toMilliseconds(device.latency()));
    check(!device.isRunning(), "the real device opens stopped");
    check(device.deviceSampleRate() > 0 && device.deviceChannels() > 0, "it reports its own format");
    check(device.latency() > Duration::zero() && device.latency() < std::chrono::seconds(2), "and a latency");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    checkEqual(frames.load(), std::uint64_t(0), "nothing is pulled before start()");
    check(device.start().ok() && device.isRunning(), "start");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (frames.load() < 4800 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(frames.load() >= 4800, "the device pulls the callback");
    device.stop();
    check(!device.isRunning(), "stop");
    const std::uint64_t atStop = frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    checkEqual(frames.load(), atStop, "after stop() the callback is not called again");
    check(device.start().ok(), "it starts again");
    const auto again = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (frames.load() < atStop + 4800 && std::chrono::steady_clock::now() < again) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(frames.load() >= atStop + 4800, "and pulls again");
    check(!badRequest.load(), "every request was in range");
    opened.value().reset(); // destroyed while running
    const std::uint64_t atDestroy = frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    checkEqual(frames.load(), atDestroy, "nothing runs after the device is destroyed");
}

} // namespace

int main() {
    pulledByHand();
    pacedLikeASoundCard();
    convertsForTheHardware();
    theRealDevice();
    return cfw::test::finish("AudioDeviceTest");
}
