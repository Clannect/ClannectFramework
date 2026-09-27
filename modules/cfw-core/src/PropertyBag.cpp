#include "cfw/core/PropertyBag.h"

#include <algorithm>

#include "cfw/core/Contract.h"

namespace cfw {

namespace {

const Variant &nullVariant() noexcept {
    // Constant-initialised (constinit proves it) and immutable: no static
    // initialisation order issue and nothing shared that anyone can write.
    static constinit const Variant kNull{};
    return kNull;
}

} // namespace

const Variant &PropertyBag::valueAt(std::size_t index) const noexcept {
    debugCheck(index < m_schema->propertyCount(), "PropertyBag::valueAt index out of range");
    if (index < m_values.size() && !m_values[index].isNull()) {
        return m_values[index];
    }
    return m_schema->property(index).defaultValue;
}

const Variant &PropertyBag::get(const Name &name) const noexcept {
    if (const auto index = m_schema->indexOf(name)) {
        return valueAt(*index);
    }
    if (const Variant *extra = m_extras.get(name)) {
        return *extra;
    }
    return nullVariant();
}

Result<bool> PropertyBag::set(const Name &name, Variant value) {
    const auto index = m_schema->indexOf(name);
    if (!index) {
        return Error(ErrorCode::NotFound, "no such property")
            .with("class", m_schema->name().str())
            .with("property", name.str());
    }
    return setAt(*index, std::move(value));
}

Result<bool> PropertyBag::setAt(std::size_t index, Variant value) {
    require(index < m_schema->propertyCount(), "PropertyBag::setAt index out of range");
    const PropertyInfo &info = m_schema->property(index);

    if (info.type == VariantType::Double) {
        if (const auto *i64 = value.getIf<std::int64_t>()) {
            value = static_cast<double>(*i64);
        }
    }
    if (value.type() != info.type) {
        return Error(ErrorCode::TypeMismatch, "property value has the wrong type")
            .with("property", info.name.str())
            .with("expected", String(variantTypeName(info.type)))
            .with("actual", String(variantTypeName(value.type())));
    }
    if (info.isChoice()) {
        const String &text = *value.getIf<String>();
        if (std::find(info.choices.begin(), info.choices.end(), text) == info.choices.end()) {
            return Error(ErrorCode::InvalidArgument, "value is not one of the property's choices")
                .with("property", info.name.str())
                .with("value", text);
        }
    }

    const bool changed = !(valueAt(index) == value);
    if (m_values.empty()) {
        m_values.resize(m_schema->propertyCount());
    }
    m_values[index] = std::move(value);
    return changed;
}

bool PropertyBag::isSet(const Name &name) const noexcept {
    const auto index = m_schema->indexOf(name);
    return index && *index < m_values.size() && !m_values[*index].isNull();
}

bool PropertyBag::reset(const Name &name) noexcept {
    const auto index = m_schema->indexOf(name);
    if (!index || *index >= m_values.size() || m_values[*index].isNull()) {
        return false;
    }
    const bool changed = !(m_values[*index] == m_schema->property(*index).defaultValue);
    m_values[*index] = Variant();
    return changed;
}

void PropertyBag::setExtra(Name name, Variant value) { m_extras.insertOrAssign(std::move(name), std::move(value)); }

} // namespace cfw
