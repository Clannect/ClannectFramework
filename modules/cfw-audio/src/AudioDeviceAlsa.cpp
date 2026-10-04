// Linux: ALSA, through libasound loaded at run time with dlopen, so CFW
// neither links against it nor fails to start on a machine without it (as
// cfw-net does with OpenSSL; docs/decisions/0013). No ALSA headers are
// needed to build: the functions used are declared here with their
// documented C signatures, and the few constants are part of ALSA's stable
// ABI.
//
// The PCM opened is "default". On a desktop that is the sound server's ALSA
// plugin (PipeWire's or PulseAudio's), which follows the user's default
// output by itself; on a bare ALSA system it is the first card, and if that
// goes away (a USB headset unplugged) the write fails, the device is closed,
// and the thread keeps pulling the callback on a timer while it tries to
// open "default" again once a second.

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "DeviceRender.h"
#include "cfw/core/Contract.h"

namespace cfw {

namespace {

constexpr int kStreamPlayback = 0;
constexpr int kAccessInterleaved = 3; // SND_PCM_ACCESS_RW_INTERLEAVED
constexpr int kFormatS16 = 2;         // SND_PCM_FORMAT_S16_LE
constexpr int kFormatS32 = 10;        // SND_PCM_FORMAT_S32_LE
constexpr int kFormatFloat = 14;      // SND_PCM_FORMAT_FLOAT_LE

struct Alsa {
    int (*snd_pcm_open)(void **, const char *, int, int) = nullptr;
    int (*snd_pcm_close)(void *) = nullptr;
    int (*snd_pcm_hw_params_malloc)(void **) = nullptr;
    void (*snd_pcm_hw_params_free)(void *) = nullptr;
    int (*snd_pcm_hw_params_any)(void *, void *) = nullptr;
    int (*snd_pcm_hw_params_set_access)(void *, void *, int) = nullptr;
    int (*snd_pcm_hw_params_set_format)(void *, void *, int) = nullptr;
    int (*snd_pcm_hw_params_set_channels_near)(void *, void *, unsigned *) = nullptr;
    int (*snd_pcm_hw_params_set_rate_near)(void *, void *, unsigned *, int *) = nullptr;
    int (*snd_pcm_hw_params_set_period_time_near)(void *, void *, unsigned *, int *) = nullptr;
    int (*snd_pcm_hw_params_set_buffer_time_near)(void *, void *, unsigned *, int *) = nullptr;
    int (*snd_pcm_hw_params)(void *, void *) = nullptr;
    int (*snd_pcm_hw_params_get_buffer_size)(const void *, unsigned long *) = nullptr;
    int (*snd_pcm_hw_params_get_period_size)(const void *, unsigned long *, int *) = nullptr;
    int (*snd_pcm_prepare)(void *) = nullptr;
    long (*snd_pcm_writei)(void *, const void *, unsigned long) = nullptr;
    int (*snd_pcm_recover)(void *, int, int) = nullptr;
    int (*snd_pcm_drop)(void *) = nullptr;
    const char *(*snd_strerror)(int) = nullptr;
    bool loaded = false;
};

template <class F> bool bind(void *library, const char *name, F &target) {
    target = reinterpret_cast<F>(dlsym(library, name));
    return target != nullptr;
}

Alsa load() {
    Alsa a;
    // The versioned name: the unversioned symlink only comes with the
    // development package.
    void *library = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        library = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!library) {
        return a;
    }
    // Never dlclose'd: the library keeps global state (its configuration cache).
    a.loaded = bind(library, "snd_pcm_open", a.snd_pcm_open) && bind(library, "snd_pcm_close", a.snd_pcm_close) &&
               bind(library, "snd_pcm_hw_params_malloc", a.snd_pcm_hw_params_malloc) &&
               bind(library, "snd_pcm_hw_params_free", a.snd_pcm_hw_params_free) &&
               bind(library, "snd_pcm_hw_params_any", a.snd_pcm_hw_params_any) &&
               bind(library, "snd_pcm_hw_params_set_access", a.snd_pcm_hw_params_set_access) &&
               bind(library, "snd_pcm_hw_params_set_format", a.snd_pcm_hw_params_set_format) &&
               bind(library, "snd_pcm_hw_params_set_channels_near", a.snd_pcm_hw_params_set_channels_near) &&
               bind(library, "snd_pcm_hw_params_set_rate_near", a.snd_pcm_hw_params_set_rate_near) &&
               bind(library, "snd_pcm_hw_params_set_period_time_near", a.snd_pcm_hw_params_set_period_time_near) &&
               bind(library, "snd_pcm_hw_params_set_buffer_time_near", a.snd_pcm_hw_params_set_buffer_time_near) &&
               bind(library, "snd_pcm_hw_params", a.snd_pcm_hw_params) &&
               bind(library, "snd_pcm_hw_params_get_buffer_size", a.snd_pcm_hw_params_get_buffer_size) &&
               bind(library, "snd_pcm_hw_params_get_period_size", a.snd_pcm_hw_params_get_period_size) &&
               bind(library, "snd_pcm_prepare", a.snd_pcm_prepare) && bind(library, "snd_pcm_writei", a.snd_pcm_writei) &&
               bind(library, "snd_pcm_recover", a.snd_pcm_recover) && bind(library, "snd_pcm_drop", a.snd_pcm_drop) &&
               bind(library, "snd_strerror", a.snd_strerror);
    return a;
}

// Loaded once, on first use, and immutable afterwards.
const Alsa &alsa() {
    static const Alsa instance = load();
    return instance;
}

String pcmName() {
    const char *chosen = std::getenv("CFW_ALSA_DEVICE");
    return chosen && *chosen ? String(chosen) : String("default");
}

class AlsaDevice final : public AudioDevice {
public:
    AlsaDevice(AudioCallback callback, const AudioDeviceOptions &options)
        : m_callback(std::move(callback)), m_options(options), m_renderer(m_callback), m_name(pcmName()) {}

