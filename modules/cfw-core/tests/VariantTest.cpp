// Variant: construction picks the intended type, equality is reflexive even for
// NaN, arrays stay homogeneous, and type names match the scene format.

#include "cfw/core/Variant.h"

#include <cmath>
#include <limits>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void constructionPicksTheIntendedType() {
    checkEqual(Variant().type(), VariantType::Null, "default is Null");
    checkEqual(Variant(true).type(), VariantType::Bool, "bool stays Bool");
    checkEqual(Variant(42).type(), VariantType::Int, "int becomes Int");
    checkEqual(Variant(std::uint32_t(7)).type(), VariantType::Int, "uint32 becomes Int");
    checkEqual(Variant(1.5f).type(), VariantType::Double, "float becomes Double");
    checkEqual(Variant("text").type(), VariantType::String, "a literal is a String, not a bool");
    checkEqual(Variant(StringView("view")).type(), VariantType::String, "StringView becomes String");
    checkEqual(Variant(Vec3{1, 2, 3}).type(), VariantType::Vec3, "Vec3");
    checkEqual(Variant(Name("Part")).type(), VariantType::Name, "Name");
    checkEqual(Variant(AssetLink("clannect://asset/1")).type(), VariantType::AssetLink, "AssetLink");
}

void accessorsAreTypeExact() {
    const Variant v(Vec3{1, 2, 3});
    check(v.getIf<Vec3>() != nullptr, "getIf with the right type");
    check(v.getIf<Vec2>() == nullptr, "getIf with the wrong type is null");
    checkEqual(v.valueOr(Vec3{}), Vec3{1, 2, 3}, "valueOr with the right type");
    checkEqual(Variant(5).valueOr(String("fallback")), String("fallback"), "valueOr with the wrong type");
    checkEqual(Variant(5).toNumber().value_or(-1.0), 5.0, "Int converts to a number");
    check(!Variant("5").toNumber().has_value(), "a String is never silently a number");
}

void equalityIsReflexive() {
    check(Variant(1) == Variant(1), "same Int");
    check(Variant(1) != Variant(1.0), "Int and Double are different types");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check(Variant(nan) == Variant(nan), "NaN equals NaN, so re-setting a value is a no-op");
    const float fnan = std::numeric_limits<float>::quiet_NaN();
    check(Variant(Vec3{fnan, 0, 0}) == Variant(Vec3{fnan, 0, 0}), "NaN components compare equal too");
    check(Variant(Color{1, 0, 0, 1}) != Variant(Color{1, 0, 0, 0.5f}), "alpha matters");
    check(Variant() == Variant(), "Null equals Null");
}

void arraysAreHomogeneous() {
    VariantArray array(VariantType::Int);
    check(array.append(1).ok(), "append matching type");
    check(array.append(2).ok(), "append matching type again");
    const Result<void> wrong = array.append("three");
    check(!wrong.ok() && wrong.error().code() == ErrorCode::TypeMismatch, "mismatched type is rejected");
    checkEqual(array.size(), std::size_t(2), "rejected element was not added");
    check(!array.set(5, 1).ok(), "set out of range fails");
    check(array.set(0, 10).ok(), "set in range");
    checkEqual(array[0], Variant(10), "set replaced the element");

    const Variant held(array);
    checkEqual(held.type(), VariantType::Array, "array in a Variant");
    check(held == Variant(array), "arrays compare by content");
}

void typeNamesMatchTheSceneFormat() {
    checkEqual(variantTypeName(VariantType::Vec3), StringView("Vector3"), "Vector3");
    checkEqual(variantTypeName(VariantType::Double), StringView("Number"), "Number");
    for (int i = 0; i <= static_cast<int>(VariantType::Array); ++i) {
        const auto type = static_cast<VariantType>(i);
        check(variantTypeFromName(variantTypeName(type)) == type, "every name maps back to its type");
    }
    check(!variantTypeFromName("Vector9").has_value(), "unknown names are rejected");
}

void copiesAreIndependent() {
    Variant original(String("a string long enough to live on the heap for sure"));
    Variant copy = original;
    original = Variant(1);
    checkEqual(copy, Variant(String("a string long enough to live on the heap for sure")), "copy kept its value");
}

} // namespace

int main() {
    constructionPicksTheIntendedType();
    accessorsAreTypeExact();
    equalityIsReflexive();
    arraysAreHomogeneous();
    typeNamesMatchTheSceneFormat();
    copiesAreIndependent();
    return cfw::test::finish("VariantTest");
}
