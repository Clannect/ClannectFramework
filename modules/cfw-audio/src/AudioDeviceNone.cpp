// Platforms without a sound backend: opening reports Unsupported, and the
// application carries on silent (or with the null device).

#include "cfw/audio/AudioDevice.h"
#include "cfw/core/Contract.h"

namespace cfw {

Result<std::unique_ptr<AudioDevice>> AudioDevice::open(AudioCallback callback, const AudioDeviceOptions &) {
    require(bool(callback), "AudioDevice::open: the callback is empty");
    return Error(ErrorCode::Unsupported, "no sound output backend for this platform");
}

} // namespace cfw
