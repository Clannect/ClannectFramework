#include "cfw/core/Contract.h"

#include <cstdio>
#include <cstdlib>

namespace cfw {

void contractViolation(const char *what, std::source_location where) noexcept {
    std::fprintf(stderr, "cfw: contract violated: %s\n  at %s:%u (%s)\n", what, where.file_name(),
                 static_cast<unsigned>(where.line()), where.function_name());
    std::fflush(stderr);
    std::abort();
}

} // namespace cfw
