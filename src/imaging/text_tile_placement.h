#pragma once

#include "formats/styled_text.h"
#include "fs/gio_ptr.h"
#include "imaging/text_tile_lines.h"
#include "imaging/text_tile_renderer.h"

#include <algorithm>
#include <array>
#include <cairo.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <pango/pangocairo.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The state behind a TextTileLayout and where each line and decoration of its document sits, in device pixels.
namespace luciole::text_tiles {
  inline constexpr double kPadding = 8.0; // logical pixels around the document
  // Headings are bold; none is larger than the text, which a preview keeps below the list's size.
  inline constexpr std::array<double, 6> kHeadingScale{1.0, 1.0, 1.0, 1.0, 1.0, 0.9};
  inline constexpr std::string_view kMark = "…";

  // A laid-out line: its box, and where its text is drawn.
  struct Piece {
    std::uint32_t block = 0;
    std::uint32_t line = 0;
    int x = 0;
    int y = 0;
    int width = 0; // code: room before the right padding; prose: the wrap width; table: the cell
    int height = 0;
    int textX = 0;
    int textY = 0;
    int wrap = 0; // 0 for code
    bool clipped = false;

    int bottom() const { return y + height; }
  };

  enum class Paint : std::uint8_t { CodeBackground, Outline };

  struct Decoration {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    Paint paint = Paint::Outline;
  };

  struct PieceRange {
    std::size_t first = 0;
    std::size_t count = 0;
  };
} // namespace luciole::text_tiles

namespace luciole {
  struct TextTileLayout::State {
    using BlockKind = formats::BlockKind;
    using StyledBlock = formats::StyledBlock;
    using Piece = text_tiles::Piece;
    using LineLayout = text_tiles::LineLayout;

    std::shared_ptr<const formats::StyledDocument> document;
    TextTileTheme theme;
    std::string codeFamily;
    double scale = 1.0;
    int requested = 0; // device pixels prose wraps within, padding included
    GioPtr<PangoFontMap> fontMap;
    GioPtr<PangoContext> context;
    text_tiles::FontPtr codeFont;
    text_tiles::FontPtr proseFont;
    std::array<text_tiles::FontPtr, text_tiles::kHeadingScale.size()> headingFonts;
    double codeEm = 0.0;
    double proseEm = 0.0;
    std::vector<Piece> pieces;
    std::vector<text_tiles::PieceRange> blockPieces;
    std::vector<text_tiles::Decoration> decorations;
    std::array<int, 256> listLefts{}; // by list nesting: the offset of item text, past the widest marker of each level
    int width = 0;
    int height = 0;
    int markWidth = 0;
    bool truncated = false;

    // Spacing in device pixels.
    int pad() const { return text_tiles::pixels(text_tiles::kPadding * scale); }
    int rule() const { return text_tiles::ruleWidth(scale); }
    int em(double fraction) const { return text_tiles::pixels(proseEm * fraction); }
    int narrowestCell() const { return (2 * em(0.5)) + em(1.0); }

    // A table's grid with every column at its narrowest.
    int tableMinimum(const StyledBlock& block) const {
      const auto columns = static_cast<int>(std::min<std::size_t>(block.columns, kMaxTextTileWidth));
      return (columns * narrowestCell()) + ((columns + 1) * rule());
    }

    const StyledBlock& blockOf(const Piece& piece) const { return document->blocks[piece.block]; }

    int leftOf(const StyledBlock& block) const {
      return pad() + (block.quoteDepth * em(1.0)) + listLefts[block.indent];
    }

    int markerWidth(const StyledBlock& block) const {
      return text_tiles::logicalBox(text_tiles::plainLayout(context.get(), proseFont.get(), block.marker).get()).width;
    }

    text_tiles::LineFace faceOf(const StyledBlock& block, std::size_t line, int wrap) const {
      if (block.kind == BlockKind::Code) {
        return {.font = codeFont.get(), .em = codeEm, .wrapWidth = 0, .align = PANGO_ALIGN_LEFT};
      }
      if (block.heading >= 1 && block.heading <= headingFonts.size()) {
        const auto level = static_cast<std::size_t>(block.heading - 1);
        return {.font = headingFonts[level].get(), .em = proseEm * text_tiles::kHeadingScale[level], .wrapWidth = wrap};
      }
      auto align = PANGO_ALIGN_LEFT;
      if (block.kind == BlockKind::Table && block.columns > 0 && line % block.columns < block.alignments.size()) {
        align = text_tiles::alignmentOf(block.alignments[line % block.columns]);
      }
      return {.font = proseFont.get(), .em = proseEm, .wrapWidth = wrap, .align = align};
    }

    LineLayout lineOf(const StyledBlock& block, std::size_t line, int wrap) const {
      return text_tiles::lineLayout(context.get(), block.lines[line], faceOf(block, line, wrap), theme, codeFamily);
    }

    LineLayout lineOf(const Piece& piece) const { return lineOf(blockOf(piece), piece.line, piece.wrap); }

    // Logical size of a line in device pixels.
    static std::pair<int, int> extents(const LineLayout& line) {
      const auto logical = text_tiles::logicalBox(line.layout.get());
      return {logical.width, logical.height};
    }

    // Adds a piece whose text sits at its top left.
    Piece& addPiece(std::uint32_t block, std::uint32_t line, int x, int y, int width, int height) {
      return pieces.emplace_back(
          Piece{.block = block, .line = line, .x = x, .y = y, .width = width, .height = height, .textX = x, .textY = y}
      );
    }

    void layoutDocument();
    void measureMarkers();
    void placeCode(std::uint32_t index, const std::vector<std::pair<int, int>>& sizes, std::size_t& next, int& y);
    void placeProse(std::uint32_t index, int& y);
    void placeTable(std::uint32_t index, int& y);
    void placeRule(const StyledBlock& block, int& y);
    void drawPiece(cairo_t* cr, const Piece& piece, TextTile& tile) const;
    void drawMarker(cairo_t* cr, const Piece& piece) const;
  };
} // namespace luciole
