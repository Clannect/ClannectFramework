#include "cfw/core/Logger.h"

namespace cfw {

const char *logLevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Off: return "OFF";
    }
    return "?";
}

Logger::~Logger() { flush(); }

LogSink &Logger::addSink(std::unique_ptr<LogSink> sink) {
    const std::lock_guard lock(m_mutex);
    m_sinks.push_back(std::move(sink));
    return *m_sinks.back();
}

void Logger::setCategoryLevel(const Name &category, LogLevel level) {
    const std::lock_guard lock(m_mutex);
    m_categoryLevels.insertOrAssign(category, level);
    m_hasCategoryLevels.store(true, std::memory_order_release);
}

bool Logger::isEnabled(LogLevel level, const Name &category) const {
    if (level == LogLevel::Off) {
        return false;
    }
    if (m_hasCategoryLevels.load(std::memory_order_acquire)) {
        const std::lock_guard lock(m_mutex);
        if (const LogLevel *categoryLevel = m_categoryLevels.get(category)) {
            return level >= *categoryLevel;
        }
    }
    return level >= m_defaultLevel.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel level, const Name &category, StringView message, std::vector<LogField> fields) {
    if (!isEnabled(level, category)) {
        return;
    }
    LogRecord record{level, category, String(message), std::move(fields), std::chrono::system_clock::now(),
                     std::this_thread::get_id()};
    const std::lock_guard lock(m_mutex);
    for (const auto &sink : m_sinks) {
        sink->write(record);
    }
}

void Logger::flush() {
    const std::lock_guard lock(m_mutex);
    for (const auto &sink : m_sinks) {
        sink->flush();
    }
}

String formatLogRecord(const LogRecord &record) {
    using namespace std::chrono;
    const auto dayPoint = floor<days>(record.time);
    const year_month_day date{dayPoint};
    const hh_mm_ss timeOfDay{floor<milliseconds>(record.time - dayPoint)};

    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                  static_cast<int>(timeOfDay.hours().count()), static_cast<int>(timeOfDay.minutes().count()),
                  static_cast<int>(timeOfDay.seconds().count()), static_cast<int>(timeOfDay.subseconds().count()));

    char level[8];
    std::snprintf(level, sizeof level, "%-5s", logLevelName(record.level));

    String out = stamp;
    out += ' ';
    out += level;
    out += " [";
    out += record.category.view();
    out += "] ";
    out += record.message;
    for (const LogField &field : record.fields) {
        out += ' ';
        out += field.key;
        out += '=';
        // Quote values with spaces so the line stays machine-splittable.
        const bool quote = field.value.find_first_of(" \t\"") != String::npos || field.value.empty();
        if (quote) {
            out += '"';
            for (char c : field.value) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                }
                out += c;
            }
            out += '"';
        } else {
            out += field.value;
        }
    }
    return out;
}

void StreamLogSink::write(const LogRecord &record) {
    const String line = formatLogRecord(record);
    std::fprintf(m_stream, "%s\n", line.c_str());
}

void StreamLogSink::flush() { std::fflush(m_stream); }

void MemoryLogSink::write(const LogRecord &record) {
    if (m_records.size() >= m_maxRecords && !m_records.empty()) {
        m_records.erase(m_records.begin());
    }
    m_records.push_back(record);
}

} // namespace cfw
