#include "cfw/io/Settings.h"

#include "cfw/io/FileSystem.h"
#include "cfw/io/JsonReader.h"
#include "cfw/io/JsonWriter.h"

namespace cfw {

namespace {

constexpr std::size_t kMaxSettingsBytes = 16u * 1024u * 1024u;

} // namespace

Result<Settings> Settings::open(Path file) {
    Settings settings;
    settings.m_path = std::move(file);
    settings.m_persistent = true;
    if (!exists(settings.m_path)) {
        return settings;
    }
    Result<String> text = readTextFile(settings.m_path, kMaxSettingsBytes);
    if (!text) {
        return std::move(text).error();
    }
    Result<JsonValue> parsed = parseJson(text.value());
    if (!parsed) {
        return std::move(parsed).error().with("path", settings.m_path.toString());
    }
    JsonObject *object = parsed.value().asObject();
    if (object == nullptr) {
        return Error(ErrorCode::Corrupt, "settings file is not a JSON object").with("path", settings.m_path.toString());
    }
    settings.m_values = std::move(*object);
    return settings;
}

Settings Settings::inMemory() { return Settings(); }

bool Settings::getBool(StringView key, bool fallback) const noexcept {
    const JsonValue *value = get(key);
    return value ? value->toBool(fallback) : fallback;
}

std::int64_t Settings::getInt(StringView key, std::int64_t fallback) const noexcept {
    const JsonValue *value = get(key);
    return value ? value->toInteger().value_or(fallback) : fallback;
}

double Settings::getDouble(StringView key, double fallback) const noexcept {
    const JsonValue *value = get(key);
    return value ? value->toDouble(fallback) : fallback;
}

String Settings::getString(StringView key, StringView fallback) const {
    const JsonValue *value = get(key);
    return String(value ? value->toString(fallback) : fallback);
}

void Settings::set(StringView key, JsonValue value) {
    if (const JsonValue *existing = get(key); existing && *existing == value) {
        return;
    }
    m_values.set(String(key), std::move(value));
    m_dirty = true;
}

bool Settings::remove(StringView key) {
    const bool removed = m_values.remove(key);
    m_dirty = m_dirty || removed;
    return removed;
}

Result<void> Settings::save() {
    if (!m_persistent || !m_dirty) {
        return success();
    }
    if (auto made = createDirectories(m_path.parent()); !made) {
        return made;
    }
    if (auto written = writeFileAtomic(m_path, writeJson(JsonValue(m_values))); !written) {
        return written;
    }
    m_dirty = false;
    return success();
}

} // namespace cfw
