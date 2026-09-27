#include "cfw/core/Variant.h"

#include <cmath>

#include "cfw/core/Contract.h"

namespace cfw {

namespace {

constexpr StringView kTypeNames[] = {"Null",    "Bool",    "Int",   "Number", "String",    "Vector2",
                                     "Vector3", "Color",   "Name",  "AssetLink", "Array"};

static_assert(std::size(kTypeNames) == static_cast<std::size_t>(VariantType::Array) + 1,
              "every VariantType needs a name");

bool sameDouble(double a, double b) noexcept { return a == b || (std::isnan(a) && std::isnan(b)); }

} // namespace

StringView variantTypeName(VariantType type) noexcept {
    const auto index = static_cast<std::size_t>(type);
    return index < std::size(kTypeNames) ? kTypeNames[index] : StringView("Null");
}

std::optional<VariantType> variantTypeFromName(StringView name) noexcept {
    for (std::size_t i = 0; i < std::size(kTypeNames); ++i) {
        if (kTypeNames[i] == name) {
            return static_cast<VariantType>(i);
        }
    }
    return std::nullopt;
}

VariantArray::VariantArray(VariantType elementType) : m_elementType(elementType) {
    require(elementType != VariantType::Null && elementType != VariantType::Array,
            "VariantArray element type must not be Null or Array");
}

const Variant &VariantArray::operator[](std::size_t index) const {
    debugCheck(index < m_items.size(), "VariantArray index out of range");
    return m_items[index];
}

Result<void> VariantArray::append(Variant value) {
    if (value.type() != m_elementType) {
        return Error(ErrorCode::TypeMismatch, "array element has the wrong type")
            .with("expected", String(variantTypeName(m_elementType)))
            .with("actual", String(variantTypeName(value.type())));
    }
    m_items.push_back(std::move(value));
    return success();
}

Result<void> VariantArray::set(std::size_t index, Variant value) {
    if (index >= m_items.size()) {
        return Error(ErrorCode::OutOfRange, "array index out of range").with("index", std::to_string(index));
    }
    if (value.type() != m_elementType) {
        return Error(ErrorCode::TypeMismatch, "array element has the wrong type")
            .with("expected", String(variantTypeName(m_elementType)))
            .with("actual", String(variantTypeName(value.type())));
    }
    m_items[index] = std::move(value);
    return success();
}

void VariantArray::removeAt(std::size_t index) {
    require(index < m_items.size(), "VariantArray::removeAt index out of range");
    m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(index));
}

bool operator==(const VariantArray &a, const VariantArray &b) {
    return a.m_elementType == b.m_elementType && a.m_items == b.m_items;
}

std::optional<double> Variant::toNumber() const noexcept {
    if (const auto *i = getIf<std::int64_t>()) {
        return static_cast<double>(*i);
    }
    if (const auto *d = getIf<double>()) {
        return *d;
    }
    return std::nullopt;
}

bool operator==(const Variant &a, const Variant &b) {
    if (a.m_value.index() != b.m_value.index()) {
        return false;
    }
    if (const auto *d = a.getIf<double>()) {
        return sameDouble(*d, *b.getIf<double>());
    }
    if (const auto *v = a.getIf<Vec2>()) {
        const Vec2 w = *b.getIf<Vec2>();
        return sameDouble(v->x, w.x) && sameDouble(v->y, w.y);
    }
    if (const auto *v = a.getIf<Vec3>()) {
        const Vec3 w = *b.getIf<Vec3>();
        return sameDouble(v->x, w.x) && sameDouble(v->y, w.y) && sameDouble(v->z, w.z);
    }
    if (const auto *c = a.getIf<Color>()) {
        const Color d = *b.getIf<Color>();
        return sameDouble(c->r, d.r) && sameDouble(c->g, d.g) && sameDouble(c->b, d.b) && sameDouble(c->a, d.a);
    }
    return a.m_value == b.m_value;
}

std::ostream &operator<<(std::ostream &out, const Variant &value) {
    out << variantTypeName(value.type()) << '(';
    std::visit(
        [&out](const auto &held) {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
            } else if constexpr (std::is_same_v<T, bool>) {
                out << (held ? "true" : "false");
            } else if constexpr (std::is_same_v<T, Name>) {
                out << held.view();
            } else if constexpr (std::is_same_v<T, VariantArray>) {
                out << held.size() << " x " << variantTypeName(held.elementType());
            } else {
                out << held;
            }
        },
        value.m_value);
    return out << ')';
}

} // namespace cfw
