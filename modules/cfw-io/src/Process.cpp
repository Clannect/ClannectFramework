#include "cfw/io/Process.h"

#include <algorithm>
#include <cstring>

#include "cfw/io/Environment.h"
#include "cfw/io/FileSystem.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <thread>

#include "cfw/core/Utf8.h"
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cfw {

namespace {

void appendCapped(String &out, const char *data, std::size_t size, std::size_t limit) {
    if (out.size() < limit) {
        out.append(data, std::min(size, limit - out.size()));
    }
}

std::vector<String> splitPath(StringView text, char separator) {
    std::vector<String> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(separator, start), text.size());
        if (end > start) {
            parts.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

} // namespace

#ifdef _WIN32

namespace {

// One argument quoted the way CommandLineToArgvW (and the C runtime) splits
// it back: backslashes are literal except before a quote.
std::u16string quoteArgument(const std::u16string &argument) {
    if (!argument.empty() && argument.find_first_of(u" \t\n\v\"") == std::u16string::npos) {
        return argument;
    }
    std::u16string out = u"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < argument.size() && argument[i] == u'\\') {
            ++i;
            ++backslashes;
        }
        if (i == argument.size()) {
            out.append(backslashes * 2, u'\\'); // before the closing quote
            break;
        }
        if (argument[i] == u'"') {
            out.append(backslashes * 2 + 1, u'\\');
            out += u'"';
        } else {
            out.append(backslashes, u'\\');
            out += argument[i];
        }
    }
    out += u'"';
    return out;
}

// Reads a pipe to its end on a thread of its own (two pipes read in turn
// can deadlock when the child fills the other one).
std::thread drain(HANDLE pipe, String &out, std::size_t limit) {
    return std::thread([pipe, &out, limit] {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(pipe, buffer, sizeof buffer, &read, nullptr) && read > 0) {
            appendCapped(out, buffer, read, limit);
        }
    });
}

} // namespace

Result<ProcessResult> runProcess(const Path &program, const ProcessOptions &options) {
    // Native separators: cmd.exe, for one, reads "C:/x/y" as switches.
    std::filesystem::path native = program.native();
    native.make_preferred();
    const std::wstring nativeText = native.wstring();
    const Result<std::u16string> programText(std::u16string(nativeText.begin(), nativeText.end()));
    std::u16string commandLine = quoteArgument(programText.value());
    for (const String &argument : options.arguments) {
        const auto wide = utf8ToUtf16(argument);
        if (!wide) {
            return wide.error();
        }
        commandLine += u' ';
        commandLine += quoteArgument(wide.value());
    }
    std::u16string directory;
    if (!options.workingDirectory.empty()) {
        std::filesystem::path nativeDirectory = options.workingDirectory.native();
        nativeDirectory.make_preferred();
        const std::wstring wide = nativeDirectory.wstring();
        directory.assign(wide.begin(), wide.end());
    }

    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &inherit, 0) || !CreatePipe(&errRead, &errWrite, &inherit, 0)) {
        return Error(ErrorCode::IoError, "could not create pipes");
    }
    // Only the child's ends are inherited.
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = outWrite;
    startup.hStdError = errWrite;
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back(0);
    const BOOL started =
        CreateProcessW(reinterpret_cast<const wchar_t *>(programText.value().c_str()), mutableLine.data(), nullptr,
                       nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       directory.empty() ? nullptr : reinterpret_cast<const wchar_t *>(directory.c_str()),
                       &startup, &info);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (!started) {
        CloseHandle(outRead);
        CloseHandle(errRead);
        return Error(ErrorCode::NotFound, "could not start " + program.toString());
    }

    ProcessResult result;
    std::thread outThread = drain(outRead, result.standardOutput, options.maxOutput);
    std::thread errThread = drain(errRead, result.standardError, options.maxOutput);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(options.timeout).count();
    if (WaitForSingleObject(info.hProcess, DWORD(std::clamp<long long>(millis, 0, 0x7ffffffe))) == WAIT_TIMEOUT) {
        TerminateProcess(info.hProcess, 1);
        WaitForSingleObject(info.hProcess, INFINITE);
        result.timedOut = true;
    }
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    result.exitCode = int(code);
    outThread.join();
    errThread.join();
    CloseHandle(outRead);
    CloseHandle(errRead);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return result;
}

