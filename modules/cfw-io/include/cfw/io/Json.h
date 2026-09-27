#pragma once

// JSON values. JsonValue, JsonArray and JsonObject are defined together
// because each contains the others.
//
// Parse with parseJson() (JsonReader.h); write with writeJson() (JsonWriter.h).

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "cfw/core/String.h"

namespace cfw {

class JsonValue;

using JsonArray = std::vector<JsonValue>;

// A JSON object. Members are kept sorted by key, ordered the way UTF-16
// strings compare (see compareUtf16Order), which is the order the Qt build
// wrote scene files in; keeping it makes CFW's output byte-identical. Keys are
// unique: setting an existing key replaces its value.
//
// Threads: a value type. Allocates: member storage.
class JsonObject {
public:
    using Member = std::pair<String, JsonValue>;

    JsonObject() = default;
    JsonObject(std::initializer_list<Member> members);

    // From members in any order. On duplicate keys the last one wins, as in
    // every mainstream parser.
    [[nodiscard]] static JsonObject fromMembers(std::vector<Member> members);

    // Defined after JsonValue: instantiating std::pair<String, JsonValue>
    // here, while JsonValue is incomplete, breaks the pair's assignment on Clang.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::vector<Member>::const_iterator begin() const noexcept;
    [[nodiscard]] std::vector<Member>::const_iterator end() const noexcept;

    // The value for `key`, or null if absent. Invalidated by set/remove.
    [[nodiscard]] const JsonValue *find(StringView key) const noexcept;
    [[nodiscard]] JsonValue *find(StringView key) noexcept;
    [[nodiscard]] bool contains(StringView key) const noexcept { return find(key) != nullptr; }

    void set(String key, JsonValue value);
    bool remove(StringView key);

    friend bool operator==(const JsonObject &a, const JsonObject &b);

private:
    [[nodiscard]] std::size_t lowerBound(StringView key) const noexcept;

    std::vector<Member> m_members;
};

enum class JsonType : std::uint8_t { Null, Bool, Integer, Double, String, Array, Object };

// One JSON value. Numbers keep the distinction the source made: an integer
// literal that fits in int64 is an Integer (exact, even above 2^53); anything
// else is a Double. Both answer toDouble().
//
// Accessors never throw and never abort: asX() returns null for the wrong
// type, and operator[] on a non-object or missing key returns a shared null
// value, so lookups into untrusted documents chain safely:
//     const JsonValue &name = response["document"]["name"];
//     if (const String *s = name.asString()) { ... }
//
// Threads: a value type. Allocates: strings and containers.
class JsonValue {
public:
    JsonValue() noexcept = default;
    JsonValue(std::nullptr_t) noexcept {}
    JsonValue(bool value) noexcept : m_value(value) {}
    template <std::integral I>
        requires(!std::is_same_v<I, bool> && !std::is_same_v<I, char> &&
                 (std::is_signed_v<I> || sizeof(I) < sizeof(std::int64_t)))
    JsonValue(I value) noexcept : m_value(static_cast<std::int64_t>(value)) {}
    template <std::floating_point F>
    JsonValue(F value) noexcept : m_value(static_cast<double>(value)) {}
    JsonValue(String value) noexcept : m_value(std::move(value)) {}
    JsonValue(StringView value) : m_value(String(value)) {}
    JsonValue(const char *value) : m_value(String(value)) {}
    JsonValue(JsonArray value) noexcept : m_value(std::move(value)) {}
    JsonValue(JsonObject value) noexcept : m_value(std::move(value)) {}

    [[nodiscard]] JsonType type() const noexcept { return static_cast<JsonType>(m_value.index()); }
    [[nodiscard]] bool isNull() const noexcept { return type() == JsonType::Null; }
    [[nodiscard]] bool isNumber() const noexcept {
        return type() == JsonType::Integer || type() == JsonType::Double;
    }

    [[nodiscard]] const bool *asBool() const noexcept { return std::get_if<bool>(&m_value); }
    [[nodiscard]] const std::int64_t *asInteger() const noexcept { return std::get_if<std::int64_t>(&m_value); }
    [[nodiscard]] const double *asDouble() const noexcept { return std::get_if<double>(&m_value); }
    [[nodiscard]] const String *asString() const noexcept { return std::get_if<String>(&m_value); }
    [[nodiscard]] const JsonArray *asArray() const noexcept { return std::get_if<JsonArray>(&m_value); }
    [[nodiscard]] JsonArray *asArray() noexcept { return std::get_if<JsonArray>(&m_value); }
    [[nodiscard]] const JsonObject *asObject() const noexcept { return std::get_if<JsonObject>(&m_value); }
    [[nodiscard]] JsonObject *asObject() noexcept { return std::get_if<JsonObject>(&m_value); }

    // Any number as a double; nothing for other types.
    [[nodiscard]] std::optional<double> toDouble() const noexcept;
    // An Integer, or a Double with an exact int64 value; nothing otherwise.
    [[nodiscard]] std::optional<std::int64_t> toInteger() const noexcept;

    [[nodiscard]] bool toBool(bool fallback) const noexcept { return asBool() ? *asBool() : fallback; }
    [[nodiscard]] double toDouble(double fallback) const noexcept { return toDouble().value_or(fallback); }
    [[nodiscard]] StringView toString(StringView fallback) const noexcept {
        return asString() ? StringView(*asString()) : fallback;
    }

    // Member lookup; a null value when this is not an object or has no such key.
    [[nodiscard]] const JsonValue &operator[](StringView key) const noexcept;
    // Element lookup; a null value when this is not an array or out of range.
    [[nodiscard]] const JsonValue &operator[](std::size_t index) const noexcept;

    // Deep equality. An Integer and a Double compare as doubles (Integer 5
    // equals Double 5.0), as in Qt; two Integers compare exactly.
    friend bool operator==(const JsonValue &a, const JsonValue &b);

private:
    std::variant<std::monostate, bool, std::int64_t, double, String, JsonArray, JsonObject> m_value;
};

inline std::size_t JsonObject::size() const noexcept { return m_members.size(); }
inline bool JsonObject::empty() const noexcept { return m_members.empty(); }
inline std::vector<JsonObject::Member>::const_iterator JsonObject::begin() const noexcept { return m_members.begin(); }
inline std::vector<JsonObject::Member>::const_iterator JsonObject::end() const noexcept { return m_members.end(); }

} // namespace cfw
