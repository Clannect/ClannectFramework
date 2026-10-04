// macOS: CoreAudio's default output unit. The unit is given 48 kHz stereo
// float and converts to the hardware's format itself, and it follows the
// system's default output device by itself, so there is no conversion and no
// device watching to do here; DeviceRenderer is not used.
//
// UNVERIFIED: written without a Mac, like the rest of the macOS backend
// (docs/decisions/0016). It follows the documented AudioToolbox and CoreAudio
// C APIs, but no compiler has checked it until the macos workflow runs.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "cfw/audio/AudioDevice.h"
#include "cfw/core/Contract.h"

namespace cfw {

namespace {

AudioObjectID defaultOutput() noexcept {
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    const AudioObjectPropertyAddress address = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr) {
        return kAudioObjectUnknown;
    }
    return device;
}

UInt32 deviceNumber(AudioObjectID device, AudioObjectPropertySelector selector) noexcept {
    UInt32 value = 0;
    UInt32 size = sizeof value;
    const AudioObjectPropertyAddress address = {selector, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr) {
        return 0;
    }
    return value;
}

class CoreAudioDevice final : public AudioDevice {
public:
    explicit CoreAudioDevice(AudioCallback callback) : m_callback(std::move(callback)) {}

    ~CoreAudioDevice() override {
        stop();
        if (m_unit) {
            const AudioObjectPropertyAddress address = {kAudioHardwarePropertyDefaultOutputDevice,
                                                        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &address, &CoreAudioDevice::defaultChanged, this);
            AudioUnitUninitialize(m_unit);
            AudioComponentInstanceDispose(m_unit);
        }
    }

    Result<void> initialise(const AudioDeviceOptions &options) {
        const AudioObjectID device = defaultOutput();
        if (device == kAudioObjectUnknown) {
            return Error(ErrorCode::NotFound, "no default output device");
        }
        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_DefaultOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (!component || AudioComponentInstanceNew(component, &m_unit) != noErr || !m_unit) {
            m_unit = nullptr;
            return Error(ErrorCode::Unsupported, "the system has no default output unit");
        }
        AudioStreamBasicDescription format{};
        format.mSampleRate = kSampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
        format.mBitsPerChannel = 32;
        format.mChannelsPerFrame = kChannels;
        format.mBytesPerFrame = 4 * kChannels;
        format.mFramesPerPacket = 1;
        format.mBytesPerPacket = format.mBytesPerFrame;
        AURenderCallbackStruct render{};
        render.inputProc = &CoreAudioDevice::render;
        render.inputProcRefCon = this;
        OSStatus status = AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                               sizeof format);
        if (status == noErr) {
            status = AudioUnitSetProperty(m_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &render,
                                          sizeof render);
        }
        // The hardware's buffer: about half the latency asked for.
        const double seconds = std::chrono::duration<double>(options.targetLatency).count();
        const double hardwareRate = deviceRate(device);
        if (status == noErr && hardwareRate > 0) {
            UInt32 frames = UInt32(std::clamp(seconds * 0.5 * hardwareRate, 64.0, 4096.0));
            const AudioObjectPropertyAddress address = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeOutput,
                                                        kAudioObjectPropertyElementMain};
            AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof frames, &frames); // a request; may be refused
        }
        if (status == noErr) {
            status = AudioUnitInitialize(m_unit);
        }
        if (status != noErr) {
            AudioComponentInstanceDispose(m_unit);
            m_unit = nullptr;
            return Error(ErrorCode::IoError, "the output device could not be opened")
                .with("osstatus", std::to_string(int(status)));
        }
        const AudioObjectPropertyAddress address = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain};
        AudioObjectAddPropertyListener(kAudioObjectSystemObject, &address, &CoreAudioDevice::defaultChanged, this);
        measure();
        return success();
    }

    Result<void> start() override {
        const std::lock_guard lock(m_control);
        if (m_running.load()) {
            return success();
        }
        const OSStatus status = AudioOutputUnitStart(m_unit);
        if (status != noErr) {
            return Error(ErrorCode::IoError, "the output device could not be started")
                .with("osstatus", std::to_string(int(status)));
        }
        m_running.store(true);
        return success();
    }

    // AudioOutputUnitStop returns once the render callback is no longer running.
    void stop() override {
        const std::lock_guard lock(m_control);
        if (m_running.exchange(false)) {
            AudioOutputUnitStop(m_unit);
        }
    }

    bool isRunning() const noexcept override { return m_running.load(); }
    Duration latency() const noexcept override { return std::chrono::nanoseconds(m_latencyNs.load()); }
    String deviceName() const override {
        const AudioObjectID device = defaultOutput();
        CFStringRef name = nullptr;
        UInt32 size = sizeof name;
        const AudioObjectPropertyAddress address = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain};
        if (device == kAudioObjectUnknown ||
            AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) != noErr || !name) {
            return {};
        }
        char text[256] = {};
        CFStringGetCString(name, text, sizeof text, kCFStringEncodingUTF8);
        CFRelease(name);
        return text;
    }
    int deviceSampleRate() const noexcept override { return m_deviceRate.load(); }
    int deviceChannels() const noexcept override { return m_deviceChannels.load(); }
    std::uint64_t deviceChanges() const noexcept override { return m_changes.load(); }

