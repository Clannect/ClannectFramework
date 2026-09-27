// Signal: ordering, disconnection by every route, and the reentrancy rules the
// header promises (connect/disconnect/emit/destroy from inside a slot).

#include "cfw/core/Signal.h"

#include <memory>
#include <vector>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void slotsRunInConnectionOrder() {
    Signal<int> signal;
    std::vector<int> calls;
    auto a = signal.connect([&](int v) { calls.push_back(v * 1); });
    auto b = signal.connect([&](int v) { calls.push_back(v * 10); });
    signal.emit(2);
    check(calls == std::vector<int>{2, 20}, "both slots, in order");
    checkEqual(signal.connectionCount(), std::size_t(2), "two connections");
}

void disconnectStopsDelivery() {
    Signal<> signal;
    int calls = 0;
    Connection c = signal.connect([&] { ++calls; });
    Connection copy = c;
    copy.disconnect();
    check(!c.connected(), "disconnecting a copy disconnects the slot");
    signal.emit();
    checkEqual(calls, 0, "disconnected slot is not called");
    checkEqual(signal.connectionCount(), std::size_t(0), "no live connections");
    c.disconnect(); // idempotent
}

void scopedConnectionDisconnectsOnDestruction() {
    Signal<> signal;
    int calls = 0;
    {
        ScopedConnection scoped = signal.connect([&] { ++calls; });
        signal.emit();
    }
    signal.emit();
    checkEqual(calls, 1, "called only while the ScopedConnection lived");
}

struct Receiver : SignalOwner {
    int calls = 0;
};

void signalOwnerDisconnectsOnDestruction() {
    Signal<> signal;
    int *counter = nullptr;
    {
        Receiver receiver;
        counter = &receiver.calls;
        signal.connect(receiver, [&receiver] { ++receiver.calls; });
        signal.emit();
        checkEqual(*counter, 1, "owner receives while alive");
    }
    signal.emit(); // would touch a dead receiver if the connection survived
    checkEqual(signal.connectionCount(), std::size_t(0), "owner's connections are gone");
}

void connectionOutlivesSignal() {
    Connection c;
    {
        Signal<> signal;
        c = signal.connect([] {});
    }
    check(!c.connected(), "a connection to a destroyed signal reports disconnected");
    c.disconnect(); // must not touch freed memory
}

void slotConnectedDuringEmissionWaitsForNextEmission() {
    Signal<> signal;
    int late = 0;
    std::vector<Connection> keep;
    keep.push_back(signal.connect([&] {
        if (keep.size() < 50) { // force reallocations of the slot list mid-emission
            keep.push_back(signal.connect([&] { ++late; }));
        }
    }));
    signal.emit();
    checkEqual(late, 0, "slots connected mid-emission are not called by it");
    signal.emit();
    checkEqual(late, 1, "they are called by the next emission");
}

void slotDisconnectedDuringEmissionIsSkipped() {
    Signal<> signal;
    int secondCalls = 0;
    Connection second;
    auto first = signal.connect([&] { second.disconnect(); });
    second = signal.connect([&] { ++secondCalls; });
    signal.emit();
    checkEqual(secondCalls, 0, "a slot disconnected earlier in the same emission is skipped");
}

void slotMayDisconnectItself() {
    Signal<> signal;
    int calls = 0;
    Connection self;
    self = signal.connect([&] {
        ++calls;
        self.disconnect();
    });
    signal.emit();
    signal.emit();
    checkEqual(calls, 1, "self-disconnect takes effect");
}

void nestedEmissionIsDefined() {
    Signal<int> signal;
    std::vector<int> seen;
    auto c = signal.connect([&](int depth) {
        seen.push_back(depth);
        if (depth < 3) {
            signal.emit(depth + 1);
        }
    });
    signal.emit(0);
    check(seen == std::vector<int>{0, 1, 2, 3}, "re-emitting from a slot recurses in order");
}

void slotMayDestroyTheSignal() {
    auto signal = std::make_unique<Signal<>>();
    int after = 0;
    auto a = signal->connect([&] { signal.reset(); });
    auto b = signal->connect([&] { ++after; });
    signal->emit();
    check(signal == nullptr, "the signal was destroyed by its own slot");
    checkEqual(after, 0, "slots after the destroying slot do not run");
    check(!b.connected(), "their connections report disconnected");
}

void emitWithNoConnectionsIsCheap() {
    Signal<const std::vector<int> &> signal;
    signal.emit({1, 2, 3}); // never connected: no state allocated, nothing happens
    checkEqual(signal.connectionCount(), std::size_t(0), "still empty");
}

} // namespace

int main() {
    slotsRunInConnectionOrder();
    disconnectStopsDelivery();
    scopedConnectionDisconnectsOnDestruction();
    signalOwnerDisconnectsOnDestruction();
    connectionOutlivesSignal();
    slotConnectedDuringEmissionWaitsForNextEmission();
    slotDisconnectedDuringEmissionIsSkipped();
    slotMayDisconnectItself();
    nestedEmissionIsDefined();
    slotMayDestroyTheSignal();
    emitWithNoConnectionsIsCheap();
    return cfw::test::finish("SignalTest");
}
