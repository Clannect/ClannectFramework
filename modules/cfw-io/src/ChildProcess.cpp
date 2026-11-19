#include "cfw/io/ChildProcess.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>

#include "cfw/core/Utf8.h"
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace cfw {

void ChildProcess::Collector::append(const char *data, std::size_t size) {
    std::lock_guard lock(mutex);
    text.append(data, size);
    if (text.size() > limit) {
        text.erase(0, text.size() - limit); // keep the newest
    }
}

String ChildProcess::Collector::take() {
    std::lock_guard lock(mutex);
    String out;
    out.swap(text);
    return out;
}

String ChildProcess::takeStandardOutput() { return m_out.take(); }
String ChildProcess::takeStandardError() { return m_err.take(); }

std::optional<int> ChildProcess::exitCode() {
    reap(false);
    return m_exit;
}

bool ChildProcess::running() {
    reap(false);
    return !m_exit.has_value();
}

bool ChildProcess::wait(Duration timeout) {
    const TimePoint deadline = Clock::now() + timeout;
    while (running()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

#ifdef _WIN32

namespace {

std::u16string quote(const std::u16string &argument) {
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
            out.append(backslashes * 2, u'\\');
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

// Every child is in this job, so they end with the application.
HANDLE job() {
    static HANDLE j = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof info);
        }
        return h;
    }();
    return j;
}

HANDLE nullDevice(bool write) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    return CreateFileW(L"NUL", write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                       OPEN_EXISTING, 0, nullptr);
}

} // namespace