    ~AlsaDevice() override {
        stop();
        closePcm();
    }

    // Opens and configures the PCM. With `error`, says why it failed.
    bool openPcm(Error *error) {
        const Alsa &a = alsa();
        const auto fail = [&](ErrorCode code, const char *what, int status) {
            if (error) {
                *error = Error(code, String(what) + ": " + a.snd_strerror(status)).with("device", m_name);
            }
            closePcm();
            return false;
        };
        int status = a.snd_pcm_open(&m_pcm, m_name.c_str(), kStreamPlayback, 0);
        if (status < 0) {
            m_pcm = nullptr;
            return fail(ErrorCode::NotFound, "no output device", status);
        }
        void *params = nullptr;
        if ((status = a.snd_pcm_hw_params_malloc(&params)) < 0) {
            return fail(ErrorCode::OutOfMemory, "could not configure the output device", status);
        }
        detail::DeviceSampleFormat format = detail::DeviceSampleFormat::Float32;
        unsigned channels = unsigned(kChannels);
        unsigned rate = unsigned(kSampleRate);
        const auto target = unsigned(std::clamp<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(m_options.targetLatency).count(), 5000, 500000));
        unsigned period = target / 3;
        unsigned buffer = target;
        unsigned long bufferFrames = 0;
        unsigned long periodFrames = 0;
        int direction = 0;
        status = a.snd_pcm_hw_params_any(m_pcm, params);
        if (status >= 0) {
            status = a.snd_pcm_hw_params_set_access(m_pcm, params, kAccessInterleaved);
        }
        if (status >= 0) {
            // Float if the device (or its plugin) takes it, else the integers
            // every sound card has.
            status = a.snd_pcm_hw_params_set_format(m_pcm, params, kFormatFloat);
            if (status < 0) {
                format = detail::DeviceSampleFormat::Int16;
                status = a.snd_pcm_hw_params_set_format(m_pcm, params, kFormatS16);
            }
            if (status < 0) {
                format = detail::DeviceSampleFormat::Int32;
                status = a.snd_pcm_hw_params_set_format(m_pcm, params, kFormatS32);
            }
        }
        if (status >= 0) {
            status = a.snd_pcm_hw_params_set_channels_near(m_pcm, params, &channels);
        }
        if (status >= 0) {
            status = a.snd_pcm_hw_params_set_rate_near(m_pcm, params, &rate, &direction);
        }
        if (status >= 0) {
            direction = 0;
            a.snd_pcm_hw_params_set_period_time_near(m_pcm, params, &period, &direction);
            direction = 0;
            a.snd_pcm_hw_params_set_buffer_time_near(m_pcm, params, &buffer, &direction);
            status = a.snd_pcm_hw_params(m_pcm, params);
        }
        if (status >= 0) {
            status = a.snd_pcm_hw_params_get_buffer_size(params, &bufferFrames);
        }
        if (status >= 0) {
            status = a.snd_pcm_hw_params_get_period_size(params, &periodFrames, &direction);
        }
        a.snd_pcm_hw_params_free(params);
        if (status < 0 || channels == 0 || rate == 0 || periodFrames == 0) {
            return fail(ErrorCode::IoError, "the output device refused every format", status < 0 ? status : -EINVAL);
        }
        m_periodFrames = int(std::min<unsigned long>(periodFrames, 8192));
        m_renderer.configure(int(rate), int(channels), format);
        m_block.assign(std::size_t(m_periodFrames) * m_renderer.frameBytes(), 0);
        m_latencyNs.store(std::int64_t(bufferFrames) * 1000000000 / std::int64_t(rate));
        m_deviceRate.store(int(rate));
        m_deviceChannels.store(int(channels));
        return true;
    }

