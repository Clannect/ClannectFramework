// Running programs: output on both streams, exit codes, arguments passed
// exactly (spaces, quotes, backslashes, no shell), the working directory, a
// timeout, and a program that does not exist.

#include "cfw/io/Process.h"

#include "cfw/io/FileSystem.h"
#include "cfw/io/TemporaryDirectory.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

#ifdef _WIN32

void windows() {
    const std::optional<Path> cmd = findExecutable("cmd");
    check(cmd.has_value(), "cmd.exe is on PATH (found through PATHEXT)");
    if (!cmd) {
        return;
    }
    // cmd.exe parses its own command line, so each command is plain tokens.
    auto exitCode = runProcess(*cmd, {{"/c", "exit", "4"}});
    check(bool(exitCode), "cmd runs");
    if (exitCode) {
        checkEqual(exitCode.value().exitCode, 4, "its exit code");
    }
    auto output = runProcess(*cmd, {{"/c", "echo", "out"}});
    check(output && output.value().standardOutput.find("out") != String::npos, "its output");
    auto error = runProcess(*cmd, {{"/c", "echo", "err", "1>&2"}});
    check(error && error.value().standardError.find("err") != String::npos, "its error output");
    check(!runProcess(Path("C:\\no\\such\\program.exe")), "a missing program fails to start");
}

#else

void outputAndExitCode() {
    const std::optional<Path> sh = findExecutable("sh");
    check(sh.has_value(), "sh is on PATH");
    if (!sh) {
        return;
    }
    auto result = runProcess(*sh, {{"-c", "echo out; echo err 1>&2; exit 3"}});
    check(bool(result), "sh runs");
    checkEqual(result.value().standardOutput, String("out\n"), "standard output");
    checkEqual(result.value().standardError, String("err\n"), "standard error");
    checkEqual(result.value().exitCode, 3, "the exit code");
    check(!result.value().timedOut, "in time");
}

void argumentsPassExactly() {
    const std::optional<Path> printf = findExecutable("printf");
    check(printf.has_value(), "printf is on PATH");
    if (!printf) {
        return;
    }
    auto result = runProcess(*printf, {{"%s|", "a b", "\"quoted\"", "back\\slash", "$HOME", ""}});
    checkEqual(result.value().standardOutput, String("a b|\"quoted\"|back\\slash|$HOME||"),
               "no shell: every argument arrives as it was");
}

void workingDirectory() {
    auto dir = TemporaryDirectory::create();
    check(bool(dir), "a temporary directory");
    const std::optional<Path> pwd = findExecutable("pwd");
    if (!dir || !pwd) {
        return;
    }
    ProcessOptions options;
    options.workingDirectory = dir.value().path();
    auto result = runProcess(*pwd, options);
    const String printed = result.value().standardOutput;
    check(printed.find(dir.value().path().fileName()) != String::npos, "runs in the given directory");
}

void timeouts() {
    const std::optional<Path> sleep = findExecutable("sleep");
    if (!sleep) {
        return;
    }
    ProcessOptions options;
    options.arguments = {"10"};
    options.timeout = std::chrono::milliseconds(200);
    const TimePoint start = Clock::now();
    auto result = runProcess(*sleep, options);
    check(result.value().timedOut, "a program past its timeout is stopped");
    check(Clock::now() - start < std::chrono::seconds(5), "promptly");
}

void failures() {
    check(!runProcess(Path("/no/such/program")), "a missing program fails to start");
    check(!findExecutable("no-such-program-cfw").has_value(), "and is not found on PATH");
}

#endif

} // namespace

int main() {
#ifdef _WIN32
    windows();
#else
    outputAndExitCode();
    argumentsPassExactly();
    workingDirectory();
    timeouts();
    failures();
#endif
    return cfw::test::finish("ProcessTest");
}
