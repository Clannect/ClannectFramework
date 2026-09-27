#pragma once

#include <cstddef>
#include <vector>

#include "cfw/core/ClassInfo.h"
#include "cfw/core/FlatMap.h"
#include "cfw/core/Name.h"
#include "cfw/core/Result.h"
#include "cfw/core/Variant.h"

namespace cfw {

// The property values of one object, stored against its ClassInfo. Replaces
// the engine's QVariantMap bag.
//
// Declared properties live in a vector indexed by the schema's property index:
// get() and set() by Name are a hash probe plus an index, with no allocation.
// A property never set reads as its declared default and takes no storage
// beyond its (null) slot; the slot vector itself is only allocated on the first
// set(), so an untouched object costs nothing.
//
// Undeclared keys ("extras") are kept separately, so a scene saved by a newer
// engine, whose classes have properties this build does not know, still loads
// and saves back without losing them. Only the loader should add extras; code
// setting a declared property by a misspelt name gets NotFound, not a silent
// extra.
//
// Threads: one thread at a time (the owning object's).
// Allocates: the slot vector on first set; values that allocate themselves.
class PropertyBag {
public:
    // `schema` is borrowed and must outlive the bag.
    explicit PropertyBag(const ClassInfo &schema) noexcept : m_schema(&schema) {}

    [[nodiscard]] const ClassInfo &schema() const noexcept { return *m_schema; }

    // The value: explicitly set, else the declared default, else the extra
    // with that name, else Null.
    [[nodiscard]] const Variant &get(const Name &name) const noexcept;
    [[nodiscard]] const Variant &valueAt(std::size_t index) const noexcept;

    // Sets a declared property. Returns whether the value changed (setting a
    // property to its current value is a no-op, so undo and change
    // notification can rely on it).
    //
    // Fails, leaving the bag unchanged, with NotFound for an undeclared name,
    // TypeMismatch for a value of the wrong type, or InvalidArgument for a
    // choice value that is not one of the choices. An Int is accepted for a
    // Number (Double) property and converted.
    Result<bool> set(const Name &name, Variant value);
    Result<bool> setAt(std::size_t index, Variant value);

    // True if the property was explicitly set (even to its default value).
    [[nodiscard]] bool isSet(const Name &name) const noexcept;
    // Back to the declared default. Returns whether the effective value changed.
    bool reset(const Name &name) noexcept;

    // Undeclared values, in Name order. Only loaders and migrations use these.
    [[nodiscard]] const FlatMap<Name, Variant> &extras() const noexcept { return m_extras; }
    void setExtra(Name name, Variant value);
    bool removeExtra(const Name &name) { return m_extras.erase(name); }

private:
    const ClassInfo *m_schema;
    std::vector<Variant> m_values; // empty until the first set; Null = default
    FlatMap<Name, Variant> m_extras;
};

} // namespace cfw
