#pragma once

#include <memory>
#include <utility>

namespace cfw {

namespace detail {

// The part of a connected slot that a Connection can see. Owned by the
// Signal; the Connection holds a weak reference, so either side may be
// destroyed first.
struct SlotState {
    bool connected = true;
};

} // namespace detail

// A handle to one Signal connection. Copyable; disconnecting through any copy
// disconnects the slot. Destroying a Connection does NOT disconnect: use
// ScopedConnection or SignalOwner for that.
//
// Threads: the signal's thread only. Allocates: nothing.
class Connection {
public:
    Connection() noexcept = default;
    explicit Connection(std::weak_ptr<detail::SlotState> slot) noexcept : m_slot(std::move(slot)) {}

    // Idempotent. Safe after the signal is gone, and during an emission: a slot
    // disconnected mid-emission is not called for the rest of that emission.
    void disconnect() noexcept {
        if (const auto slot = m_slot.lock()) {
            slot->connected = false;
        }
        m_slot.reset();
    }

    [[nodiscard]] bool connected() const noexcept {
        const auto slot = m_slot.lock();
        return slot && slot->connected;
    }

private:
    std::weak_ptr<detail::SlotState> m_slot;
};

// Disconnects when destroyed. Move-only.
//
// Threads: the signal's thread only. Allocates: nothing.
class ScopedConnection {
public:
    ScopedConnection() noexcept = default;
    ScopedConnection(Connection connection) noexcept : m_connection(std::move(connection)) {}
    ScopedConnection(ScopedConnection &&) noexcept = default;
    ScopedConnection &operator=(ScopedConnection &&other) noexcept {
        if (this != &other) {
            m_connection.disconnect();
            m_connection = std::move(other.m_connection);
            other.m_connection = Connection();
        }
        return *this;
    }
    ScopedConnection(const ScopedConnection &) = delete;
    ScopedConnection &operator=(const ScopedConnection &) = delete;
    ~ScopedConnection() { m_connection.disconnect(); }

    void disconnect() noexcept { m_connection.disconnect(); }
    [[nodiscard]] bool connected() const noexcept { return m_connection.connected(); }

    // Gives up ownership without disconnecting.
    [[nodiscard]] Connection release() noexcept { return std::exchange(m_connection, Connection()); }

private:
    Connection m_connection;
};

} // namespace cfw
