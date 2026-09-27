# 0004 — Signals share ownership of their slot list

**Status:** accepted, 2026-09-27

## Decision

§5 forbids shared ownership "without a written reason". This is the reason for `cfw::Signal`.

A signal's slot list is owned by a `std::shared_ptr`, and each `Connection` holds a `std::weak_ptr` to its
slot. Three things have independent lifetimes, and any of them can end first:

1. **The Signal.** It can be destroyed by one of its own slots mid-emission, for example a slot that closes
   the panel owning the signal.
2. **Connections.** They can outlive the signal (`Connection::disconnect()` after the sender is gone must be a
   no-op, not a use-after-free).
3. **A running emission.** It must keep iterating valid memory even if the signal is destroyed under it.

The emission holds its own `shared_ptr` to the slot list for its duration. Disconnection only flags a slot.
Flagged slots are erased once no emission is running. The alternatives were:

- **Raw back-pointers with manual unregistering (Qt's approach).** This needs every object to know every
  connection it is part of, so it is a larger object graph with more ways to dangle.
- **Deferring signal destruction to the event loop.** That is not available in cfw-core, and it makes
  teardown nondeterministic (constraint 7).

## Costs

- One allocation per signal, on first connect only. An unconnected signal costs one null pointer.
- One allocation per connected slot.
- One atomic increment and decrement per `emit()` (copying the state pointer), none per slot.
- `emit()` itself never allocates.

Signals are single-thread objects, so the atomics are never contended.
