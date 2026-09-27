#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <type_traits>
#include <variant>
#include <vector>

#include "cfw/core/AssetLink.h"
#include "cfw/core/Color.h"
#include "cfw/core/Name.h"
#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/core/Vec2.h"
#include "cfw/core/Vec3.h"

namespace cfw {

// The kinds of value a Variant can hold. Closed on purpose: a property can
// only hold these, so every serialiser, the undo system and the Properties
// panel handle every case. Values are stable: append only.
enum class VariantType : std::uint8_t {
    Null = 0,
    Bool = 1,
    Int = 2,
    Double = 3,
    String = 4,
    Vec2 = 5,
    Vec3 = 6,
    Color = 7,
    Name = 8,
    AssetLink = 9,
    Array = 10,
};

// Stable names for VariantType, matching the scene format's existing "type"
// strings where one exists: "Null", "Bool", "Int", "Number", "String",
// "Vector2", "Vector3", "Color", "Name", "AssetLink", "Array".
[[nodiscard]] StringView variantTypeName(VariantType type) noexcept;
[[nodiscard]] std::optional<VariantType> variantTypeFromName(StringView name) noexcept;

class Variant;

// A homogeneous list of Variants: every element has elementType(). Arrays of
// arrays and arrays of Null are not allowed, which keeps every array a flat,
// typed list that maps directly to a JSON array or a length-prefixed binary
// run.
//
// Threads: a value type. Allocates: the element storage.
class VariantArray {
public:
    // `elementType` must not be Null or Array.
    explicit VariantArray(VariantType elementType);

    [[nodiscard]] VariantType elementType() const noexcept { return m_elementType; }
    // Defined after Variant: std::vector<Variant> members need it complete.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const Variant &operator[](std::size_t index) const;
    [[nodiscard]] std::vector<Variant>::const_iterator begin() const noexcept;
    [[nodiscard]] std::vector<Variant>::const_iterator end() const noexcept;

    // Fails with TypeMismatch if `value` is not of elementType().
    Result<void> append(Variant value);
    // Fails with OutOfRange or TypeMismatch; the array is unchanged on failure.
    Result<void> set(std::size_t index, Variant value);
    void removeAt(std::size_t index);
    void reserve(std::size_t count);
    void clear() noexcept;

    friend bool operator==(const VariantArray &a, const VariantArray &b);

private:
    VariantType m_elementType;
    std::vector<Variant> m_items;
};

// A value of one of the closed set of property types. Replaces QVariant.
//
// Construction is implicit from each supported type, so
//     instance.setProperty(kPosition, Vec3{0, 5, 0});
// reads naturally. Integers become Int (int64), floats become Double, text
// becomes String. Unsigned 64-bit values are rejected at compile time since
// they may not fit; cast explicitly.
//
// Equality: same type and equal value. Doubles compare by value, except that
// NaN equals NaN, so "set a property to what it already is" is always a no-op
// for undo and change notification.
//
// Threads: a value type. Allocates: for String/Name/AssetLink values longer
// than the small-string buffer, and for arrays.
class Variant {
public:
    Variant() noexcept = default;

    Variant(bool value) noexcept : m_value(value) {}

    template <std::integral I>
        requires(!std::is_same_v<I, bool> && !std::is_same_v<I, char> && !std::is_same_v<I, char8_t> &&
                 !std::is_same_v<I, char16_t> && !std::is_same_v<I, char32_t> && !std::is_same_v<I, wchar_t> &&
                 (std::is_signed_v<I> || sizeof(I) < sizeof(std::int64_t)))
    Variant(I value) noexcept : m_value(static_cast<std::int64_t>(value)) {}

    template <std::floating_point F>
    Variant(F value) noexcept : m_value(static_cast<double>(value)) {}

    Variant(String value) noexcept : m_value(std::move(value)) {}
    Variant(StringView value) : m_value(String(value)) {}
    Variant(const char *value) : m_value(String(value)) {}
    Variant(Vec2 value) noexcept : m_value(value) {}
    Variant(Vec3 value) noexcept : m_value(value) {}
    Variant(Color value) noexcept : m_value(value) {}
    Variant(Name value) noexcept : m_value(std::move(value)) {}
    Variant(AssetLink value) noexcept : m_value(std::move(value)) {}
    Variant(VariantArray value) noexcept : m_value(std::move(value)) {}

    [[nodiscard]] VariantType type() const noexcept { return static_cast<VariantType>(m_value.index()); }
    [[nodiscard]] bool isNull() const noexcept { return type() == VariantType::Null; }

    // Pointer to the held value if it has exactly type T, otherwise null.
    // T is one of: bool, std::int64_t, double, String, Vec2, Vec3, Color,
    // Name, AssetLink, VariantArray.
    template <class T>
    [[nodiscard]] const T *getIf() const noexcept {
        return std::get_if<T>(&m_value);
    }
    template <class T>
    [[nodiscard]] T *getIf() noexcept {
        return std::get_if<T>(&m_value);
    }

    // The held value if it has exactly type T, otherwise `fallback`.
    template <class T>
    [[nodiscard]] T valueOr(T fallback) const {
        const T *value = getIf<T>();
        return value ? *value : fallback;
    }

    // Int or Double as a double; nothing for any other type.
    [[nodiscard]] std::optional<double> toNumber() const noexcept;

    friend bool operator==(const Variant &a, const Variant &b);

    friend std::ostream &operator<<(std::ostream &out, const Variant &value);

private:
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, String, Vec2, Vec3, Color, Name,
                                 AssetLink, VariantArray>;
    Storage m_value;
};

inline std::size_t VariantArray::size() const noexcept { return m_items.size(); }
inline bool VariantArray::empty() const noexcept { return m_items.empty(); }
inline std::vector<Variant>::const_iterator VariantArray::begin() const noexcept { return m_items.begin(); }
inline std::vector<Variant>::const_iterator VariantArray::end() const noexcept { return m_items.end(); }
inline void VariantArray::reserve(std::size_t count) { m_items.reserve(count); }
inline void VariantArray::clear() noexcept { m_items.clear(); }

} // namespace cfw
