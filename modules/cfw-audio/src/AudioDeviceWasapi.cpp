// Windows: WASAPI in shared mode, event driven. One thread owns every COM
// object: it opens the default render endpoint, waits for the audio engine
// to ask for data, and fills the engine's buffer through DeviceRenderer.
//
// Following the default device: an IMMNotificationClient says when the
// default changes, and a failing call says when the current endpoint is gone
// (unplugged). Either way the thread closes the endpoint and opens the new
// default; while there is none it keeps pulling the callback on a timer and
// tries again twice a second.
//
// The interface and class ids are spelled out here, so nothing depends on
// which import libraries a toolchain ships.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <propidl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

#include "DeviceRender.h"
#include "cfw/core/Contract.h"
#include "cfw/core/Utf8.h"

namespace cfw {

namespace {

using detail::DeviceSampleFormat;

constexpr GUID kClsidDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr GUID kIidDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr GUID kIidNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
constexpr GUID kIidAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
constexpr GUID kIidAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
constexpr GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr PROPERTYKEY kFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

constexpr WORD kFormatExtensible = 0xFFFE;
constexpr WORD kFormatFloat = 3;
constexpr WORD kFormatPcm = 1;

template <class T> void release(T *&pointer) noexcept {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

// Tells the device thread that the default output changed. COM objects
// delete themselves in Release, so the interface has no virtual destructor.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
class DefaultDeviceWatcher final : public IMMNotificationClient {
public:
    DefaultDeviceWatcher(std::atomic<bool> &changed, HANDLE wake) : m_changed(changed), m_wake(wake) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, kIidUnknown) || IsEqualGUID(riid, kIidNotificationClient)) {
            *object = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long refs = --m_refs;
        if (refs == 0) {
            delete this;
        }
        return ULONG(refs);
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            notify();
        }
        return S_OK;
    }
    // A device arriving may be the first there is.
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~DefaultDeviceWatcher() = default;
    void notify() noexcept {
        m_changed.store(true);
        SetEvent(m_wake);
    }
    std::atomic<bool> &m_changed;
    HANDLE m_wake;
    std::atomic<long> m_refs{1};
};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

String describe(HRESULT result) {
    char text[32];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(result));
    return text;
}

