// ClassInfo and PropertyBag: schema validation, O(1) lookup that survives
// hash-table collisions, defaults, type enforcement, choices, change detection,
// and preservation of unknown keys from newer files.

#include "cfw/core/ClassInfo.h"
#include "cfw/core/PropertyBag.h"

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

constexpr Name kSize = "Size";
constexpr Name kAnchored = "Anchored";
constexpr Name kTransparency = "Transparency";
constexpr Name kMaterial = "Material";
constexpr Name kFace = "Face";

ClassInfo makePart() {
    Result<ClassInfo> built = ClassInfo::Builder("Part")
                                  .property({.name = kSize, .type = VariantType::Vec3, .defaultValue = Vec3{4, 1, 2}})
                                  .property({.name = kAnchored, .type = VariantType::Bool, .defaultValue = false})
                                  .property({.name = kTransparency, .type = VariantType::Double, .defaultValue = 0})
                                  .property({.name = kMaterial,
                                             .type = VariantType::String,
                                             .defaultValue = "Plastic",
                                             .section = "Appearance"})
                                  .property({.name = kFace,
                                             .type = VariantType::String,
                                             .defaultValue = "Front",
                                             .choices = {"Top", "Bottom", "Front", "Back", "Left", "Right"}})
                                  .build();
    check(built.ok(), "Part schema builds");
    return std::move(built).value();
}

void schemaRejectsMistakes() {
    auto wrongDefault = ClassInfo::Builder("X").property({.name = "A", .type = VariantType::Bool, .defaultValue = 3}).build();
    check(!wrongDefault.ok() && wrongDefault.error().code() == ErrorCode::InvalidArgument, "wrong default type");

    auto duplicate = ClassInfo::Builder("X")
                         .property({.name = "A", .type = VariantType::Bool, .defaultValue = true})
                         .property({.name = "A", .type = VariantType::Bool, .defaultValue = true})
                         .build();
    check(!duplicate.ok(), "duplicate name");

    auto badChoice = ClassInfo::Builder("X")
                         .property({.name = "A", .type = VariantType::String, .defaultValue = "Nope", .choices = {"Yes"}})
                         .build();
    check(!badChoice.ok(), "choice default must be a choice");

    auto unnamed = ClassInfo::Builder("X").property({.type = VariantType::Bool, .defaultValue = true}).build();
    check(!unnamed.ok(), "empty name");
}

void lookupIsExactEvenWithCollisions() {
    const ClassInfo part = makePart();
    checkEqual(part.indexOf(kFace).value_or(99), std::size_t(4), "index of a declared property");
    checkEqual(part.indexOf(StringView("Material")).value_or(99), std::size_t(3), "lookup by text");
    check(!part.indexOf(Name("Colour")).has_value(), "unknown name");
    checkEqual(part.property(3).section, Name("Appearance"), "section kept");

    // A big class forces collisions and wrap-around in the probe table.
    ClassInfo::Builder builder("Big");
    std::vector<String> names;
    for (int i = 0; i < 300; ++i) {
        names.push_back("Property" + std::to_string(i));
    }
    for (const String &n : names) {
        builder.property({.name = Name(n), .type = VariantType::Int, .defaultValue = 0});
    }
    const ClassInfo big = std::move(builder).build().value();
    int misses = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        misses += big.indexOf(Name(names[i])) == i ? 0 : 1;
    }
    checkEqual(misses, 0, "every one of 300 properties is found at its index");
    check(!big.indexOf(Name(StringView("Property300"))).has_value(), "and nothing else is");
}

void inheritanceCopiesBaseProperties() {
    const ClassInfo base = makePart();
    const ClassInfo mesh = ClassInfo::Builder("MeshPart")
                               .inherit(base)
                               .property({.name = "MeshId", .type = VariantType::AssetLink, .defaultValue = AssetLink()})
                               .build()
                               .value();
    checkEqual(mesh.propertyCount(), std::size_t(6), "base properties plus its own");
    check(mesh.find(kSize) != nullptr, "inherited property is found");
}

void unsetPropertiesReadAsDefaults() {
    const ClassInfo part = makePart();
    const PropertyBag bag(part);
    checkEqual(bag.get(kSize), Variant(Vec3{4, 1, 2}), "default Vec3");
    checkEqual(bag.get(kMaterial), Variant("Plastic"), "default String");
    check(!bag.isSet(kSize), "defaults are not 'set'");
    check(bag.get(Name(StringView("Nope"))).isNull(), "unknown reads as Null");
}

void setEnforcesTypesAndChoices() {
    const ClassInfo part = makePart();
    PropertyBag bag(part);

    const Result<bool> changed = bag.set(kSize, Vec3{1, 2, 3});
    check(changed.ok() && changed.value(), "set a new value reports a change");
    checkEqual(bag.get(kSize), Variant(Vec3{1, 2, 3}), "value stored");

    const Result<bool> same = bag.set(kSize, Vec3{1, 2, 3});
    check(same.ok() && !same.value(), "setting the same value is not a change");

    const Result<bool> wrongType = bag.set(kAnchored, "yes");
    check(!wrongType.ok() && wrongType.error().code() == ErrorCode::TypeMismatch, "wrong type rejected");
    checkEqual(bag.get(kAnchored), Variant(false), "rejected set leaves the value alone");

    check(bag.set(kTransparency, 1).ok(), "Int accepted for a Number property");
    checkEqual(bag.get(kTransparency), Variant(1.0), "and stored as Double");

    check(bag.set(kFace, "Top").ok(), "valid choice");
    const Result<bool> badChoice = bag.set(kFace, "Diagonal");
    check(!badChoice.ok() && badChoice.error().code() == ErrorCode::InvalidArgument, "invalid choice rejected");

    const Result<bool> typo = bag.set(Name(StringView("Anchord")), true);
    check(!typo.ok() && typo.error().code() == ErrorCode::NotFound, "misspelt property is an error, not an extra");
}

void settingTheDefaultExplicitlyIsNotAChange() {
    const ClassInfo part = makePart();
    PropertyBag bag(part);
    const Result<bool> r = bag.set(kAnchored, false);
    check(r.ok() && !r.value(), "explicitly setting the default value changes nothing visible");
    check(bag.isSet(kAnchored), "but the property now counts as set");
    check(!bag.reset(kAnchored), "resetting a default-valued property is not a change");
    check(!bag.isSet(kAnchored), "reset clears 'set'");
    check(bag.set(kAnchored, true).value(), "set to non-default");
    check(bag.reset(kAnchored), "reset back to default is a change");
}

void extrasSurviveForNewerFiles() {
    const ClassInfo part = makePart();
    PropertyBag bag(part);
    bag.setExtra(Name(StringView("FutureFeature")), 42);
    checkEqual(bag.get(Name(StringView("FutureFeature"))), Variant(42), "extra readable by name");
    checkEqual(bag.extras().size(), std::size_t(1), "one extra");
    check(bag.removeExtra(Name(StringView("FutureFeature"))), "extra removable");
}

} // namespace

int main() {
    schemaRejectsMistakes();
    lookupIsExactEvenWithCollisions();
    inheritanceCopiesBaseProperties();
    unsetPropertiesReadAsDefaults();
    setEnforcesTypesAndChoices();
    settingTheDefaultExplicitlyIsNotAChange();
    extrasSurviveForNewerFiles();
    return cfw::test::finish("PropertyTest");
}
