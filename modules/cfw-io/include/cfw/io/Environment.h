#pragma once

#include <optional>

#include "cfw/core/String.h"

namespace cfw {

// An environment variable's value as UTF-8, or nothing if it is not set
// (qEnvironmentVariable). On Windows it is read from the wide environment,
// so any value survives, whatever the ANSI code page.
[[nodiscard]] std::optional<String> environmentVariable(StringView name);

// Sets a variable for this process and the ones it starts (qputenv). False if
// the name is empty or contains '='.
bool setEnvironmentVariable(StringView name, StringView value);

} // namespace cfw
