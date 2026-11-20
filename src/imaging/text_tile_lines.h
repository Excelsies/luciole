#pragma once

#include "formats/styled_text.h"
#include "fs/gio_ptr.h"
#include "imaging/text_tile_renderer.h"

#include <cairo.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <pango/pangocairo.h>
#include <string>
#include <string_view>
#include <vector>

// One styled line as a Pango layout, its geometry, and the squares drawn beside its text, for TextTileLayout and
// TextLineStrip.
namespace luciole::text_tiles {
  inline constexpr double kMinScale = 0.5;
  inline constexpr double kMaxScale = 4.0;
  inline constexpr double kMinFont = 4.0; // logical pixels
  inline constexpr double kMaxFont = 200.0;

  inline int pixels(double value) { return static_cast<int>(std::lround(value)); }

  // NaN takes the fallback; everything else, infinities included, is clamped.
  double clampOr(double value, double low, double high, double fallback);

  // The device pixels of a font size in logical pixels at a scale already clamped; an invalid size takes the fallback.
  double emOf(double size, double fallback, double scale);

  // The width of rules and swatch borders in device pixels.
  int ruleWidth(double scale);

  // A context on the font map with the antialiasing, hinting and unrounded glyph positions every text tile uses.
  GioPtr<PangoContext> contextOf(PangoFontMap* fontMap);

  // Converts a Cairo ARGB32 surface of the given size to premultiplied RGBA bytes, top row first.
  void copyPixels(cairo_surface_t* surface, int width, int height, std::vector<std::uint8_t>& rgba);

  struct FontFree {
    void operator()(PangoFontDescription* font) const { pango_font_description_free(font); }
  };

  using FontPtr = std::unique_ptr<PangoFontDescription, FontFree>;

  // A font of the given family and size in device pixels, bold when asked.
  FontPtr fontOf(const std::string& family, double pixels, bool bold);

  // How a line is set: its base font and em in device pixels, the wrap width (none below 1) and alignment.
  struct LineFace {
    const PangoFontDescription* font = nullptr;
    double em = 0.0;
    int wrapWidth = 0;
    PangoAlignment align = PANGO_ALIGN_LEFT;
  };

  PangoAlignment alignmentOf(formats::CellAlign align);

  struct LineLayout {
    GioPtr<PangoLayout> layout;
    std::vector<int> columnBytes; // layout byte of each column, then of the line's end
    std::vector<int> swatchBytes; // layout byte of each swatch's gap
    int swatchSize = 0;           // device pixels
    int swatchGap = 0;            // device pixels kept before a color code
  };

  // The layout of a line in the context: run colors from the theme, inline code in the code family, a kept gap before
  // each color code, and an ellipsis after a line that continues.
  LineLayout lineLayout(
      PangoContext* context, const formats::StyledLine& line, const LineFace& face, const TextTileTheme& theme,
      const std::string& codeFamily
  );

  // Text in one font, without attributes: a mark or a list marker.
  GioPtr<PangoLayout> plainLayout(PangoContext* context, const PangoFontDescription* font, std::string_view text);

  // The layout's logical extents in device pixels.
  PangoRectangle logicalBox(PangoLayout* layout);

  // Device pixels of the widest stretch of the line between two line break opportunities, without its trailing white
  // space: the narrowest wrap width that breaks no word. The line is laid out without a wrap width.
  int longestWord(const LineLayout& line);

  // The column at a layout byte: a byte in a swatch gap belongs to the color code after it, a byte past the line's
  // text to its end. `trailing` moves past the character.
  std::size_t columnAt(const LineLayout& line, int byte, int trailing);

  // The box of a column with the layout at the origin; a column at the line's end gives a box without width. None
  // past the end.
  std::optional<TextBox> characterBox(const LineLayout& line, std::size_t column, double originX, double originY);

  // The swatch square of a gap, with the layout at the origin.
  struct SwatchSquare {
    int x = 0;
    int y = 0;
    int size = 0;
  };

  SwatchSquare swatchSquare(const LineLayout& line, std::size_t swatch, double originX, double originY);

  void fillRect(cairo_t* cr, const Color& color, int x, int y, int width, int height);

  // The square in the swatch's color with a border of `border` pixels, on the theme's checkerboard when translucent.
  void drawSwatch(
      cairo_t* cr, const SwatchSquare& square, const formats::Rgba& color, const TextTileTheme& theme, int border
  );
} // namespace luciole::text_tiles