std::optional<Path> findExecutable(StringView name) {
    if (name.empty()) {
        return std::nullopt;
    }
    const String pathVariable = environmentVariable("PATH").value_or(String());
    std::vector<String> extensions{""};
    if (Path(name).extension().empty()) {
        for (const String &ext : splitPath(environmentVariable("PATHEXT").value_or(".COM;.EXE;.BAT;.CMD"), ';')) {
            extensions.push_back(ext);
        }
    }
    for (const String &directory : splitPath(pathVariable, ';')) {
        for (const String &ext : extensions) {
            const Path candidate = Path(directory) / (String(name) + ext);
            if (isFile(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

#else

Result<ProcessResult> runProcess(const Path &program, const ProcessOptions &options) {
    int out[2] = {-1, -1};
    int err[2] = {-1, -1};
    if (::pipe(out) != 0 || ::pipe(err) != 0) {
        return Error(ErrorCode::IoError, "could not create pipes");
    }
    // Everything the child needs, prepared before fork (only async-signal-safe
    // calls may follow it in the child).
    const String programText = program.toString();
    const String directory = options.workingDirectory.empty() ? String() : options.workingDirectory.toString();
    std::vector<String> strings;
    strings.push_back(programText);
    strings.insert(strings.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char *> argv;
    for (String &s : strings) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    // A pipe that closes on exec tells the parent whether exec worked.
    int status[2] = {-1, -1};
    if (::pipe(status) != 0) {
        return Error(ErrorCode::IoError, "could not create pipes");
    }
    ::fcntl(status[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return Error(ErrorCode::IoError, "could not start " + programText);
    }
    if (pid == 0) {
        ::close(out[0]);
        ::close(err[0]);
        ::close(status[0]);
        ::dup2(out[1], 1);
        ::dup2(err[1], 2);
        if (!directory.empty() && ::chdir(directory.c_str()) != 0) {
            const int e = errno;
            [[maybe_unused]] const ssize_t w = ::write(status[1], &e, sizeof e);
            ::_exit(127);
        }
        ::execv(programText.c_str(), argv.data());
        const int e = errno;
        [[maybe_unused]] const ssize_t w = ::write(status[1], &e, sizeof e);
        ::_exit(127);
    }
    ::close(out[1]);
    ::close(err[1]);
    ::close(status[1]);
    int execError = 0;
    const ssize_t got = ::read(status[0], &execError, sizeof execError);
    ::close(status[0]);
    if (got == ssize_t(sizeof execError)) {
        ::close(out[0]);
        ::close(err[0]);
        ::waitpid(pid, nullptr, 0);
        return Error(ErrorCode::NotFound, "could not start " + programText + ": " + std::strerror(execError));
    }

    ProcessResult result;
    const TimePoint deadline = Clock::now() + options.timeout;
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    int open = 2;
    char buffer[4096];
    while (open > 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            ::kill(pid, SIGKILL);
            result.timedOut = true;
            break;
        }
        const int ready = ::poll(fds, 2, int(std::min<long long>(left, 1000)));
        if (ready < 0 && errno != EINTR) {
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            const ssize_t n = ::read(fds[i].fd, buffer, sizeof buffer);
            if (n > 0) {
                appendCapped(i == 0 ? result.standardOutput : result.standardError, buffer, std::size_t(n),
                             options.maxOutput);
            } else if (n == 0 || errno != EINTR) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                --open;
            }
        }
    }
    for (pollfd &fd : fds) {
        if (fd.fd >= 0) {
            ::close(fd.fd);
        }
    }
    int waitStatus = 0;
    while (::waitpid(pid, &waitStatus, 0) < 0 && errno == EINTR) {
    }
    if (!result.timedOut) {
        result.exitCode = WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : 128 + WTERMSIG(waitStatus);
    }
    return result;
}

std::optional<Path> findExecutable(StringView name) {
    if (name.empty()) {
        return std::nullopt;
    }
    if (name.find('/') != StringView::npos) {
        const String path(name);
        return ::access(path.c_str(), X_OK) == 0 ? std::optional<Path>(Path(path)) : std::nullopt;
    }
    for (const String &directory : splitPath(environmentVariable("PATH").value_or(String()), ':')) {
        const Path candidate = Path(directory) / String(name);
        const String text = candidate.toString();
        if (isFile(candidate) && ::access(text.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

#endif

} // namespace cfw