Result<std::unique_ptr<ChildProcess>> ChildProcess::start(const Path &program, const ChildProcessOptions &options) {
    std::filesystem::path native = program.native();
    native.make_preferred();
    const std::wstring nativeText = native.wstring();
    const std::u16string programText(nativeText.begin(), nativeText.end());
    std::u16string commandLine = quote(programText);
    for (const String &argument : options.arguments) {
        const auto wide = utf8ToUtf16(argument);
        if (!wide) {
            return wide.error();
        }
        commandLine += u' ';
        commandLine += quote(wide.value());
    }
    std::u16string directory;
    if (!options.workingDirectory.empty()) {
        std::filesystem::path d = options.workingDirectory.native();
        d.make_preferred();
        const std::wstring w = d.wstring();
        directory.assign(w.begin(), w.end());
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    using Output = ChildProcessOptions::Output;
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr,
           errWrite = nullptr;
    if (options.writableInput) {
        if (!CreatePipe(&inRead, &inWrite, &inherit, 0)) {
            return Error(ErrorCode::IoError, "could not create a pipe");
        }
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    } else {
        inRead = nullDevice(false);
    }
    auto makeOut = [&](Output mode, HANDLE &read, HANDLE &write) {
        if (mode == Output::Discard) {
            write = nullDevice(true);
            return true;
        }
        if (!CreatePipe(&read, &write, &inherit, 1 << 20)) {
            return false;
        }
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        return true;
    };
    if (!makeOut(options.standardOutput, outRead, outWrite) || !makeOut(options.standardError, errRead, errWrite)) {
        return Error(ErrorCode::IoError, "could not create a pipe");
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inRead;
    startup.hStdOutput = outWrite;
    startup.hStdError = errWrite;
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> line(commandLine.begin(), commandLine.end());
    line.push_back(0);
    const BOOL started = CreateProcessW(reinterpret_cast<const wchar_t *>(programText.c_str()), line.data(), nullptr,
                                        nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                        directory.empty() ? nullptr : reinterpret_cast<const wchar_t *>(directory.c_str()),
                                        &startup, &info);
    for (HANDLE h : {inRead, outWrite, errWrite}) {
        if (h && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
    if (!started) {
        for (HANDLE h : {inWrite, outRead, errRead}) {
            if (h) {
                CloseHandle(h);
            }
        }
        return Error(ErrorCode::NotFound, "could not start " + program.toString());
    }
    if (HANDLE j = job()) {
        AssignProcessToJobObject(j, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    std::unique_ptr<ChildProcess> p(new ChildProcess());
    p->m_pid = std::int64_t(info.dwProcessId);
    p->m_process = info.hProcess;
    p->m_input = inWrite;
    p->m_out.limit = p->m_err.limit = options.maxCollected;
    auto collect = [](HANDLE pipe, Collector &into) {
        return std::thread([pipe, &into] {
            char buffer[8192];
            DWORD n = 0;
            while (ReadFile(pipe, buffer, sizeof buffer, &n, nullptr) && n > 0) {
                into.append(buffer, n);
            }
            CloseHandle(pipe);
        });
    };
    if (options.standardOutput == Output::Collect) {
        p->m_outThread = collect(outRead, p->m_out);
    } else if (options.standardOutput == Output::Stream) {
        p->m_stream = outRead;
    }
    if (errRead) {
        p->m_errThread = collect(errRead, p->m_err);
    }
    return p;
}

void ChildProcess::reap(bool block) {
    if (m_exit || !m_process) {
        return;
    }
    if (WaitForSingleObject(m_process, block ? INFINITE : 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(m_process, &code);
        m_exit = int(code);
    }
}

bool ChildProcess::write(StringView data) {
    if (!m_input) {
        return false;
    }
    std::size_t done = 0;
    while (done < data.size()) {
        DWORD n = 0;
        if (!WriteFile(m_input, data.data() + done, DWORD(data.size() - done), &n, nullptr)) {
            return false;
        }
        done += n;
    }
    return true;
}

void ChildProcess::closeInput() {
    if (m_input) {
        CloseHandle(m_input);
        m_input = nullptr;
    }
}

bool ChildProcess::readExactly(Span<std::byte> buffer) {
    if (!m_stream) {
        return false;
    }
    std::size_t done = 0;
    while (done < buffer.size()) {
        DWORD n = 0;
        if (!ReadFile(m_stream, buffer.data() + done, DWORD(buffer.size() - done), &n, nullptr) || n == 0) {
            return false;
        }
        done += n;
    }
    return true;
}

void ChildProcess::interrupt() {
    if (m_input) {
        closeInput();
    } else {
        terminate();
    }
}

void ChildProcess::terminate() {
    if (m_process && running()) {
        TerminateProcess(m_process, 1);
    }
}

void ChildProcess::kill() { terminate(); }

ChildProcess::~ChildProcess() {
    if (running()) {
        terminate();
    }
    reap(true);
    closeInput();
    if (m_stream) {
        CloseHandle(m_stream);
    }
    if (m_outThread.joinable()) {
        m_outThread.join();
    }
    if (m_errThread.joinable()) {
        m_errThread.join();
    }
    if (m_process) {
        CloseHandle(m_process);
    }
}

#else

Result<std::unique_ptr<ChildProcess>> ChildProcess::start(const Path &program, const ChildProcessOptions &options) {
    using Output = ChildProcessOptions::Output;
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, status[2] = {-1, -1};
    auto closeAll = [&] {
        for (int *p : {in, out, err, status}) {
            for (int i = 0; i < 2; ++i) {
                if (p[i] >= 0) {
                    ::close(p[i]);
                    p[i] = -1;
                }
            }
        }
    };
    if ((options.writableInput && ::pipe(in) != 0) ||
        (options.standardOutput != Output::Discard && ::pipe(out) != 0) ||
        (options.standardError != Output::Discard && ::pipe(err) != 0) || ::pipe(status) != 0) {
        closeAll();
        return Error(ErrorCode::IoError, "could not create pipes");
    }
    for (int fd : {in[1], out[0], err[0], status[0], status[1]}) {
        if (fd >= 0) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
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
    const std::vector<int> pass = options.passDescriptors;
    const pid_t parent = ::getpid();

    const pid_t pid = ::fork();
    if (pid < 0) {
        closeAll();
        return Error(ErrorCode::IoError, "could not start " + programText);
    }
    if (pid == 0) {
#ifdef __linux__
        // Ends with the application, whatever ends it.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent) {
            ::_exit(127);
        }
#endif
        // Its own process group: a Ctrl+C in the terminal goes to the
        // application, which decides how to stop its children.
        ::setpgid(0, 0);
        const int devNull = ::open("/dev/null", O_RDWR);
        ::dup2(in[0] >= 0 ? in[0] : devNull, 0);
        ::dup2(out[1] >= 0 ? out[1] : devNull, 1);
        ::dup2(err[1] >= 0 ? err[1] : devNull, 2);
        // Passed descriptors to 3, 4, ...: first out of the way, then in place.
        int high[16];
        const std::size_t count = std::min<std::size_t>(pass.size(), 16);
        for (std::size_t i = 0; i < count; ++i) {
            high[i] = ::fcntl(pass[i], F_DUPFD, 64);
        }
        for (std::size_t i = 0; i < count; ++i) {
            ::dup2(high[i], 3 + int(i));
        }
        // Nothing else of ours leaks into it.
        for (int fd = 3 + int(count); fd < 1024; ++fd) {
            if (fd != status[1]) {
                ::close(fd);
            }
        }
        // SIGINT and SIGTERM as the default (the application may ignore them).
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGPIPE, SIG_DFL);
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
    for (int *fd : {&in[0], &out[1], &err[1], &status[1]}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    int execError = 0;
    const ssize_t got = ::read(status[0], &execError, sizeof execError);
    ::close(status[0]);
    status[0] = -1;
    if (got == ssize_t(sizeof execError)) {
        closeAll();
        ::waitpid(pid, nullptr, 0);
        return Error(ErrorCode::NotFound, "could not start " + programText + ": " + std::strerror(execError));
    }
    std::unique_ptr<ChildProcess> p(new ChildProcess());
    p->m_pid = pid;
    p->m_input = in[1];
    p->m_out.limit = p->m_err.limit = options.maxCollected;
    auto collect = [](int fd, Collector &into) {
        return std::thread([fd, &into] {
            char buffer[8192];
            for (;;) {
                const ssize_t n = ::read(fd, buffer, sizeof buffer);
                if (n > 0) {
                    into.append(buffer, std::size_t(n));
                } else if (n == 0 || errno != EINTR) {
                    break;
                }
            }
            ::close(fd);
        });
    };
    if (options.standardOutput == Output::Collect) {
        p->m_outThread = collect(out[0], p->m_out);
    } else if (options.standardOutput == Output::Stream) {
        p->m_stream = out[0];
    }
    if (err[0] >= 0) {
        p->m_errThread = collect(err[0], p->m_err);
    }
    return p;
}

void ChildProcess::reap(bool block) {
    if (m_exit || m_pid <= 0) {
        return;
    }
    int status = 0;
    pid_t r;
    while ((r = ::waitpid(pid_t(m_pid), &status, block ? 0 : WNOHANG)) < 0 && errno == EINTR) {
    }
    if (r == pid_t(m_pid)) {
        m_exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    } else if (r < 0) {
        m_exit = -1; // not ours any more
    }
}

bool ChildProcess::write(StringView data) {
    if (m_input < 0) {
        return false;
    }
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(m_input, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += std::size_t(n);
    }
    return true;
}

void ChildProcess::closeInput() {
    if (m_input >= 0) {
        ::close(m_input);
        m_input = -1;
    }
}

bool ChildProcess::readExactly(Span<std::byte> buffer) {
    if (m_stream < 0) {
        return false;
    }
    std::size_t done = 0;
    while (done < buffer.size()) {
        const ssize_t n = ::read(m_stream, buffer.data() + done, buffer.size() - done);
        if (n > 0) {
            done += std::size_t(n);
        } else if (n == 0 || errno != EINTR) {
            return false;
        }
    }
    return true;
}

void ChildProcess::interrupt() {
    if (running()) {
        ::kill(pid_t(m_pid), SIGINT);
    }
}

void ChildProcess::terminate() {
    if (running()) {
        ::kill(pid_t(m_pid), SIGTERM);
    }
}

void ChildProcess::kill() {
    if (running()) {
        ::kill(pid_t(m_pid), SIGKILL);
    }
}

ChildProcess::~ChildProcess() {
    closeInput();
    if (running()) {
        terminate();
        if (!wait(std::chrono::seconds(1))) {
            kill();
        }
    }
    reap(true);
    if (m_stream >= 0) {
        ::close(m_stream);
    }
    if (m_outThread.joinable()) {
        m_outThread.join();
    }
    if (m_errThread.joinable()) {
        m_errThread.join();
    }
}

#endif

} // namespace cfw
