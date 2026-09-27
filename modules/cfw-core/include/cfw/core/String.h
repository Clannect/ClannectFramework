#pragma once

// CFW's string types. Text is UTF-8 everywhere; UTF-16 exists only at the OS
// boundary (see Utf8.h). cfw::String is std::string: it already has a
// small-string buffer and every library we will ever link speaks it. The
// reasoning is in docs/decisions/0001-string-is-std-string.md.
//
// Validity: a String is expected to hold valid UTF-8. Text arriving from
// outside (files, network, OS) is validated where it enters, not on every
// operation.

#include <string>
#include <string_view>

namespace cfw {

using String = std::string;
using StringView = std::string_view;

} // namespace cfw