class WasapiDevice final : public AudioDevice {
public:
    WasapiDevice(AudioCallback callback, const AudioDeviceOptions &options)
        : m_callback(std::move(callback)), m_options(options), m_renderer(m_callback) {
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_bufferReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~WasapiDevice() override {
        if (m_thread.joinable()) {
            command(Command::Quit);
            m_thread.join();
        }
        CloseHandle(m_wake);
        CloseHandle(m_bufferReady);
    }

    // Starts the device thread and waits for it to open the endpoint.
    Result<void> initialise() {
        m_thread = std::thread([this] { run(); });
        std::unique_lock lock(m_mutex);
        m_changedState.wait(lock, [this] { return m_opened; });
        if (m_openError) {
            Error error = std::move(*m_openError);
            lock.unlock();
            command(Command::Quit);
            m_thread.join();
            return error;
        }
        return success();
    }

    Result<void> start() override {
        command(Command::Run);
        return success();
    }
    void stop() override {
        require(std::this_thread::get_id() != m_thread.get_id(), "AudioDevice::stop called from the audio callback");
        command(Command::Stop);
    }
    bool isRunning() const noexcept override { return m_running.load(); }
    Duration latency() const noexcept override { return std::chrono::nanoseconds(m_latencyNs.load()); }
    String deviceName() const override {
        const std::lock_guard lock(m_mutex);
        return m_name;
    }
    int deviceSampleRate() const noexcept override { return m_deviceRate.load(); }
    int deviceChannels() const noexcept override { return m_deviceChannels.load(); }
    std::uint64_t deviceChanges() const noexcept override { return m_changes.load(); }

private:
    enum class Command { Stop, Run, Quit };

    // Hands the thread a command and waits until it has taken effect: after
    // Stop returns, the callback is not running.
    void command(Command wanted) {
        std::unique_lock lock(m_mutex);
        m_wanted = wanted;
        ++m_commandSerial;
        const std::uint64_t serial = m_commandSerial;
        SetEvent(m_wake);
        m_changedState.wait(lock, [&] { return m_handledSerial >= serial || m_exited; });
    }

    // ---- The device thread ----

    HRESULT openEndpoint(String &failure) {
        IMMDevice *device = nullptr;
        HRESULT result = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result)) {
            failure = "no default output device";
            m_noEndpoint = true;
            return result;
        }
        m_noEndpoint = false;
        result = device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&m_client));
        WAVEFORMATEX *format = nullptr;
        if (SUCCEEDED(result)) {
            result = m_client->GetMixFormat(&format);
        }
        DeviceSampleFormat sampleFormat = DeviceSampleFormat::Float32;
        if (SUCCEEDED(result)) {
            WORD tag = format->wFormatTag;
            if (tag == kFormatExtensible && format->cbSize >= 22) {
                const GUID &subtype = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format)->SubFormat;
                tag = IsEqualGUID(subtype, kSubtypeFloat) ? kFormatFloat : IsEqualGUID(subtype, kSubtypePcm) ? kFormatPcm : WORD(0);
            }
            const WORD bytes = WORD(format->nChannels ? format->nBlockAlign / format->nChannels : 0);
            if (tag == kFormatFloat && bytes == 4) {
                sampleFormat = DeviceSampleFormat::Float32;
            } else if (tag == kFormatPcm && bytes == 2) {
                sampleFormat = DeviceSampleFormat::Int16;
            } else if (tag == kFormatPcm && bytes == 3) {
                sampleFormat = DeviceSampleFormat::Int24;
            } else if (tag == kFormatPcm && bytes == 4) {
                sampleFormat = DeviceSampleFormat::Int32;
            } else {
                failure = "the output device's mix format is not PCM or float";
                result = E_FAIL;
            }
        }
        if (SUCCEEDED(result)) {
            // In 100 ns units. The engine rounds it up to whole device periods.
            const auto wanted = std::chrono::duration_cast<std::chrono::nanoseconds>(m_options.targetLatency).count() / 100;
            result = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                          REFERENCE_TIME(std::clamp<long long>(wanted, 30000, 5000000)), 0, format, nullptr);
        }
        if (SUCCEEDED(result)) {
            result = m_client->SetEventHandle(m_bufferReady);
        }
        if (SUCCEEDED(result)) {
            result = m_client->GetBufferSize(&m_bufferFrames);
        }
        if (SUCCEEDED(result)) {
            result = m_client->GetService(kIidAudioRenderClient, reinterpret_cast<void **>(&m_render));
        }
        if (SUCCEEDED(result)) {
            m_renderer.configure(int(format->nSamplesPerSec), int(format->nChannels), sampleFormat);
            REFERENCE_TIME stream = 0;
            m_client->GetStreamLatency(&stream);
            const std::int64_t buffer = std::int64_t(m_bufferFrames) * 1000000000 / std::int64_t(format->nSamplesPerSec);
            m_latencyNs.store(buffer + std::int64_t(stream) * 100);
            m_deviceRate.store(int(format->nSamplesPerSec));
            m_deviceChannels.store(int(format->nChannels));
            String name;
            IPropertyStore *properties = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
                    name = utf16ToUtf8Lossy(
                        std::u16string_view(reinterpret_cast<const char16_t *>(value.pwszVal), wcslen(value.pwszVal)));
                }
                PropVariantClear(&value);
                properties->Release();
            }
            const std::lock_guard lock(m_mutex);
            m_name = std::move(name);
        } else {
            if (failure.empty()) {
                failure = "the output device could not be opened";
            }
            closeEndpoint();
        }
        if (format) {
            CoTaskMemFree(format);
        }
        device->Release();
        return result;
    }

    void closeEndpoint() noexcept {
        if (m_client && m_started) {
            m_client->Stop();
        }
        m_started = false;
        release(m_render);
        release(m_client);
    }

    // Fills what the engine's buffer has room for. False: the endpoint is gone.
    bool fill() noexcept {
        UINT32 padding = 0;
        if (FAILED(m_client->GetCurrentPadding(&padding))) {
            return false;
        }
        const UINT32 room = m_bufferFrames > padding ? m_bufferFrames - padding : 0;
        if (room == 0) {
            return true;
        }
        BYTE *data = nullptr;
        if (FAILED(m_render->GetBuffer(room, &data)) || !data) {
            return false;
        }
        m_renderer.render(data, int(room));
        return SUCCEEDED(m_render->ReleaseBuffer(room, 0));
    }

    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        String failure;
        HRESULT result = CoCreateInstance(kClsidDeviceEnumerator, nullptr, CLSCTX_ALL, kIidDeviceEnumerator,
                                          reinterpret_cast<void **>(&m_enumerator));
        DefaultDeviceWatcher *watcher = nullptr;
        if (SUCCEEDED(result)) {
            result = openEndpoint(failure);
        } else {
            failure = "the system's audio service is not available";
        }
        {
            const std::lock_guard lock(m_mutex);
            if (FAILED(result)) {
                const ErrorCode code = !m_enumerator ? ErrorCode::Unsupported
                                     : m_noEndpoint  ? ErrorCode::NotFound
                                                     : ErrorCode::IoError;
                m_openError = Error(code, failure).with("hresult", describe(result));
            }
            m_opened = true;
        }
        m_changedState.notify_all();
        if (SUCCEEDED(result)) {
            watcher = new DefaultDeviceWatcher(m_defaultChanged, m_wake);
            m_enumerator->RegisterEndpointNotificationCallback(watcher);
            // The system's scheduling class for audio threads, where it has one.
            raisePriority();
        }

        bool running = false;
        auto lastTick = std::chrono::steady_clock::now();
        auto lastAttempt = lastTick;
        for (;;) {
            // Sleep until the engine wants data, a command arrives, or (with no
            // endpoint) the next block is due.
            DWORD timeout = INFINITE;
            if (running) {
                timeout = m_client ? 200 : 10;
            }
            const HANDLE handles[2] = {m_wake, m_bufferReady};
            WaitForMultipleObjects(2, handles, FALSE, timeout);

            Command wanted;
            std::uint64_t serial;
            {
                const std::lock_guard lock(m_mutex);
                wanted = m_wanted;
                serial = m_commandSerial;
            }
            if (FAILED(result)) {
                wanted = Command::Quit; // never opened: only leaving is left
            }
            if (wanted != Command::Quit) {
                const bool shouldRun = wanted == Command::Run;
                if (shouldRun != running) {
                    running = shouldRun;
                    if (m_client) {
                        if (running) {
                            m_started = SUCCEEDED(m_client->Start());
                        } else {
                            m_client->Stop();
                            m_client->Reset();
                            m_started = false;
                        }
                    }
                    lastTick = std::chrono::steady_clock::now();
                    m_running.store(running);
                }
            }
            {
                const std::lock_guard lock(m_mutex);
                m_handledSerial = serial;
            }
            m_changedState.notify_all();
            if (wanted == Command::Quit) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            bool reopen = m_defaultChanged.exchange(false);
            if (running && m_client && !reopen) {
                reopen = !fill();
            }
            if (!m_client && now - lastAttempt > std::chrono::milliseconds(500)) {
                reopen = true;
            }
            if (reopen) {
                closeEndpoint();
                lastAttempt = now;
                String ignored;
                if (SUCCEEDED(openEndpoint(ignored))) {
                    m_changes.fetch_add(1);
                    if (running) {
                        m_started = SUCCEEDED(m_client->Start());
                    }
                }
                lastTick = now;
            }
            if (running && !m_client) {
                // No endpoint: keep the application's audio clock moving.
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastTick).count();
                const auto frames = std::clamp<long long>(elapsed * kSampleRate / 1000000, 0, kSampleRate / 4);
                if (frames > 0) {
                    m_renderer.discard(int(frames));
                    lastTick += std::chrono::microseconds(frames * 1000000 / kSampleRate);
                }
            }
        }

        closeEndpoint();
        if (watcher) {
            m_enumerator->UnregisterEndpointNotificationCallback(watcher);
            watcher->Release();
        }
        release(m_enumerator);
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        {
            const std::lock_guard lock(m_mutex);
            m_exited = true;
        }
        m_running.store(false);
        m_changedState.notify_all();
    }

    static void raisePriority() noexcept {
        const HMODULE avrt = LoadLibraryW(L"avrt.dll");
        if (!avrt) {
            return;
        }
        using Set = HANDLE(WINAPI *)(LPCWSTR, LPDWORD);
        const auto set = reinterpret_cast<Set>(reinterpret_cast<void *>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")));
        if (set) {
            DWORD task = 0;
            set(L"Pro Audio", &task);
        }
    }

    AudioCallback m_callback;
    AudioDeviceOptions m_options;
    detail::DeviceRenderer m_renderer;
    HANDLE m_wake = nullptr;
    HANDLE m_bufferReady = nullptr;
    std::thread m_thread;

    mutable std::mutex m_mutex;
    std::condition_variable m_changedState;
    bool m_opened = false;
    bool m_exited = false;
    std::optional<Error> m_openError;
    Command m_wanted = Command::Stop;
    std::uint64_t m_commandSerial = 0;
    std::uint64_t m_handledSerial = 0;
    String m_name;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_defaultChanged{false};
    std::atomic<std::int64_t> m_latencyNs{0};
    std::atomic<int> m_deviceRate{0};
    std::atomic<int> m_deviceChannels{0};
    std::atomic<std::uint64_t> m_changes{0};

    // The device thread's own.
    IMMDeviceEnumerator *m_enumerator = nullptr;
    IAudioClient *m_client = nullptr;
    IAudioRenderClient *m_render = nullptr;
    UINT32 m_bufferFrames = 0;
    bool m_started = false;
    bool m_noEndpoint = false;
};

} // namespace

Result<std::unique_ptr<AudioDevice>> AudioDevice::open(AudioCallback callback, const AudioDeviceOptions &options) {
    require(bool(callback), "AudioDevice::open: the callback is empty");
    auto device = std::make_unique<WasapiDevice>(std::move(callback), options);
    if (Result<void> opened = device->initialise(); !opened) {
        return std::move(opened).error();
    }
    return std::unique_ptr<AudioDevice>(std::move(device));
}

} // namespace cfw
