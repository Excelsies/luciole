#pragma once

#include <pango/pango.h>
#include <utility>
#include <vector>

// The glyphs of a one-line Pango layout that a window of it shows, for TextLineStrip.
namespace luciole::text_tiles {
  // Device pixels from the layout's top left; empty when right is not past left.
  struct InkBox {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    bool empty() const { return right <= left || bottom <= top; }
  };

  // Hides every glyph of the layout whose ink misses [from, to) in device pixels, until this goes away: the glyph
  // becomes an empty one that keeps its advance, so the others keep their places and colors. The layout must not
  // change meanwhile.
  class GlyphWindow {
  public:
    GlyphWindow(PangoLayout* layout, double from, double to);
    ~GlyphWindow();
    GlyphWindow(const GlyphWindow&) = delete;
    GlyphWindow& operator=(const GlyphWindow&) = delete;

    // The ink of the glyphs still shown.
    const InkBox& ink() const { return m_ink; }

  private:
    std::vector<std::pair<PangoGlyphInfo*, PangoGlyph>> m_hidden;
    InkBox m_ink;
  };
} // namespace luciole::text_tiles
