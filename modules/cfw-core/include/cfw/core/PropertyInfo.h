#pragma once

#include <cstdint>
#include <vector>

#include "cfw/core/Name.h"
#include "cfw/core/String.h"
#include "cfw/core/Variant.h"

namespace cfw {

enum class PropertyFlags : std::uint8_t {
    None = 0,
    ReadOnly = 1 << 0,  // shown, but the Properties panel offers no editor
    Hidden = 1 << 1,    // not shown in the Properties panel
    Transient = 1 << 2, // never saved (runtime state such as "IsPlaying")
};

[[nodiscard]] constexpr PropertyFlags operator|(PropertyFlags a, PropertyFlags b) noexcept {
    return static_cast<PropertyFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr bool hasFlag(PropertyFlags set, PropertyFlags flag) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}

// One declared property: everything the Properties panel, the serialiser and
// the undo system need to know about it. Declared once per class.
//
// A "choice" property is a String property with a non-empty `choices` list;
// only those values may be stored.
//
// Threads: immutable once part of a built ClassInfo. Allocates: its strings.
struct PropertyInfo {
    Name name;
    VariantType type = VariantType::String;
    Variant defaultValue;
    // Properties panel grouping ("Appearance", "Text"). Empty = the class's own name.
    Name section;
    std::vector<String> choices;
    PropertyFlags flags = PropertyFlags::None;

    [[nodiscard]] bool isChoice() const noexcept { return !choices.empty(); }
    [[nodiscard]] bool isSaved() const noexcept { return !hasFlag(flags, PropertyFlags::Transient); }
};

} // namespace cfw
