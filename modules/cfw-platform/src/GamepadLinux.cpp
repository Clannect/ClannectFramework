// Linux gamepads: evdev, straight from the kernel (/dev/input/event*), with
// no udev or SDL. Each event device is asked what it has (EVIOCGBIT,
// EVIOCGABS); the ones with controller buttons and an axis are pads, and
// detail::EvdevPad turns their events into states. Hot-plug is inotify on
// /dev/input: a node appearing (or its permissions settling, which is when
// it becomes readable) triggers a rescan, and a read failing with ENODEV is
// a pad unplugged. Rumble is the kernel's force feedback (EV_FF).
//
// The kernel's structures and request numbers are spelled out here, as the
// OpenSSL and ALSA backends do with theirs: they are stable ABI, and
// <linux/input.h> is not on every build machine.
//
// UNVERIFIED with hardware: the mapping is tested through EvdevPad with
// recorded-style event sequences, and the scan and inotify paths ran in a
// container that has no input devices; no real pad was read.

#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>

#include "GamepadMapping.h"
#include "cfw/platform/Gamepad.h"

namespace cfw {

namespace {

// <linux/input.h>
struct InputEvent {
    long seconds;
    long microseconds;
    std::uint16_t type;
    std::uint16_t code;
    std::int32_t value;
};
struct InputId {
    std::uint16_t bus, vendor, product, version;
};
struct InputAbsInfo {
    std::int32_t value, minimum, maximum, fuzz, flat, resolution;
};
// struct ff_effect with its rumble member; the union's largest member
// (ff_periodic_effect) holds a pointer, which sets its size and alignment.
struct FfEffect {
    std::uint16_t type;
    std::int16_t id;
    std::uint16_t direction;
    std::uint16_t triggerButton, triggerInterval;
    std::uint16_t replayLength, replayDelay;
    union {
        struct {
            std::uint16_t strong, weak;
        } rumble;
        struct {
            std::uint16_t a[8];
            std::uint32_t b;
            void *c;
        } largest;
    } u;
};

constexpr unsigned long ioc(unsigned long direction, unsigned long type, unsigned long number, unsigned long size) {
    return (direction << 30) | (size << 16) | (type << 8) | number;
}
constexpr unsigned long kRead = 2, kWrite = 1;
constexpr unsigned long kGetId = ioc(kRead, 'E', 0x02, sizeof(InputId));
constexpr unsigned long getName(unsigned long size) { return ioc(kRead, 'E', 0x06, size); }
constexpr unsigned long getBits(unsigned long event, unsigned long size) { return ioc(kRead, 'E', 0x20 + event, size); }
constexpr unsigned long getAbs(unsigned long axis) { return ioc(kRead, 'E', 0x40 + axis, sizeof(InputAbsInfo)); }
constexpr unsigned long kSendEffect = ioc(kWrite, 'E', 0x80, sizeof(FfEffect));


constexpr int kEventKey = 0x01, kEventAbs = 0x03, kEventForce = 0x15;
constexpr int kKeyMax = 0x2FF, kAbsMax = 0x3F;
constexpr int kForceRumble = 0x50;

bool bit(const unsigned char *bits, int index) noexcept { return (bits[index / 8] >> (index % 8)) & 1; }

struct Device {
    int fd = -1;
    bool canWrite = false;
    int effect = -1;
    GamepadId id = 0;
    detail::EvdevPad pad;
};

class GamepadsLinux final : public Gamepads {
public:
    GamepadsLinux() {
        m_watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_watch >= 0) {
            inotify_add_watch(m_watch, "/dev/input", IN_CREATE | IN_ATTRIB);
        }
        scan();
    }

    ~GamepadsLinux() override {
        for (auto &[path, device] : m_devices) {
            ::close(device.fd);
        }
        if (m_watch >= 0) {
            ::close(m_watch);
        }
    }

protected:
    void readDevices() override {
        // A node appeared or became readable: look again.
        if (m_watch >= 0) {
            char buffer[4096];
            bool changed = false;
            while (::read(m_watch, buffer, sizeof buffer) > 0) {
                changed = true;
            }
            if (changed) {
                scan();
            }
        }
        for (auto it = m_devices.begin(); it != m_devices.end();) {
            Device &device = it->second;
            InputEvent events[64];
            bool gone = false;
            bool any = false;
            for (;;) {
                const ssize_t bytes = ::read(device.fd, events, sizeof events);
                if (bytes < 0) {
                    gone = errno != EAGAIN && errno != EINTR;
                    break;
                }
                if (bytes == 0) {
                    break;
                }
                for (std::size_t i = 0; i < std::size_t(bytes) / sizeof(InputEvent); ++i) {
                    device.pad.handle(events[i].type, events[i].code, events[i].value);
                    any = true;
                }
            }
            if (gone) {
                deviceRemoved(device.id);
                ::close(device.fd);
                it = m_devices.erase(it);
                continue;
            }
            if (any) {
                deviceState(device.id, device.pad.state());
            }
            ++it;
        }
    }

