#pragma once

#include <vector>

#include "cfw/core/String.h"

namespace cfw {

// The process's command-line arguments as UTF-8, program name first, for
// CommandLine::parse. On Windows they come from the wide command line (argv
// there is in the ANSI code page, which cannot hold every file name); on
// other platforms argv is used as it is.
[[nodiscard]] std::vector<String> processArguments(int argc, const char *const *argv);

} // namespace cfw
