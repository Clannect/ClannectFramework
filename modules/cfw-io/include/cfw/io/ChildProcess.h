#pragma once

// A program running beside this one (QProcess without an event loop): it is
// started, keeps running while the caller does other things, can be written
// to and read from, interrupted and stopped. runProcess (Process.h) is the
// blocking "run and collect" case; this is the long-running one: encoders,
// recorders, decoders streaming frames.
//
//     ChildProcessOptions o;
//     o.arguments = {"-i", "in.mkv", "-f", "rawvideo", "-pix_fmt", "rgba", "-"};
//     o.standardOutput = ChildProcessOptions::Output::Stream;
//     auto ff = ChildProcess::start(*findExecutable("ffmpeg"), o).value();
//     std::vector<std::byte> frame(w * h * 4);
//     while (ff->readExactly(frame)) { ... }   // on a thread of its own
//
// Output is discarded, collected in the background (takeStandardOutput()
// returns what arrived since the last call; a capped amount is kept), or
// left to the caller to read (Stream; reads block, so a worker thread does
// them). On POSIX, open descriptors can be handed to the child as 3, 4, ...
// (a PipeWire remote from the screen-cast portal, for one).
//
// Destroying a ChildProcess that still runs stops it (terminate, then kill
// after a second) and waits for it.
//
// Threads: start, write and the signal calls from one thread; readExactly
// from one other thread at a time; the take* calls from any thread.

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "cfw/core/Clock.h"
#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"

namespace cfw {

struct ChildProcessOptions {
    enum class Output : std::uint8_t { Discard, Collect, Stream };
    std::vector<String> arguments; // after the program itself
    Path workingDirectory;         // empty: this process's
    bool writableInput = false;    // a pipe to its standard input (else nothing)
    Output standardOutput = Output::Collect;
    Output standardError = Output::Collect; // Stream is treated as Collect
    std::size_t maxCollected = std::size_t(1) << 20; // per stream, oldest dropped first
    // POSIX: descriptors the child gets as 3, 4, ... in this order (the
    // caller keeps and closes its own). Ignored on Windows.
    std::vector<int> passDescriptors;
};

class ChildProcess {
public:
    // Fails when the program could not be started.
    [[nodiscard]] static Result<std::unique_ptr<ChildProcess>> start(const Path &program,
                                                                     const ChildProcessOptions &options);
    ~ChildProcess();
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    // Whether it still runs (collects its exit status when it has ended).
    [[nodiscard]] bool running();
    // Its exit code once it has ended (128 + signal on POSIX when killed).
    [[nodiscard]] std::optional<int> exitCode();
    // Waits up to `timeout` for it to end; true when it has.
    bool wait(Duration timeout);
    [[nodiscard]] std::int64_t processId() const noexcept { return m_pid; }

    // ---- Input (writableInput) ----
    bool write(StringView data);
    void closeInput();

    // ---- Output ----
    // Collected output since the last call.
    [[nodiscard]] String takeStandardOutput();
    [[nodiscard]] String takeStandardError();
    // Stream: reads exactly buffer.size() bytes (blocking); false at the end
    // of the stream (the program ended or closed it) or on an error.
    bool readExactly(Span<std::byte> buffer);

    // ---- Stopping ----
    // Asks it to finish cleanly: SIGINT on POSIX (what Ctrl+C sends; encoders
    // then close their files); on Windows, closes its input (ffmpeg and most
    // tools that read commands stop at the end of input) or terminates.
    void interrupt();
    // SIGTERM / TerminateProcess.
    void terminate();
    // SIGKILL / TerminateProcess.
    void kill();

private:
    ChildProcess() = default;
    struct Collector {
        std::mutex mutex;
        String text;
        std::size_t limit = 0;
        void append(const char *data, std::size_t size);
        String take();
    };
    void reap(bool block);

    std::int64_t m_pid = 0;
    std::optional<int> m_exit;
    Collector m_out, m_err;
    std::thread m_outThread, m_errThread;
#ifdef _WIN32
    void *m_process = nullptr;
    void *m_input = nullptr;
    void *m_stream = nullptr; // Stream mode stdout
#else
    int m_input = -1;
    int m_stream = -1;
#endif
};

} // namespace cfw
