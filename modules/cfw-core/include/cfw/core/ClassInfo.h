#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "cfw/core/Name.h"
#include "cfw/core/PropertyInfo.h"
#include "cfw/core/Result.h"
#include "cfw/core/Span.h"

namespace cfw {

// The declared properties of one class: the schema that PropertyBag stores
// values against. Built once (typically at startup, from the object catalogue)
// and immutable afterwards, so any number of objects and threads can share it.
//
//     Result<ClassInfo> part = ClassInfo::Builder("Part")
//         .property({.name = "Size", .type = VariantType::Vec3, .defaultValue = Vec3{4, 1, 2}})
//         .property({.name = "Anchored", .type = VariantType::Bool, .defaultValue = false})
//         .build();
//
// Lookup by Name is O(1) with no allocation: an open-addressing table keyed by
// the Name's precomputed hash maps to the property's index. Indices are stable
// for the life of the ClassInfo, so hot code can resolve a name once and use
// the index afterwards.
//
// Threads: build on one thread; a built ClassInfo is immutable and safe to read
// from any thread. Allocates: at build time only.
class ClassInfo {
public:
    class Builder {
    public:
        explicit Builder(Name className) : m_name(std::move(className)) {}

        // Copies every property of `base` (in order) before this class's own.
        Builder &inherit(const ClassInfo &base) &;
        Builder &&inherit(const ClassInfo &base) && { return std::move(static_cast<Builder &>(*this).inherit(base)); }
        Builder &property(PropertyInfo info) &;
        Builder &&property(PropertyInfo info) && { return std::move(static_cast<Builder &>(*this).property(std::move(info))); }

        // Fails with InvalidArgument on a duplicate name, an empty name, a
        // default of the wrong type, an array- or null-typed property, or a
        // choice default that is not one of the choices.
        [[nodiscard]] Result<ClassInfo> build() &&;

    private:
        Name m_name;
        std::vector<PropertyInfo> m_properties;
    };

    ClassInfo(ClassInfo &&) noexcept = default;
    ClassInfo &operator=(ClassInfo &&) noexcept = default;
    ClassInfo(const ClassInfo &) = delete;
    ClassInfo &operator=(const ClassInfo &) = delete;

    [[nodiscard]] const Name &name() const noexcept { return m_name; }
    [[nodiscard]] std::size_t propertyCount() const noexcept { return m_properties.size(); }
    [[nodiscard]] Span<const PropertyInfo> properties() const noexcept { return m_properties; }
    [[nodiscard]] const PropertyInfo &property(std::size_t index) const noexcept;

    [[nodiscard]] std::optional<std::size_t> indexOf(const Name &name) const noexcept;
    // Hashes `name` first; for parsers holding text rather than a Name.
    [[nodiscard]] std::optional<std::size_t> indexOf(StringView name) const noexcept;
    [[nodiscard]] const PropertyInfo *find(const Name &name) const noexcept;

private:
    ClassInfo() = default;
    void buildIndex();
    [[nodiscard]] std::optional<std::size_t> lookup(std::uint64_t hash, StringView text) const noexcept;

    Name m_name;
    std::vector<PropertyInfo> m_properties;
    // Open addressing, linear probing, power-of-two size, load factor <= 0.5.
    // Each slot is a property index + 1; 0 marks an empty slot.
    std::vector<std::uint32_t> m_slots;
};

} // namespace cfw
