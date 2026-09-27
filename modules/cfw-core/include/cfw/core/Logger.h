#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "cfw/core/FlatMap.h"
#include "cfw/core/Name.h"
#include "cfw/core/String.h"

namespace cfw {

enum class LogLevel : std::uint8_t { Trace = 0, Debug = 1, Info = 2, Warning = 3, Error = 4, Off = 5 };

[[nodiscard]] const char *logLevelName(LogLevel level) noexcept;

struct LogField {
    String key;
    String value;
};

// One structured log entry. Fields carry machine-readable context
// ("path" = "...", "bytes" = "1024") instead of formatting it into the message,
// so logs can be filtered and aggregated.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    Name category;
    String message;
    std::vector<LogField> fields;
    std::chrono::system_clock::time_point time;
    std::thread::id thread;
};

// Where records go. Sinks are called with the logger's lock held, one record
// at a time, so an implementation needs no locking of its own; it must not log
// through the same Logger (that would deadlock).
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord &record) = 0;
    virtual void flush() {}
};

// Structured, levelled, categorised logging. There is no global logger
// (constraint 5): the application owns one and hands references to the
// subsystems that log. A subsystem given no logger logs nothing.
//
//     logger.info("net", "client connected", {{"address", peer}, {"id", id}});
//
// Levels filter per category; a category without its own level uses the
// default. isEnabled() is lock-free for the common "no per-category levels"
// case, so disabled logging costs one atomic load.
//
// Threads: every member function may be called from any thread.
// Allocates: the record (message and fields) for each enabled log call.
class Logger {
public:
    Logger() = default;
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    ~Logger();

    // Takes ownership; returns the sink for configuration.
    LogSink &addSink(std::unique_ptr<LogSink> sink);

    void setLevel(LogLevel level) noexcept { m_defaultLevel.store(level, std::memory_order_relaxed); }
    void setCategoryLevel(const Name &category, LogLevel level);
    [[nodiscard]] bool isEnabled(LogLevel level, const Name &category) const;

    void log(LogLevel level, const Name &category, StringView message, std::vector<LogField> fields = {});
    void trace(const Name &category, StringView message, std::vector<LogField> fields = {}) {
        log(LogLevel::Trace, category, message, std::move(fields));
    }
    void debug(const Name &category, StringView message, std::vector<LogField> fields = {}) {
        log(LogLevel::Debug, category, message, std::move(fields));
    }
    void info(const Name &category, StringView message, std::vector<LogField> fields = {}) {
        log(LogLevel::Info, category, message, std::move(fields));
    }
    void warning(const Name &category, StringView message, std::vector<LogField> fields = {}) {
        log(LogLevel::Warning, category, message, std::move(fields));
    }
    void error(const Name &category, StringView message, std::vector<LogField> fields = {}) {
        log(LogLevel::Error, category, message, std::move(fields));
    }

    void flush();

private:
    std::atomic<LogLevel> m_defaultLevel{LogLevel::Info};
    std::atomic<bool> m_hasCategoryLevels{false};
    mutable std::mutex m_mutex;
    FlatMap<Name, LogLevel> m_categoryLevels;
    std::vector<std::unique_ptr<LogSink>> m_sinks;
};

// "2026-09-27T10:04:05.123Z INFO  [net] client connected address=1.2.3.4"
[[nodiscard]] String formatLogRecord(const LogRecord &record);

// Writes formatted records to a C stream (stderr by default). Does not own or
// close the stream.
class StreamLogSink final : public LogSink {
public:
    explicit StreamLogSink(std::FILE *stream = stderr) noexcept : m_stream(stream) {}
    void write(const LogRecord &record) override;
    void flush() override;

private:
    std::FILE *m_stream;
};

// Keeps records in memory: for tests, and for an in-editor log panel.
class MemoryLogSink final : public LogSink {
public:
    explicit MemoryLogSink(std::size_t maxRecords = 10000) noexcept : m_maxRecords(maxRecords) {}
    void write(const LogRecord &record) override;
    // The kept records, oldest first. Not synchronised with logging: read it
    // only while no other thread can log to this sink (tests, or after the
    // frame loop has stopped).
    [[nodiscard]] const std::vector<LogRecord> &records() const noexcept { return m_records; }

private:
    std::size_t m_maxRecords;
    std::vector<LogRecord> m_records;
};

} // namespace cfw