    bool setMotors(GamepadId id, float lowFrequency, float highFrequency) override {
        for (auto &[path, device] : m_devices) {
            if (device.id != id || !device.canWrite) {
                continue;
            }
            // Upload (or update) the effect, then play or stop it. The
            // length is the driver's maximum: poll() ends it at the time asked.
            FfEffect effect{};
            effect.type = kForceRumble;
            effect.id = std::int16_t(device.effect);
            effect.replayLength = 0xFFFF;
            effect.u.rumble.strong = std::uint16_t(lowFrequency * 65535.0f);
            effect.u.rumble.weak = std::uint16_t(highFrequency * 65535.0f);
            if (::ioctl(device.fd, kSendEffect, &effect) < 0) {
                return false;
            }
            device.effect = effect.id;
            InputEvent play{};
            play.type = kEventForce;
            play.code = std::uint16_t(effect.id);
            play.value = lowFrequency > 0.0f || highFrequency > 0.0f ? 1 : 0;
            return ::write(device.fd, &play, sizeof play) == ssize_t(sizeof play);
        }
        return false;
    }

private:
    void scan() {
        DIR *directory = ::opendir("/dev/input");
        if (!directory) {
            return;
        }
        while (const dirent *entry = ::readdir(directory)) {
            if (std::strncmp(entry->d_name, "event", 5) != 0) {
                continue;
            }
            const std::string path = std::string("/dev/input/") + entry->d_name;
            if (!m_devices.count(path)) {
                open(path);
            }
        }
        ::closedir(directory);
    }

    void open(const std::string &path) {
        Device device;
        // Writing is only for rumble; a read-only node still gives the pad.
        device.fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        device.canWrite = device.fd >= 0;
        if (device.fd < 0) {
            device.fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
        if (device.fd < 0) {
            return; // not ours to read (yet: IN_ATTRIB brings us back)
        }
        unsigned char keyBits[kKeyMax / 8 + 1] = {};
        unsigned char absBits[kAbsMax / 8 + 1] = {};
        unsigned char forceBits[16] = {};
        InputId id{};
        if (::ioctl(device.fd, getBits(kEventKey, sizeof keyBits), keyBits) < 0 ||
            ::ioctl(device.fd, getBits(kEventAbs, sizeof absBits), absBits) < 0) {
            ::close(device.fd);
            return;
        }
        ::ioctl(device.fd, kGetId, &id);
        std::vector<int> keys;
        for (int code = 0x100; code <= kKeyMax; ++code) {
            if (bit(keyBits, code)) {
                keys.push_back(code);
            }
        }
        std::vector<detail::EvdevPad::Axis> axes;
        for (int code = 0; code <= kAbsMax; ++code) {
            InputAbsInfo info{};
            if (bit(absBits, code) && ::ioctl(device.fd, getAbs(static_cast<unsigned long>(code)), &info) >= 0) {
                axes.push_back({code, info.minimum, info.maximum, info.value});
            }
        }
        device.pad.configure(std::move(keys), std::move(axes), id.vendor);
        if (!device.pad.isController()) {
            ::close(device.fd); // a keyboard, a mouse, a touchpad...
            return;
        }
        char name[256] = {};
        if (::ioctl(device.fd, getName(sizeof name - 1), name) < 0 || name[0] == '\0') {
            std::strcpy(name, "Gamepad");
        }
        if (device.canWrite && ::ioctl(device.fd, getBits(kEventForce, sizeof forceBits), forceBits) < 0) {
            std::memset(forceBits, 0, sizeof forceBits);
        }
        GamepadInfo info;
        info.name = name;
        info.standard = device.pad.isGamepad();
        info.canRumble = device.canWrite && bit(forceBits, kForceRumble);
        info.vendorId = id.vendor;
        info.productId = id.product;
        device.id = deviceAdded(std::move(info));
        deviceState(device.id, device.pad.state());
        m_devices.emplace(path, std::move(device));
    }

    int m_watch = -1;
    std::map<std::string, Device> m_devices;
};

} // namespace

std::unique_ptr<Gamepads> Gamepads::create() { return std::make_unique<GamepadsLinux>(); }

} // namespace cfw