private:
    static double deviceRate(AudioObjectID device) noexcept {
        Float64 rate = 0;
        UInt32 size = sizeof rate;
        const AudioObjectPropertyAddress address = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate) != noErr) {
            return 0;
        }
        return rate;
    }

    // The current device's rate, channels and latency.
    void measure() noexcept {
        const AudioObjectID device = defaultOutput();
        if (device == kAudioObjectUnknown) {
            return;
        }
        const double rate = deviceRate(device);
        AudioStreamBasicDescription hardware{};
        UInt32 size = sizeof hardware;
        if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &hardware, &size) == noErr) {
            m_deviceChannels.store(int(hardware.mChannelsPerFrame));
        }
        m_deviceRate.store(int(std::lround(rate)));
        const UInt32 frames = deviceNumber(device, kAudioDevicePropertyBufferFrameSize) +
                              deviceNumber(device, kAudioDevicePropertyLatency) +
                              deviceNumber(device, kAudioDevicePropertySafetyOffset);
        if (rate > 0) {
            m_latencyNs.store(std::int64_t(double(frames) / rate * 1e9));
        }
    }

    static OSStatus defaultChanged(AudioObjectID, UInt32, const AudioObjectPropertyAddress *, void *self) {
        auto *device = static_cast<CoreAudioDevice *>(self);
        device->m_changes.fetch_add(1);
        device->measure();
        return noErr;
    }

    static OSStatus render(void *self, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames,
                           AudioBufferList *data) {
        auto *device = static_cast<CoreAudioDevice *>(self);
        auto *out = static_cast<float *>(data->mBuffers[0].mData);
        std::fill_n(out, std::size_t(frames) * kChannels, 0.0f);
        for (UInt32 done = 0; done < frames;) {
            const UInt32 block = std::min<UInt32>(frames - done, kMaxCallbackFrames);
            device->m_callback(out + std::size_t(done) * kChannels, int(block));
            done += block;
        }
        for (std::size_t i = 0; i < std::size_t(frames) * kChannels; ++i) {
            if (!std::isfinite(out[i])) {
                out[i] = 0.0f;
            }
        }
        return noErr;
    }

    AudioCallback m_callback;
    AudioUnit m_unit = nullptr;
    std::mutex m_control;
    std::atomic<bool> m_running{false};
    std::atomic<std::int64_t> m_latencyNs{0};
    std::atomic<int> m_deviceRate{0};
    std::atomic<int> m_deviceChannels{0};
    std::atomic<std::uint64_t> m_changes{0};
};

} // namespace

Result<std::unique_ptr<AudioDevice>> AudioDevice::open(AudioCallback callback, const AudioDeviceOptions &options) {
    require(bool(callback), "AudioDevice::open: the callback is empty");
    auto device = std::make_unique<CoreAudioDevice>(std::move(callback));
    if (Result<void> opened = device->initialise(options); !opened) {
        return std::move(opened).error();
    }
    return std::unique_ptr<AudioDevice>(std::move(device));
}

} // namespace cfw
