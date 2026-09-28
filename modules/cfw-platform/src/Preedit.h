#pragma once

// An input method's preedit (the text being composed), as XIM's on-the-spot
// callbacks describe it: edits replacing a range of characters, and a caret.
// Kept apart from Xlib so it can be tested anywhere.

#include <algorithm>
#include <string>

#include "cfw/core/Input.h"
#include "cfw/core/Utf8.h"

namespace cfw::detail {

class Preedit {
public:
    // Replaces `length` characters at `first` with `insert` (XIM's
    // PreeditDraw: chg_first, chg_length, text) and puts the caret at
    // `caret`. Out-of-range values are clamped: input methods get them wrong.
    void draw(int first, int length, std::u32string_view insert, int caret) {
        const std::size_t from = std::min(std::size_t(std::max(first, 0)), m_text.size());
        const std::size_t count = std::min(std::size_t(std::max(length, 0)), m_text.size() - from);
        m_text.replace(from, count, insert);
        setCaret(caret);
    }
    void setCaret(int caret) { m_caret = std::min(std::size_t(std::max(caret, 0)), m_text.size()); }
    void moveCaret(int by) { setCaret(int(m_caret) + by); }
    void clear() {
        m_text.clear();
        m_caret = 0;
    }
    [[nodiscard]] const std::u32string &text() const noexcept { return m_text; }
    [[nodiscard]] std::size_t caret() const noexcept { return m_caret; }

    // As the event the window sends (UTF-8, the caret as a byte offset).
    [[nodiscard]] CompositionEvent event() const {
        CompositionEvent e;
        for (std::size_t i = 0; i < m_text.size(); ++i) {
            if (i == m_caret) {
                e.cursor = e.text.size();
            }
            appendUtf8(e.text, m_text[i]);
        }
        if (m_caret >= m_text.size()) {
            e.cursor = e.text.size();
        }
        return e;
    }

private:
    std::u32string m_text;
    std::size_t m_caret = 0;
};

} // namespace cfw::detail
