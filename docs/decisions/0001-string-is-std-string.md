# 0001 — `cfw::String` is `std::string`

**Status:** accepted, 2026-09-27

## Decision

`cfw::String` is an alias for `std::string` and `cfw::StringView` for `std::string_view`. Text is UTF-8 by
convention. It is validated where it enters the process (files, network, OS APIs) using `cfw/core/Utf8.h`,
not on every string operation.

## Rejected

- **A custom small-string-optimised class.** The spec asks for "owning, small-string-optimised". `std::string`
  already is: 15 bytes inline on libstdc++ and MSVC, 22 on libc++. A custom class would cost weeks and carry
  risk, and it would need conversions at every boundary with HarfBuzz, FreeType, JSON, the OS and Lua. It
  would win nothing we have measured.
- **A wrapper class that enforces valid UTF-8.** Validating on every construction costs time on hot paths
  (JSON parsing, property keys), and boundary validation gives the same guarantee.
- **Copy-on-write (like `QString`).** COW makes copies cheap, but it puts atomic reference counting on every
  mutation and makes thread-safety subtle. Hot-path identifiers use `cfw::Name` instead (see 0002).

## What would change this

A §7 benchmark where string copies or allocations dominate, and a string-type change fixes it. The alias
means a later switch is mostly mechanical.
