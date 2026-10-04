// What every platform shares: the null device.

#include "cfw/audio/AudioDevice.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "cfw/core/Contract.h"

namespace cfw {

AudioDevice::~AudioDevice() = default;

struct NullAudioDevice::State {
    AudioCallback callback;
    bool paced = true;
    std::vector<float> buffer;
    std::atomic<std::uint64_t> frames{0};
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    bool running = false; // under mutex
    std::atomic<bool> isRunning{false};

    void pull(int frames48k) {
        while (frames48k > 0) {
            const int block = std::min(frames48k, AudioDevice::kMaxCallbackFrames);
            std::fill_n(buffer.data(), std::size_t(block) * 2, 0.0f);
            callback(buffer.data(), block);
            frames.fetch_add(std::uint64_t(block), std::memory_order_relaxed);
            frames48k -= block;
        }
    }

    // 10 ms blocks on the steady clock, as a sound card's interrupts would.
    void run() {
        constexpr int kBlock = AudioDevice::kSampleRate / 100;
        const auto period = std::chrono::milliseconds(10);
        auto due = std::chrono::steady_clock::now() + period;
        std::unique_lock lock(mutex);
        while (running) {
            if (wake.wait_until(lock, due, [this] { return !running; })) {
                break;
            }
            lock.unlock();
            pull(kBlock);
            lock.lock();
            due += period;
            // After a long stall, carry on from now instead of catching up.
            const auto now = std::chrono::steady_clock::now();
            if (now - due > std::chrono::milliseconds(200)) {
                due = now + period;
            }
        }
    }
};

std::unique_ptr<NullAudioDevice> AudioDevice::openNull(AudioCallback callback, bool paced) {
    require(bool(callback), "AudioDevice::openNull: the callback is empty");
    auto state = std::make_unique<NullAudioDevice::State>();
    state->callback = std::move(callback);
    state->paced = paced;
    state->buffer.assign(std::size_t(kMaxCallbackFrames) * 2, 0.0f);
    return std::unique_ptr<NullAudioDevice>(new NullAudioDevice(std::move(state)));
}

NullAudioDevice::NullAudioDevice(std::unique_ptr<State> state) : m_state(std::move(state)) {}

NullAudioDevice::~NullAudioDevice() { stop(); }

Result<void> NullAudioDevice::start() {
    std::unique_lock lock(m_state->mutex);
    if (m_state->running) {
        return success();
    }
    m_state->running = true;
    m_state->isRunning = true;
    if (m_state->paced) {
        lock.unlock();
        m_state->thread = std::thread([state = m_state.get()] { state->run(); });
    }
    return success();
}

void NullAudioDevice::stop() {
    require(std::this_thread::get_id() != m_state->thread.get_id(), "AudioDevice::stop called from the audio callback");
    {
        const std::lock_guard lock(m_state->mutex);
        if (!m_state->running) {
            return;
        }
        m_state->running = false;
        m_state->isRunning = false;
    }
    m_state->wake.notify_all();
    if (m_state->thread.joinable()) {
        m_state->thread.join();
    }
}

bool NullAudioDevice::isRunning() const noexcept { return m_state->isRunning.load(); }

void NullAudioDevice::render(int frames) {
    require(!m_state->paced, "NullAudioDevice::render: only an unpaced device is pulled by hand");
    m_state->pull(frames);
}

std::uint64_t NullAudioDevice::framesRendered() const noexcept { return m_state->frames.load(std::memory_order_relaxed); }

} // namespace cfw
