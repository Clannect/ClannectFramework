#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "cfw/core/Connection.h"
#include "cfw/core/SignalOwner.h"

// Qt defines `emit` as an empty macro. While CFW and Qt code live side by side
// (the engine's port), this header must still compile after Qt's; code that
// includes both calls sig.emit(...) with Qt's macro off (QT_NO_EMIT) or not at
// all.
#ifdef emit
#pragma push_macro("emit")
#undef emit
#define CFW_SIGNAL_RESTORE_EMIT
#endif

namespace cfw {

// A typed notification with any number of connected slots. Plain C++: no moc,
// no macros. Declare as a public member:
//
//     Signal<Instance &> instanceAdded;
//     auto c = world.instanceAdded.connect([](Instance &i) { ... });
//     world.instanceAdded.emit(part);
//
// Rules (all defined behaviour, all tested):
// - Slots run synchronously, in connection order.
// - A slot connected during an emission is not called by that emission.
// - A slot disconnected during an emission is not called for the rest of it,
//   including by the slot itself disconnecting.
// - A slot may emit the same signal again (nested emission sees the same rules).
// - A slot may destroy the Signal (or its owner); the emission stops after
//   that slot returns and nothing dangles.
// - There is no cross-thread delivery here; cfw-app's event loop posts to a
//   thread, then emits there.
//
// Threads: one thread (the owner's). Not synchronised.
// Allocates: on connect (the slot and, on first connect, the slot list), and
// whatever std::function needs for large captures. emit() does not allocate.
//
// Shared ownership of the slot list is deliberate: Connections, Signals and
// running emissions have independent lifetimes. See
// docs/decisions/0004-signal-slot-lifetime.md.
template <class... Args>
class Signal {
    static_assert((!std::is_rvalue_reference_v<Args> && ...),
                  "every slot sees the same arguments, so they cannot be moved from");

public:
    using Slot = std::function<void(Args...)>;

    Signal() noexcept = default;
    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;
    ~Signal() { disconnectAll(); }

    // Connects `slot`. Keep the returned Connection (or wrap it in a
    // ScopedConnection) to disconnect later.
    [[nodiscard]] Connection connect(Slot slot) {
        if (!m_state) {
            m_state = std::make_shared<State>();
        }
        compactIfIdle(*m_state);
        auto entry = std::make_shared<Entry>();
        entry->slot = std::move(slot);
        Connection connection{std::weak_ptr<detail::SlotState>(entry)};
        m_state->entries.push_back(std::move(entry));
        return connection;
    }

    // Connects `slot` for as long as `owner` lives.
    void connect(SignalOwner &owner, Slot slot) { owner.track(connect(std::move(slot))); }

    void emit(Args... args) const {
        if (!m_state) {
            return;
        }
        // Holding the state keeps the entries alive even if a slot destroys
        // this Signal.
        const std::shared_ptr<State> state = m_state;
        EmitScope scope(*state);
        const std::size_t count = state->entries.size();
        for (std::size_t i = 0; i < count; ++i) {
            // Index each time: a slot may connect and reallocate the vector.
            // Entries are never erased while an emission is running.
            Entry &entry = *state->entries[i];
            if (entry.connected) {
                entry.slot(args...);
            }
        }
    }

    // Number of live connections.
    [[nodiscard]] std::size_t connectionCount() const noexcept {
        std::size_t count = 0;
        if (m_state) {
            for (const auto &entry : m_state->entries) {
                if (entry->connected) {
                    ++count;
                }
            }
        }
        return count;
    }

    void disconnectAll() noexcept {
        if (!m_state) {
            return;
        }
        for (const auto &entry : m_state->entries) {
            entry->connected = false;
        }
        compactIfIdle(*m_state);
        m_state.reset();
    }

private:
    struct Entry : detail::SlotState {
        Slot slot;
    };

    struct State {
        std::vector<std::shared_ptr<Entry>> entries;
        int emitDepth = 0;
    };

    // Tracks nesting and compacts disconnected entries once the outermost
    // emission finishes (also if a slot throws).
    class EmitScope {
    public:
        explicit EmitScope(State &state) noexcept : m_state(state) { ++m_state.emitDepth; }
        ~EmitScope() {
            --m_state.emitDepth;
            compactIfIdle(m_state);
        }
        EmitScope(const EmitScope &) = delete;
        EmitScope &operator=(const EmitScope &) = delete;

    private:
        State &m_state;
    };

    static void compactIfIdle(State &state) noexcept {
        if (state.emitDepth == 0) {
            std::erase_if(state.entries, [](const std::shared_ptr<Entry> &entry) { return !entry->connected; });
        }
    }

    std::shared_ptr<State> m_state; // created on first connect
};

} // namespace cfw

#ifdef CFW_SIGNAL_RESTORE_EMIT
#pragma pop_macro("emit")
#undef CFW_SIGNAL_RESTORE_EMIT
#endif
