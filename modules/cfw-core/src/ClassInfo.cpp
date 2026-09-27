#include "cfw/core/ClassInfo.h"

#include <algorithm>
#include <bit>

#include "cfw/core/Contract.h"

namespace cfw {

namespace {

Error invalid(const Name &className, const Name &property, const char *message) {
    return Error(ErrorCode::InvalidArgument, message)
        .with("class", className.str())
        .with("property", property.str());
}

} // namespace

ClassInfo::Builder &ClassInfo::Builder::inherit(const ClassInfo &base) & {
    for (const PropertyInfo &info : base.properties()) {
        m_properties.push_back(info);
    }
    return *this;
}

ClassInfo::Builder &ClassInfo::Builder::property(PropertyInfo info) & {
    m_properties.push_back(std::move(info));
    return *this;
}

Result<ClassInfo> ClassInfo::Builder::build() && {
    for (std::size_t i = 0; i < m_properties.size(); ++i) {
        PropertyInfo &info = m_properties[i];
        if (info.name.empty()) {
            return invalid(m_name, info.name, "property has no name");
        }
        if (info.type == VariantType::Null || info.type == VariantType::Array) {
            return invalid(m_name, info.name, "property type must be a single value type");
        }
        // Int defaults for Number properties are a common slip; accept them.
        if (info.type == VariantType::Double) {
            if (const auto *i64 = info.defaultValue.getIf<std::int64_t>()) {
                info.defaultValue = static_cast<double>(*i64);
            }
        }
        if (info.defaultValue.type() != info.type) {
            return invalid(m_name, info.name, "default value has the wrong type")
                .with("expected", String(variantTypeName(info.type)))
                .with("actual", String(variantTypeName(info.defaultValue.type())));
        }
        if (info.isChoice()) {
            if (info.type != VariantType::String) {
                return invalid(m_name, info.name, "choice properties must be String-typed");
            }
            const String &value = *info.defaultValue.getIf<String>();
            if (std::find(info.choices.begin(), info.choices.end(), value) == info.choices.end()) {
                return invalid(m_name, info.name, "default is not one of the choices").with("default", value);
            }
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (m_properties[j].name == info.name) {
                return invalid(m_name, info.name, "duplicate property");
            }
        }
    }
    require(m_properties.size() < UINT32_MAX / 2, "absurd property count");

    ClassInfo info;
    info.m_name = std::move(m_name);
    info.m_properties = std::move(m_properties);
    info.buildIndex();
    return info;
}

void ClassInfo::buildIndex() {
    const std::size_t size = std::bit_ceil(std::max<std::size_t>(m_properties.size() * 2, 4));
    m_slots.assign(size, 0);
    const std::size_t mask = size - 1;
    for (std::size_t i = 0; i < m_properties.size(); ++i) {
        std::size_t slot = static_cast<std::size_t>(m_properties[i].name.hash()) & mask;
        while (m_slots[slot] != 0) {
            slot = (slot + 1) & mask;
        }
        m_slots[slot] = static_cast<std::uint32_t>(i + 1);
    }
}

std::optional<std::size_t> ClassInfo::lookup(std::uint64_t hash, StringView text) const noexcept {
    if (m_slots.empty()) {
        return std::nullopt;
    }
    const std::size_t mask = m_slots.size() - 1;
    for (std::size_t slot = static_cast<std::size_t>(hash) & mask;; slot = (slot + 1) & mask) {
        const std::uint32_t entry = m_slots[slot];
        if (entry == 0) {
            return std::nullopt;
        }
        const Name &candidate = m_properties[entry - 1].name;
        if (candidate.hash() == hash && candidate.view() == text) {
            return entry - 1;
        }
    }
}

std::optional<std::size_t> ClassInfo::indexOf(const Name &name) const noexcept {
    return lookup(name.hash(), name.view());
}

std::optional<std::size_t> ClassInfo::indexOf(StringView name) const noexcept {
    return lookup(hashString(name), name);
}

const PropertyInfo &ClassInfo::property(std::size_t index) const noexcept {
    debugCheck(index < m_properties.size(), "ClassInfo::property index out of range");
    return m_properties[index];
}

const PropertyInfo *ClassInfo::find(const Name &name) const noexcept {
    const auto index = indexOf(name);
    return index ? &m_properties[*index] : nullptr;
}

} // namespace cfw
