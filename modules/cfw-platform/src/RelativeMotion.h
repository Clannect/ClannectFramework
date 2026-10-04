#pragma once

// Relative mouse mode's arithmetic, apart from the system calls around it.
//
// A mouse reports how far it moved. A tablet, a remote desktop session or a
// virtual machine's pointer reports where it is instead, as a position on the
// screen; what moved is the difference from the position before. Either way
// the result is a delta in device counts (or screen pixels for absolute
// devices), which is what PointerEvent::delta carries.
//
// No OS calls: tested on every platform.

#include "cfw/core/Vec2.h"

namespace cfw::detail {

class RelativeMotion {
public:
    // A new stretch of relative mode: the next absolute position is where
    // the pointer starts, not a movement.
    void reset() noexcept { m_haveLast = false; }

    // A device that reports movement: the movement itself.
    [[nodiscard]] Vec2 relative(float dx, float dy) noexcept { return {dx, dy}; }

    // A device that reports positions: `x` and `y` in the device's range of
    // 0 to `range` (65535 on Windows) across a screen of `screen` pixels.
    [[nodiscard]] Vec2 absolute(float x, float y, float range, Vec2 screen) noexcept {
        const Vec2 at{x / range * screen.x, y / range * screen.y};
        const Vec2 delta = m_haveLast ? at - m_last : Vec2{};
        m_last = at;
        m_haveLast = true;
        return delta;
    }

private:
    Vec2 m_last;
    bool m_haveLast = false;
};

} // namespace cfw::detail
