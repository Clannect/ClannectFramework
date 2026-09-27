#include "cfw/io/Json.h"

#include <algorithm>
#include <cmath>

#include "cfw/core/Utf8.h"

namespace cfw {

namespace {

const JsonValue &nullJson() noexcept {
    // Constant-initialised and immutable (see PropertyBag's equivalent).
    static constinit const JsonValue kNull{};
    return kNull;
}

bool keyLess(StringView a, StringView b) noexcept { return compareUtf16Order(a, b) < 0; }

} // namespace

JsonObject::JsonObject(std::initializer_list<Member> members)
    : JsonObject(fromMembers(std::vector<Member>(members))) {}

JsonObject JsonObject::fromMembers(std::vector<Member> members) {
    const auto less = [](const Member &a, const Member &b) { return keyLess(a.first, b.first); };
    JsonObject object;
    // Files written by CFW or the Qt build are already in order with unique
    // keys: take the vector as it is, with no sort and no copy.
    const bool strictlySorted =
        std::adjacent_find(members.begin(), members.end(),
                           [&](const Member &a, const Member &b) { return !less(a, b); }) == members.end();
    if (!strictlySorted) {
        // Stable sort keeps duplicates in source order; keep the last of each run.
        std::stable_sort(members.begin(), members.end(), less);
        std::size_t kept = 0;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (i + 1 < members.size() && members[i + 1].first == members[i].first) {
                continue; // a later duplicate wins
            }
            if (kept != i) {
                members[kept] = std::move(members[i]);
            }
            ++kept;
        }
        members.erase(members.begin() + static_cast<std::ptrdiff_t>(kept), members.end());
    }
    object.m_members = std::move(members);
    return object;
}

std::size_t JsonObject::lowerBound(StringView key) const noexcept {
    const auto it = std::lower_bound(m_members.begin(), m_members.end(), key,
                                     [](const Member &m, StringView k) { return keyLess(m.first, k); });
    return static_cast<std::size_t>(it - m_members.begin());
}

const JsonValue *JsonObject::find(StringView key) const noexcept {
    const std::size_t i = lowerBound(key);
    return i < m_members.size() && m_members[i].first == key ? &m_members[i].second : nullptr;
}

JsonValue *JsonObject::find(StringView key) noexcept {
    const std::size_t i = lowerBound(key);
    return i < m_members.size() && m_members[i].first == key ? &m_members[i].second : nullptr;
}

void JsonObject::set(String key, JsonValue value) {
    const std::size_t i = lowerBound(key);
    if (i < m_members.size() && m_members[i].first == key) {
        m_members[i].second = std::move(value);
        return;
    }
    m_members.emplace(m_members.begin() + static_cast<std::ptrdiff_t>(i), std::move(key), std::move(value));
}

bool JsonObject::remove(StringView key) {
    const std::size_t i = lowerBound(key);
    if (i < m_members.size() && m_members[i].first == key) {
        m_members.erase(m_members.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

bool operator==(const JsonObject &a, const JsonObject &b) { return a.m_members == b.m_members; }

std::optional<double> JsonValue::toDouble() const noexcept {
    if (const auto *i = asInteger()) {
        return static_cast<double>(*i);
    }
    if (const auto *d = asDouble()) {
        return *d;
    }
    return std::nullopt;
}

std::optional<std::int64_t> JsonValue::toInteger() const noexcept {
    if (const auto *i = asInteger()) {
        return *i;
    }
    if (const auto *d = asDouble()) {
        // 2^63 is exactly representable; anything >= it does not fit.
        if (std::isfinite(*d) && std::trunc(*d) == *d && *d >= -9223372036854775808.0 && *d < 9223372036854775808.0) {
            return static_cast<std::int64_t>(*d);
        }
    }
    return std::nullopt;
}

const JsonValue &JsonValue::operator[](StringView key) const noexcept {
    if (const JsonObject *object = asObject()) {
        if (const JsonValue *value = object->find(key)) {
            return *value;
        }
    }
    return nullJson();
}

const JsonValue &JsonValue::operator[](std::size_t index) const noexcept {
    if (const JsonArray *array = asArray(); array && index < array->size()) {
        return (*array)[index];
    }
    return nullJson();
}

bool operator==(const JsonValue &a, const JsonValue &b) {
    // JSON has one number type, so an Integer and a Double compare as doubles,
    // as Qt compares them. Both Qt and CFW write integral doubles without an
    // exponent (-2.5e10 as -25000000000), which re-parses as an Integer; this
    // rule is what keeps a write + re-parse equal. Two Integers still compare
    // exactly.
    if (a.isNumber() && b.isNumber() && a.type() != b.type()) {
        return a.toDouble() == b.toDouble();
    }
    return a.m_value == b.m_value;
}

} // namespace cfw
