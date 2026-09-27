#pragma once

#include <vector>

#include "cfw/core/Connection.h"

namespace cfw {

// Holds connections for an object that receives signals and disconnects them
// all when the object is destroyed, so a signal never calls into a dead
// receiver. Use as a base or a member:
//
//     class Explorer : public SignalOwner {
//         Explorer(World &world) {
//             world.instanceAdded.connect(*this, [this](Instance &i) { addRow(i); });
//         }
//     };
//
// Destruction order: as a base class, SignalOwner disconnects after the
// derived destructor has run. If a slot could fire while the derived part is
// being torn down, call disconnectAll() first thing in the derived destructor.
//
// Threads: the owning object's thread only. Allocates: one vector entry per
// tracked connection.
class SignalOwner {
public:
    SignalOwner() = default;
    SignalOwner(const SignalOwner &) = delete;
    SignalOwner &operator=(const SignalOwner &) = delete;

    void track(Connection connection) { m_connections.emplace_back(std::move(connection)); }
    void disconnectAll() noexcept { m_connections.clear(); }
    [[nodiscard]] std::size_t trackedCount() const noexcept { return m_connections.size(); }

protected:
    ~SignalOwner() = default;

private:
    std::vector<ScopedConnection> m_connections;
};

} // namespace cfw
