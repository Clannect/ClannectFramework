// The preedit an input method draws (XIM on-the-spot): replacing ranges,
// deleting, the caret, clamping bad ranges, and the event it becomes.

#include "../src/Preedit.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::detail::Preedit;
using cfw::test::check;
using cfw::test::checkEqual;

int main() {
    Preedit preedit;
    preedit.draw(0, 0, U"ni", 2);
    checkEqual(preedit.event().text, String("ni"), "typing starts a preedit");
    preedit.draw(2, 0, U"h", 3);
    preedit.draw(0, 3, U"にほ", 2); // the input method converts to kana
    checkEqual(preedit.event().text, String("にほ"), "a range is replaced");
    checkEqual(preedit.event().cursor, std::size_t(6), "the caret is a byte offset (two 3-byte characters)");
    preedit.moveCaret(-1);
    checkEqual(preedit.event().cursor, std::size_t(3), "the caret moves back one character");
    preedit.draw(1, 1, U"", 1);
    checkEqual(preedit.event().text, String("に"), "a deletion");
    preedit.draw(5, 9, U"x", 99);
    checkEqual(preedit.event().text, String("にx"), "out-of-range edits are clamped");
    checkEqual(preedit.caret(), std::size_t(2), "and so is the caret");
    preedit.clear();
    check(preedit.event().text.empty() && preedit.event().cursor == 0, "cleared, the composition ends");
    return cfw::test::finish("PreeditTest");
}
