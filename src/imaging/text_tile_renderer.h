#pragma once

#include "formats/styled_text.h"
#include "render/core/color.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Styled documents laid out with Pango and drawn with Cairo into RGBA tiles, in memory.
namespace luciole {
  inline constexpr int kTextTileHeight = 256;    // device pixels; the last tile may be shorter
  inline constexpr int kMaxTextTileWidth = 4096; // device pixels; code wider than this is clipped with a mark
  inline constexpr std::size_t kMaxTextTiles = 512;
  inline constexpr std::uint64_t kMaxTextPixels = 256ULL * 1024ULL * 1024ULL; // width times height of a layout

  // Colors the caller resolves; nothing here reads the palette.
  struct TextTileTheme {
    std::array<Color, formats::kStyleRoleCount> roles{}; // text color of each formats::StyleRole
    Color background{};
    Color codeBackground{}; // code blocks and inline code
    Color outline{};        // rules, table grids, quote bars and swatch borders
    Color checkerLight{};   // under translucent swatches
    Color checkerDark{};
  };

  struct TextTileFonts {
    std::string prose = "sans-serif";
    std::string code = "monospace";
    double proseSize = 13.0; // logical pixels
    double codeSize = 12.0;
  };

  struct TextTileOptions {
    double width = 320.0; // logical pixels; prose wraps here
    double scale = 1.0;   // device pixels per logical pixel
    TextTileTheme theme{};
    TextTileFonts fonts{};
  };

  // A line of a block, and a column in code points of its text. Table cells are the block's lines.
  struct TextPosition {
    std::size_t block = 0;
    std::size_t line = 0;
    std::size_t column = 0;
    bool operator==(const TextPosition&) const = default;
  };

  // Device pixels from the document's top left.
  struct TextBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
  };

  struct TextSwatchBox {
    int x = 0; // device pixels from the document's top left
    int y = 0;
    int size = 0;
    formats::Rgba color{};
  };

  struct TextTile {
    int top = 0; // device pixels from the document's top
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;      // premultiplied, width * 4 bytes per row, top row first
    std::vector<TextSwatchBox> swatches; // swatches drawn in this tile
  };

  // One document at one width, scale and theme. Code lines keep their spacing in the monospace family and widen the
  // layout up to kMaxTextTileWidth; prose wraps at the width in the interface family. Table columns narrow to fit the
  // width, and a table that needs more at its narrowest widens the layout as code does. List item text sits past the
  // widest marker of its nesting level. The layout's height stops at kMaxTextTiles tiles and at kMaxTextPixels, and
  // truncated() says so, as it does for a table cut at kMaxTextTileWidth. Swatches sit in a gap the layout keeps
  // before each color code; document columns do not count it.
  //
  // A layout owns its own Pango font map and context. Use it from one thread at a time: two layouts may render on two
  // threads at once, but one layout must not be shared between threads without the caller serializing every call.
  // Runs hold valid UTF-8, as styledText makes them. Invalid width, scale or font sizes are clamped.
  class TextTileLayout {
  public:
    TextTileLayout(std::shared_ptr<const formats::StyledDocument> document, const TextTileOptions& options);
    ~TextTileLayout();
    TextTileLayout(const TextTileLayout&) = delete;
    TextTileLayout& operator=(const TextTileLayout&) = delete;
    TextTileLayout(TextTileLayout&&) noexcept;
    TextTileLayout& operator=(TextTileLayout&&) noexcept;

    // Device pixels.
    int width() const;
    int height() const;
    std::size_t tileCount() const;
    bool truncated() const;

    // Rows [index * kTextTileHeight, its end) of the document; an error for an index past tileCount().
    std::expected<TextTile, std::string> renderTile(std::size_t index);

    // The box of the character at the position, or a caret-wide box at the line's end; none for a position the
    // document or the layout's height does not hold.
    std::optional<TextBox> characterBox(const TextPosition& position);

    // The position nearest the point on the line under it; none between lines or outside the layout.
    std::optional<TextPosition> positionAt(double x, double y);

    // Device pixels: the top of each line and table cell in document order, the cells of a row sharing their row's
    // top, for those that start within the height.
    std::vector<int> lineTops() const;

  private:
    struct State;
    std::unique_ptr<State> m_state;
  };
} // namespace luciole
