// ChildProcess: collected and streamed output, input, passed descriptors,
// interrupting and stopping. POSIX shells drive it; on Windows the test
// checks starting and stopping only.

#include <cstdio>

#include "cfw/io/ChildProcess.h"
#include "cfw/io/Process.h"
#include "cfw/test/Check.h"

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

int main() {
#ifdef _WIN32
    const auto cmd = findExecutable("cmd");
    check(cmd.has_value(), "cmd.exe is found");
    if (cmd) {
        ChildProcessOptions o;
        o.arguments = {"/c", "echo hello"};
        auto p = ChildProcess::start(*cmd, o);
        check(bool(p), "a program starts");
        if (p) {
            check(p.value()->wait(std::chrono::seconds(10)), "and ends");
            checkEqual(p.value()->exitCode().value_or(-1), 0, "with exit code 0");
        }
    }
#else
    const auto sh = findExecutable("sh");
    check(sh.has_value(), "sh is found");
    if (!sh) {
        return cfw::test::finish("ChildProcessTest");
    }
    {
        // Input in, collected output out.
        ChildProcessOptions o;
        o.arguments = {"-c", "echo start; cat; echo oops >&2; exit 3"};
        o.writableInput = true;
        auto p = std::move(ChildProcess::start(*sh, o)).value();
        check(p->write("one\ntwo\n"), "writing to its input");
        p->closeInput();
        check(p->wait(std::chrono::seconds(5)), "it ends when its input does");
        checkEqual(p->exitCode().value_or(-1), 3, "with its exit code");
        // The collectors may finish a moment after the exit.
        String out, err;
        for (int i = 0; i < 100 && (out.size() < 14 || err.empty()); ++i) {
            out += p->takeStandardOutput();
            err += p->takeStandardError();
            usleep(10000);
        }
        checkEqual(out, String("start\none\ntwo\n"), "its output is collected");
        checkEqual(err, String("oops\n"), "and its errors");
    }
    {
        // Streamed binary output, read in exact pieces.
        ChildProcessOptions o;
        o.arguments = {"-c", "printf 'abcdefgh'"};
        o.standardOutput = ChildProcessOptions::Output::Stream;
        auto p = std::move(ChildProcess::start(*sh, o)).value();
        std::byte buf[4];
        check(p->readExactly(buf), "the first four bytes");
        check(char(buf[0]) == 'a' && char(buf[3]) == 'd', "are abcd");
        check(p->readExactly(buf) && char(buf[0]) == 'e', "then efgh");
        check(!p->readExactly(buf), "then the end");
    }
    {
        // A descriptor passed as 3.
        int pipeFds[2];
        check(::pipe(pipeFds) == 0, "a pipe");
        ChildProcessOptions o;
        o.arguments = {"-c", "echo via-three >&3"};
        o.passDescriptors = {pipeFds[1]};
        auto p = std::move(ChildProcess::start(*sh, o)).value();
        ::close(pipeFds[1]);
        char text[32] = {};
        const ssize_t n = ::read(pipeFds[0], text, sizeof text - 1);
        ::close(pipeFds[0]);
        checkEqual(String(text, n > 0 ? std::size_t(n) : 0), String("via-three\n"), "the child writes to descriptor 3");
        p->wait(std::chrono::seconds(5));
    }
    {
        // Interrupt: SIGINT, which a shell running sleep dies of.
        ChildProcessOptions o;
        o.arguments = {"-c", "sleep 30"};
        auto p = std::move(ChildProcess::start(*sh, o)).value();
        check(p->running(), "a long program runs");
        p->interrupt();
        check(p->wait(std::chrono::seconds(5)), "interrupt stops it");
        checkEqual(p->exitCode().value_or(-1), 128 + 2, "by SIGINT");
    }
    {
        // Destroying a running one stops it.
        ChildProcessOptions o;
        o.arguments = {"-c", "trap '' TERM; sleep 30"};
        auto p = std::move(ChildProcess::start(*sh, o)).value();
        const pid_t pid = pid_t(p->processId());
        p.reset();
        check(::kill(pid, 0) != 0, "destroying it ends the program (killed when it ignores TERM)");
    }
    check(!ChildProcess::start(Path("/nonexistent/program"), {}), "a missing program fails to start");
#endif
    return cfw::test::finish("ChildProcessTest");
}
