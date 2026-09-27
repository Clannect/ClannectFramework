// FlatMap and FlatSet: ordering, insert/replace/erase, and lookups by a type
// other than the key (StringView into a map of String) without building a key.

#include "cfw/core/FlatMap.h"
#include "cfw/core/FlatSet.h"

#include "cfw/core/String.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void keepsKeysSorted() {
    FlatMap<int, String> map;
    map.insertOrAssign(3, "c");
    map.insertOrAssign(1, "a");
    map.insertOrAssign(2, "b");
    String order;
    for (const auto &[key, value] : map) {
        order += value;
    }
    checkEqual(order, String("abc"), "iteration is in key order");
}

void insertReplaceAndErase() {
    FlatMap<String, int> map;
    check(map.insertOrAssign("x", 1), "first insert is new");
    check(!map.insertOrAssign("x", 2), "second insert replaces");
    checkEqual(*map.get("x"), 2, "value was replaced");

    const auto [it, inserted] = map.tryEmplace("x", 99);
    check(!inserted && it->second == 2, "tryEmplace leaves an existing value alone");

    map["y"] += 5;
    checkEqual(*map.get("y"), 5, "operator[] default-constructs");

    check(map.erase("x"), "erase existing key");
    check(!map.erase("x"), "erase missing key");
    check(map.get("x") == nullptr, "erased key is gone");
    checkEqual(map.size(), std::size_t(1), "size after erase");
}

void findsWithoutBuildingAKey() {
    FlatMap<String, int> map;
    map.insertOrAssign("Position", 1);
    const StringView key = "Position";
    check(map.contains(key), "heterogeneous lookup by StringView");
    check(!map.contains(StringView("Pos")), "prefix is not a match");
}

void setsDeduplicate() {
    FlatSet<int> set;
    check(set.insert(2), "insert new");
    check(!set.insert(2), "insert duplicate");
    set.insert(1);
    checkEqual(*set.begin(), 1, "ordered");
    check(set.contains(2), "contains");
    check(set.erase(2) && !set.contains(2), "erase");
}

} // namespace

int main() {
    keepsKeysSorted();
    insertReplaceAndErase();
    findsWithoutBuildingAKey();
    setsDeduplicate();
    return cfw::test::finish("FlatMapTest");
}
