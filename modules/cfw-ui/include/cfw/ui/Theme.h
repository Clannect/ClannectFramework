#pragma once

// Theme tokens: colours, spacing, radii and fonts as data (no
// stylesheet language). Controls read tokens by meaning, never raw colours.
//
// Threads: a plain value; share one per surface.

#include <memory>

#include "cfw/core/Color.h"

namespace cfw {

class FontDatabase;
class FontFace;

struct Theme {
    // Surfaces
    Color window;        // behind everything
    Color panel;         // docks, toolbars
    Color control;       // buttons, fields at rest
    Color controlHover;
    Color controlPressed;
    Color controlDisabled;
    Color border;
    Color accent;        // focus rings, selection, primary buttons
    Color accentText;    // text on accent
    Color scrim;         // over the window behind a modal dialog
    Color error;         // a field whose text is not accepted
    Color success;       // a finished upload, a healthy status dot
    Color warning;       // needs attention, not an error
    Color info;          // neutral highlights (a second series in a meter)
    Color controlRaised; // a popup, a toast, a selected segment: above the panel
    // Text
    Color text;
    Color textMuted;
    Color textDisabled;
    // Metrics (logical pixels)
    float spacing = 6.0f;
    float padding = 8.0f;
    float radius = 4.0f;
    float radiusLarge = 8.0f; // cards, dialogs, toasts
    float focusRingWidth = 1.5f;
    float fontSize = 13.0f;
    float controlHeight = 26.0f;
    // Animations (a switch's knob, a toast sliding in) jump straight to
    // their end when set: the user asked the system for less motion.
    bool reducedMotion = false;
    // Fonts; null means text is measured as empty and not drawn.
    std::shared_ptr<const FontFace> font;
    std::shared_ptr<const FontFace> monoFont; // logs, code; null: the font above
    std::shared_ptr<const FontFace> strongFont; // semibold labels; null: the font above
    std::shared_ptr<const FontFace> titleFont;  // headings; null: the font above
    std::shared_ptr<FontDatabase> fonts; // fallback for missing characters

    // Clannect's editor palettes. Fonts are left empty; see withSystemFonts().
    [[nodiscard]] static Theme dark();
    [[nodiscard]] static Theme light();
    // The theme with the platform's interface font (Segoe UI, SF, DejaVu...).
    [[nodiscard]] Theme withSystemFonts() const;
};

} // namespace cfw
