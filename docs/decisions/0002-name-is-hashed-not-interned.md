# 0002 — `cfw::Name` is hashed, not interned

**Status:** accepted, 2026-09-27

## Decision

The requirement was an "interned `cfw::Name`". Interning means a table where each distinct string is stored once,
and names compare by pointer. That table is process-wide mutable state, which CFW's rules forbid ("no global
mutable state… a process may create two independent `cfw::App` instances"). A thread-safe interning table also
costs a lock or an atomic on every runtime name creation.

Instead, a `Name` carries its 64-bit FNV-1a hash, computed once:

- **Literal names** (`constexpr Name kPosition = "Position";`) hash at compile time and point at the literal.
  There is no allocation and no static initialisation. They are the common case: every property key the
  engine declares.
- **Runtime names** (`Name(StringView)`) copy the text into a buffer they own (through `std::allocator`).
- **Equality** compares the hash, then the text, so a collision can never make two different names equal.
  Maps look names up by hash with no allocation, which meets the "O(1), no allocation" requirement for
  property access.

`Name` is 24 bytes. The owned buffer is not small-string-optimised. An earlier draft held a `std::string` to
get SSO, but libstdc++ cannot place a `std::string` in a `constexpr` object, because its SSO pointer points
into itself. That would have cost compile-time names, which matter more. The draft was also 56 bytes, which
bloated `Variant`.

## Rejected

- Interning with a table per `cfw::App`: every `Name` would need a context pointer, and names could not be
  `constexpr`.
- Interning with a global table: forbidden by the no-global-state rule.

## What would change this

A profile showing runtime-name allocations are hot, for example a loader that builds a `Name` per property
per instance. The first fix is to look keys up in the property registry's literal names, not to change
`Name`.
