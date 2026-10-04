#pragma once

// The system's default sound output, pulled: the device calls the
// application whenever it needs more audio.
//
//     auto device = AudioDevice::open([&](float *stereo, int frames) { mixer.mix(stereo, frames); });
//     if (device) device.value()->start();   // no device: carry on silent
//
// The callback always fills 48 kHz stereo 32-bit float, interleaved (left,
// right). Whatever the hardware really wants - another rate, more or fewer
// channels, integer samples - is converted here, on the device's thread. A
// mono device gets the average of left and right; a device with more than
// two channels gets left and right in its first two and silence in the rest.
//
// Backends:
//   Windows  WASAPI, shared mode, event driven.
//   Linux    ALSA's "default" PCM, with libasound loaded at run time: where
//            PipeWire or PulseAudio own the sound card, that is their ALSA
//            plugin, so the output goes through the sound server. Without
//            libasound, open() fails with Unsupported. The environment
//            variable CFW_ALSA_DEVICE names another PCM ("hw:1", "null").
//   macOS    CoreAudio's default output unit (which leaves the callback
//            unpulled while the system has no output device).
//   Null     no hardware: for tests and headless runs (openNull()).
//
// The default device is followed. When the user picks another output, or
// headphones go in or out, the stream moves to the new default with no call
// from the application; a few milliseconds of sound may be lost. While there
// is no output device at all, the callback is still pulled at the right
// rate, so mixers, music positions and anything else timed by it keep
// moving, and sound comes back when a device does.
//
// The callback runs on the device's own thread, which is real-time on every
// backend: it must not block, lock a contended mutex, allocate, or call
// start(), stop() or the destructor. It is never called before start() is,
// and never after stop() or the destructor returns.
//
// Threads: open(), start(), stop() and the destructor from any thread except
// the callback's (stop() from the callback is a contract violation), one at
// a time. latency() and isRunning() from any thread, the callback's too.
// Allocates: when opened and when the device changes; nothing per callback.

#include <cstdint>
#include <functional>
#include <memory>

#include "cfw/core/Clock.h"
#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw {

// Fills `frames` frames (2 * frames floats) of 48 kHz stereo. `frames` is
// between 1 and AudioDevice::kMaxCallbackFrames and varies from call to call.
// The buffer arrives holding silence, so a callback with nothing to play may
// simply return; samples that are not finite numbers are played as silence.
using AudioCallback = std::function<void(float *interleavedStereo, int frames)>;

struct AudioDeviceOptions {
    // The latency to aim for: the time from the callback producing a sample
    // to it reaching the device. Between 20 and 40 ms is a good range for
    // games; lower risks drop-outs when the system is busy. The system has
    // the last word: read latency() for what it gave.
    Duration targetLatency = std::chrono::milliseconds(30);
};

class NullAudioDevice;

class AudioDevice {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kMaxCallbackFrames = 4096;

    // Opens the system's default output, stopped. Fails with NotFound when
    // the system has no output device, Unsupported when the platform has no
    // backend (or libasound is not installed), and IoError when the device
    // refuses to open; the message says which. `callback` must not be empty
    // (a contract).
    [[nodiscard]] static Result<std::unique_ptr<AudioDevice>> open(AudioCallback callback,
                                                                   const AudioDeviceOptions &options = {});
    // A device with no hardware behind it. Paced (the default), it pulls the
    // callback from its own thread at 48 kHz in 10 ms blocks, as a real
    // device would, and throws the sound away. Unpaced, it has no thread:
    // the callback is pulled only by NullAudioDevice::render(), so a test
    // decides exactly what is asked for and when.
    [[nodiscard]] static std::unique_ptr<NullAudioDevice> openNull(AudioCallback callback, bool paced = true);

    // Stops (as stop() does) and closes the device.
    virtual ~AudioDevice();
    AudioDevice(const AudioDevice &) = delete;
    AudioDevice &operator=(const AudioDevice &) = delete;

    // Starts pulling the callback. Does nothing if already running.
    [[nodiscard]] virtual Result<void> start() = 0;
    // Stops pulling it. When stop() returns the callback is not running and
    // will not be called again until start(). Does nothing if stopped.
    virtual void stop() = 0;
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;

    // The output latency the system actually gave: its buffer plus what the
    // device reports for itself. It may change when the device does.
    [[nodiscard]] virtual Duration latency() const noexcept = 0;
    // What the output is called ("Speakers (Realtek Audio)"), for logs and
    // settings screens; empty when the system does not say.
    [[nodiscard]] virtual String deviceName() const = 0;
    // The device's own rate and channel count, before conversion.
    [[nodiscard]] virtual int deviceSampleRate() const noexcept = 0;
    [[nodiscard]] virtual int deviceChannels() const noexcept = 0;
    // How many times the stream moved to another device since it was opened.
    [[nodiscard]] virtual std::uint64_t deviceChanges() const noexcept { return 0; }

protected:
    AudioDevice() = default;
};

class NullAudioDevice final : public AudioDevice {
public:
    ~NullAudioDevice() override;

    Result<void> start() override;
    void stop() override;
    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] Duration latency() const noexcept override { return std::chrono::milliseconds(10); }
    [[nodiscard]] String deviceName() const override { return "Null output"; }
    [[nodiscard]] int deviceSampleRate() const noexcept override { return kSampleRate; }
    [[nodiscard]] int deviceChannels() const noexcept override { return kChannels; }

    // Unpaced devices only (a contract), on the calling thread: pulls
    // `frames` frames through the callback now, in blocks of at most
    // kMaxCallbackFrames, whether or not the device is started.
    void render(int frames);
    // Frames pulled so far, by the thread or by render().
    [[nodiscard]] std::uint64_t framesRendered() const noexcept;

private:
    friend class AudioDevice;
    struct State;
    explicit NullAudioDevice(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
};

} // namespace cfw
