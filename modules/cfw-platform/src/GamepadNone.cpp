// Platforms without a gamepad backend: no real pads; virtual ones work.

#include "cfw/platform/Gamepad.h"

namespace cfw {

namespace {
class GamepadsNone final : public Gamepads {};
} // namespace

std::unique_ptr<Gamepads> Gamepads::create() { return std::make_unique<GamepadsNone>(); }

} // namespace cfw
