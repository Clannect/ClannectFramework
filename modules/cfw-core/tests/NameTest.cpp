// Name: compile-time literal names, runtime names, and that copies and moves
// never leave a view pointing at another object's buffer.

#include "cfw/core/Name.h"

#include <memory>
#include <unordered_map>
#include <utility>

#include "cfw/core/FlatMap.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

// Constant-initialised: no static initialisation code runs for these.
constexpr Name kPosition = "Position";
constexpr Name kSize = "Size";

static_assert(kPosition.hash() == hashString("Position"), "literal names hash at compile time");
static_assert(kPosition != kSize, "different literals differ");

void literalAndRuntimeNamesCompareEqual() {
    const String text = "Position";
    const Name runtime(text);
    check(runtime == kPosition, "runtime and literal names with the same text are equal");
    checkEqual(runtime.hash(), kPosition.hash(), "same hash");
    check(runtime == StringView("Position"), "compares with text");
    check(runtime != Name(StringView("position")), "case-sensitive");
}

void copiesOwnTheirText() {
    // Long enough to live on the heap, and short enough for the small buffer.
    for (const char *text : {"ThisNameIsLongerThanTheSmallStringBuffer", "Tiny"}) {
        auto original = std::make_unique<Name>(StringView(text));
        const Name copy = *original;
        Name assigned;
        assigned = *original;
        original.reset();
        checkEqual(copy.view(), StringView(text), "copy survives the original");
        checkEqual(assigned.view(), StringView(text), "assignment survives the original");
    }
}

void movesLeaveTheSourceEmpty() {
    Name source(StringView("Tiny")); // small-string: the buffer moves with the object
    const Name moved = std::move(source);
    checkEqual(moved.view(), StringView("Tiny"), "moved-to keeps the text");
    check(source.empty(), "moved-from is empty"); // NOLINT(bugprone-use-after-move)
    check(source == Name(), "moved-from equals the empty name");

    Name literal = kSize;
    Name target;
    target = std::move(literal);
    check(target == kSize, "moving a literal name keeps the literal");
}

void worksAsAMapKey() {
    FlatMap<Name, int> flat;
    flat.insertOrAssign(kPosition, 1);
    flat.insertOrAssign(Name(StringView("Size")), 2);
    checkEqual(*flat.get(kSize), 2, "flat map finds a runtime key with a literal");

    std::unordered_map<Name, int, NameHash> hashed;
    hashed[kPosition] = 7;
    checkEqual(hashed[Name(StringView("Position"))], 7, "unordered_map with NameHash");
}

void emptyNamesAreEqual() {
    check(Name() == Name(StringView()), "default and empty runtime names are equal");
    check(Name().empty(), "default name is empty");
}

} // namespace

int main() {
    literalAndRuntimeNamesCompareEqual();
    copiesOwnTheirText();
    movesLeaveTheSourceEmpty();
    worksAsAMapKey();
    emptyNamesAreEqual();
    return cfw::test::finish("NameTest");
}
