#pragma once

// Running another program and collecting what it prints (QProcess with
// waitForFinished): the arguments are passed as they are, with no shell in
// between, so nothing in them is interpreted.
//
// Threads: any; runProcess blocks the calling thread until the program ends
// or the timeout passes. Allocates: the output.

#include <optional>
#include <vector>

#include "cfw/core/Clock.h"
#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"

namespace cfw {

struct ProcessOptions {
    std::vector<String> arguments;  // after the program itself
    Path workingDirectory;          // empty: this process's
    Duration timeout = std::chrono::seconds(30);
    // Output beyond this is dropped (the program still runs to the end).
    std::size_t maxOutput = std::size_t(16) << 20;
};

struct ProcessResult {
    int exitCode = -1;       // meaningless when timedOut
    bool timedOut = false;   // the program was killed at the timeout
    String standardOutput;
    String standardError;
};

// Starts `program` (a path; see findExecutable) and waits for it. Fails only
// when it could not be started; a program that fails reports it in exitCode.
[[nodiscard]] Result<ProcessResult> runProcess(const Path &program, const ProcessOptions &options = {});

// The first `name` on PATH that is an executable file (on Windows also
// trying PATHEXT's extensions when `name` has none), or nothing.
[[nodiscard]] std::optional<Path> findExecutable(StringView name);

} // namespace cfw
