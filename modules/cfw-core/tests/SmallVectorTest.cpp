// SmallVector: inline storage, spilling to the heap, copies and moves in both
// states, and that every constructed element is destroyed exactly once.

#include "cfw/core/SmallVector.h"

#include <memory>
#include <string>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

// Counts live instances so leaks and double-destroys show up.
struct Tracked {
    static inline int live = 0;
    int value = 0;
    Tracked() { ++live; }
    Tracked(int v) : value(v) { ++live; }
    Tracked(const Tracked &o) : value(o.value) { ++live; }
    Tracked(Tracked &&o) noexcept : value(o.value) { ++live; }
    Tracked &operator=(const Tracked &) = default;
    Tracked &operator=(Tracked &&) noexcept = default;
    ~Tracked() { --live; }
    friend bool operator==(const Tracked &a, const Tracked &b) { return a.value == b.value; }
};

void staysInlineUpToN() {
    SmallVector<int, 4> v;
    for (int i = 0; i < 4; ++i) {
        v.push_back(i);
    }
    check(v.isInline(), "four elements fit inline");
    v.push_back(4);
    check(!v.isInline(), "the fifth spills to the heap");
    checkEqual(v.size(), std::size_t(5), "size after spill");
    for (int i = 0; i < 5; ++i) {
        checkEqual(v[static_cast<std::size_t>(i)], i, "elements survive the spill");
    }
}

void copiesAndMovesInBothStates() {
    for (int count : {2, 9}) {
        {
            SmallVector<Tracked, 4> original;
            for (int i = 0; i < count; ++i) {
                original.emplace_back(i);
            }
            SmallVector<Tracked, 4> copy = original;
            check(copy == original, "copy equals original");

            SmallVector<Tracked, 4> moved = std::move(original);
            check(moved == copy, "moved-to equals the copy");
            check(original.empty() && original.isInline(), "moved-from is empty and inline"); // NOLINT

            SmallVector<Tracked, 4> assigned{Tracked(100)};
            assigned = copy;
            check(assigned == copy, "copy assignment replaces the contents");
            assigned = std::move(moved);
            check(assigned == copy, "move assignment replaces the contents");
        }
        checkEqual(Tracked::live, 0, "every element destroyed exactly once");
    }
}

void copyAssignOverHeapStorageDoesNotLeak() {
    {
        SmallVector<Tracked, 2> big;
        for (int i = 0; i < 10; ++i) {
            big.emplace_back(i);
        }
        SmallVector<Tracked, 2> other;
        for (int i = 0; i < 20; ++i) {
            other.emplace_back(i);
        }
        big = other; // both on the heap
        checkEqual(big.size(), std::size_t(20), "assigned size");
    }
    checkEqual(Tracked::live, 0, "heap-to-heap assignment destroys everything");
}

void pushBackOfOwnElementSurvivesGrowth() {
    SmallVector<std::string, 2> v{"first-long-enough-to-allocate-on-the-heap", "second"};
    v.push_back(v[0]); // grows while the argument refers into the old storage
    checkEqual(v[2], std::string("first-long-enough-to-allocate-on-the-heap"), "self-reference is safe");
}

void eraseResizeAndPop() {
    SmallVector<int, 3> v{1, 2, 3, 4};
    v.erase(v.begin() + 1);
    checkEqual(v.size(), std::size_t(3), "erase shrinks");
    checkEqual(v[1], 3, "erase shifts later elements");
    v.resize(5);
    checkEqual(v[4], 0, "resize value-initialises");
    v.resize(1);
    checkEqual(v.back(), 1, "resize down keeps the front");
    v.pop_back();
    check(v.empty(), "pop_back to empty");
}

void holdsMoveOnlyTypes() {
    SmallVector<std::unique_ptr<int>, 1> v;
    v.push_back(std::make_unique<int>(1));
    v.push_back(std::make_unique<int>(2)); // spills: must move, not copy
    checkEqual(*v[1], 2, "move-only elements survive growth");
}

} // namespace

int main() {
    staysInlineUpToN();
    copiesAndMovesInBothStates();
    copyAssignOverHeapStorageDoesNotLeak();
    pushBackOfOwnElementSurvivesGrowth();
    eraseResizeAndPop();
    holdsMoveOnlyTypes();
    return cfw::test::finish("SmallVectorTest");
}
