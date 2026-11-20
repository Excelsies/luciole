#pragma once

#include "formats/styled_text.h"
#include "imaging/text_tile_renderer.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

// One line of code drawn on its own, exactly as a code line of a text tile draws it.
namespace luciole {
  struct TextLineStripOptions {
    double scale = 1.0;    // device pixels per logical pixel
    TextTileTheme theme{}; // role, outline and checker colors; the background is the caller's
    TextTileFonts fonts{}; // lines are set in the code family at the code size
  };

  // Columns [begin, end) of a line; empty when end is not past begin.
  struct ColumnRange {
    std::size_t begin = 0;
    std::size_t end = 0;
  };

  struct TextStrip {
    int x0 = 0; // device pixels from the line's start
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // premultiplied, width * 4 bytes per row, transparent where nothing is drawn
  };

  // Draws lines of code one at a time and answers where a caret goes on them, in device pixels from the line's start.
  // Columns are code points of the line's runs, as StyledLine swatches count them. A swatch takes a gap before its
  // column, as in a tile, and the gap belongs to that column. A caret at a column sits on the leading edge of the glyph
  // drawn for it, the right edge in right-to-left text, where the place between two runs of opposite direction stands
  // for two columns; a column inside a grapheme cluster takes the cluster's start, so no caret lands inside one.
  // Every line has the same height and baseline. A line stops at the first column whose glyph would end past about a
  // million device pixels, where Pango's positions run out; columns from there on are neither drawn nor reached.
  //
  // The shaped layout of the line asked about last is kept, so repeated questions about one line do not shape it
  // again. An object owns its own Pango font map; use it from one thread. Runs hold valid UTF-8, as styledText makes
  // them. Invalid scale or font sizes are clamped.
  class TextLineStrip {
  public:
    explicit TextLineStrip(const TextLineStripOptions& options);
    ~TextLineStrip();
    TextLineStrip(const TextLineStrip&) = delete;
    TextLineStrip& operator=(const TextLineStrip&) = delete;
    TextLineStrip(TextLineStrip&&) noexcept;
    TextLineStrip& operator=(TextLineStrip&&) noexcept;

    // Device pixels of every line.
    int lineHeight() const;

    // Device pixels from the line's start to the right edge of its last glyph.
    int lineWidth(const formats::StyledLine& line);

    // The line's pixels in [x0, x0 + width), the width clamped to [0, kMaxTextTileWidth]. A window past the line's
    // ends is transparent. The composing columns are underlined. An error only when Cairo cannot draw.
    std::expected<TextStrip, std::string>
    render(const formats::StyledLine& line, int x0, int width, ColumnRange composing = {});

    // The caret's x at a column; a column past the line's end takes the end.
    double caretX(const formats::StyledLine& line, std::size_t column);

    // The caret column nearest x: 0 before the line and for NaN, the line's end after it.
    std::size_t columnAt(const formats::StyledLine& line, double x);

  private:
    struct State;
    std::unique_ptr<State> m_state;
  };
} // namespace luciole