    Result<void> start() override {
        const std::lock_guard lock(m_control);
        if (m_running.load()) {
            return success();
        }
        m_running.store(true);
        m_thread = std::thread([this] { run(); });
        return success();
    }

    void stop() override {
        require(std::this_thread::get_id() != m_thread.get_id(), "AudioDevice::stop called from the audio callback");
        const std::lock_guard lock(m_control);
        if (!m_running.exchange(false)) {
            return;
        }
        m_thread.join();
        if (m_pcm) {
            // Throw away what is queued, and be ready to start again.
            alsa().snd_pcm_drop(m_pcm);
            alsa().snd_pcm_prepare(m_pcm);
        }
    }

    bool isRunning() const noexcept override { return m_running.load(); }
    Duration latency() const noexcept override { return std::chrono::nanoseconds(m_latencyNs.load()); }
    String deviceName() const override { return m_name; }
    int deviceSampleRate() const noexcept override { return m_deviceRate.load(); }
    int deviceChannels() const noexcept override { return m_deviceChannels.load(); }
    std::uint64_t deviceChanges() const noexcept override { return m_changes.load(); }

private:
    void closePcm() noexcept {
        if (m_pcm) {
            alsa().snd_pcm_close(m_pcm);
            m_pcm = nullptr;
        }
    }

    void run() {
        const Alsa &a = alsa();
        auto lastTick = std::chrono::steady_clock::now();
        auto lastAttempt = lastTick;
        while (m_running.load()) {
            if (!m_pcm) {
                // The device went away: keep the application's audio clock
                // moving, and look for a device again once a second.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastTick).count();
                const auto frames = std::clamp<long long>(elapsed * kSampleRate / 1000000, 0, kSampleRate / 4);
                m_renderer.discard(int(frames));
                lastTick = now;
                if (now - lastAttempt > std::chrono::seconds(1)) {
                    lastAttempt = now;
                    if (openPcm(nullptr)) {
                        m_changes.fetch_add(1);
                    }
                }
                continue;
            }
            m_renderer.render(m_block.data(), m_periodFrames);
            const std::uint8_t *from = m_block.data();
            unsigned long left = static_cast<unsigned long>(m_periodFrames);
            while (left > 0 && m_running.load()) {
                // Blocks until the device has room: this is the thread's clock.
                long written = a.snd_pcm_writei(m_pcm, from, left);
                if (written < 0) {
                    // An underrun or a suspend is recovered in place; anything
                    // else means the device is gone.
                    if (a.snd_pcm_recover(m_pcm, int(written), 1) < 0) {
                        closePcm();
                        lastTick = lastAttempt = std::chrono::steady_clock::now();
                        break;
                    }
                    continue;
                }
                from += std::size_t(written) * m_renderer.frameBytes();
                left -= static_cast<unsigned long>(written);
            }
        }
    }

    AudioCallback m_callback;
    AudioDeviceOptions m_options;
    detail::DeviceRenderer m_renderer;
    String m_name;
    void *m_pcm = nullptr;
    int m_periodFrames = 0;
    std::vector<std::uint8_t> m_block;
    std::mutex m_control; // start() and stop()
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<std::int64_t> m_latencyNs{0};
    std::atomic<int> m_deviceRate{0};
    std::atomic<int> m_deviceChannels{0};
    std::atomic<std::uint64_t> m_changes{0};
};

} // namespace

Result<std::unique_ptr<AudioDevice>> AudioDevice::open(AudioCallback callback, const AudioDeviceOptions &options) {
    require(bool(callback), "AudioDevice::open: the callback is empty");
    if (!alsa().loaded) {
        return Error(ErrorCode::Unsupported, "no sound output: libasound.so.2 (ALSA) is not installed");
    }
    auto device = std::make_unique<AlsaDevice>(std::move(callback), options);
    Error error(ErrorCode::IoError, "the output device could not be opened");
    if (!device->openPcm(&error)) {
        return error;
    }
    return std::unique_ptr<AudioDevice>(std::move(device));
}

} // namespace cfw
