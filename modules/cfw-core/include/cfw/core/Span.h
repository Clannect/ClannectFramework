#pragma once

// A non-owning view of contiguous elements. std::span is bounds-checked in
// debug builds through standard-library hardening (_GLIBCXX_ASSERTIONS on
// GCC/Clang with libstdc++, iterator debugging on MSVC): see
// docs/decisions/0003-bounds-checks-via-stdlib-hardening.md.

#include <cstddef>
#include <span>

namespace cfw {

template <class T, std::size_t Extent = std::dynamic_extent>
using Span = std::span<T, Extent>;

} // namespace cfw
