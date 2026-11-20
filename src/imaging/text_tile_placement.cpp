#include "imaging/text_tile_placement.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace luciole {
  namespace {
    using text_tiles::kMark;
    using text_tiles::Paint;
    using text_tiles::pixels;

    std::int64_t sum(const std::vector<int>& widths) {
      return std::ranges::fold_left(widths, std::int64_t{0}, std::plus<>{});
    }

    // Each column at the same cap, clamped to its low and high, for the largest cap that fits the room; every column
    // at its low when even those do not fit. Each low is at most its high.
    std::vector<int> capColumns(const std::vector<int>& lows, const std::vector<int>& highs, int room) {
      const auto total = [&](int cap) {
        std::int64_t width = 0;
        for (std::size_t c = 0; c < lows.size(); ++c) {
          width += std::clamp(cap, lows[c], highs[c]);
        }
        return width;
      };
      // Halves the range each pass, so at most 32 passes.
      int fits = 0;
      int over = std::ranges::max(highs) + 1;
      while (over - fits > 1) {
        const int cap = fits + ((over - fits) / 2);
        (total(cap) <= room ? fits : over) = cap;
      }
      std::vector<int> widths(lows.size());
      for (std::size_t c = 0; c < lows.size(); ++c) {
        widths[c] = std::clamp(fits, lows[c], highs[c]);
      }
      return widths;
    }

    // Natural widths when they fit the room. Otherwise the widest columns give way first: down to their longest
    // words while the words fit, then below those, never under `narrowest`.
    std::vector<int>
    fitColumns(const std::vector<int>& natural, const std::vector<int>& words, int narrowest, int room) {
      if (sum(words) <= room) {
        return capColumns(words, natural, room);
      }
      return capColumns(std::vector<int>(words.size(), narrowest), words, room);
    }
  } // namespace

  void TextTileLayout::State::measureMarkers() {
    decltype(listLefts) widest{};
    for (const auto& block : document->blocks) {
      if (!block.marker.empty()) {
        widest[block.indent] = std::max(widest[block.indent], markerWidth(block));
      }
    }
    for (std::size_t level = 1; level < listLefts.size(); ++level) {
      listLefts[level] = listLefts[level - 1] + std::max(em(1.6), widest[level] + em(0.4));
    }
  }

  void TextTileLayout::State::layoutDocument() {
    const auto& blocks = document->blocks;
    measureMarkers();
    // Code lines and tables at their narrowest are measured first: the widest one widens the layout.
    std::vector<std::pair<int, int>> codeSizes;
    int widest = requested;
    for (const auto& block : blocks) {
      if (block.kind == BlockKind::Table) {
        widest = std::max(widest, leftOf(block) + tableMinimum(block) + pad());
      }
      if (block.kind != BlockKind::Code) {
        continue;
      }
      const int inset = block.framed ? pixels(codeEm * 0.5) : 0;
      for (std::size_t k = 0; k < block.lines.size(); ++k) {
        const auto size = extents(lineOf(block, k, 0));
        codeSizes.push_back(size);
        widest = std::max(widest, leftOf(block) + (2 * inset) + size.first + pad());
      }
    }
    width = std::clamp(widest, 1, kMaxTextTileWidth);
    const auto maxHeight = static_cast<int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(kMaxTextTiles) * kTextTileHeight, kMaxTextPixels / static_cast<std::uint64_t>(width)
    ));
    markWidth = text_tiles::logicalBox(text_tiles::plainLayout(context.get(), codeFont.get(), kMark).get()).width;

    std::size_t nextCode = 0;
    int y = pad();
    int previousDepth = 0;
    blockPieces.assign(blocks.size(), {});
    for (std::uint32_t index = 0; index < blocks.size() && y < maxHeight; ++index) {
      const auto& block = blocks[index];
      const int gapTop = y;
      if (index > 0) {
        const bool listed = block.indent > 0 && blocks[index - 1].indent > 0;
        y += block.heading > 0 ? em(0.9) : listed ? em(0.25) : em(0.5);
      }
      const int top = y;
      blockPieces[index].first = pieces.size();
      switch (block.kind) {
      case BlockKind::Code:
        placeCode(index, codeSizes, nextCode, y);
        break;
      case BlockKind::Prose:
        placeProse(index, y);
        break;
      case BlockKind::Table:
        placeTable(index, y);
        break;
      case BlockKind::Rule:
        placeRule(block, y);
        break;
      }
      blockPieces[index].count = pieces.size() - blockPieces[index].first;
      for (int depth = 0; depth < block.quoteDepth; ++depth) {
        const int barTop = previousDepth > depth ? gapTop : top;
        decorations.push_back(
            {.x = pad() + (depth * em(1.0)), .y = barTop, .width = std::max(2, em(0.2)), .height = y - barTop}
        );
      }
      previousDepth = block.quoteDepth;
      truncated = truncated || (y >= maxHeight && index + 1 < blocks.size());
    }
    height = blocks.empty() ? 0 : y + pad();
    if (height > maxHeight) {
      height = maxHeight;
      truncated = true;
    }
  }

  void TextTileLayout::State::placeCode(
      std::uint32_t index, const std::vector<std::pair<int, int>>& sizes, std::size_t& next, int& y
  ) {
    const auto& block = document->blocks[index];
    const int left = leftOf(block);
    const int inset = block.framed ? pixels(codeEm * 0.5) : 0;
    const int top = y;
    y += inset;
    for (std::uint32_t k = 0; k < block.lines.size(); ++k) {
      const auto [lineWidth, lineHeight] = sizes[next++];
      const int x = left + inset;
      const int room = std::max(1, width - pad() - inset - x);
      addPiece(index, k, x, y, room, lineHeight).clipped = lineWidth > room;
      y += lineHeight;
    }
    y += inset;
    if (block.framed) {
      decorations.push_back(
          {.x = left, .y = top, .width = width - pad() - left, .height = y - top, .paint = Paint::CodeBackground}
      );
    }
  }

  void TextTileLayout::State::placeProse(std::uint32_t index, int& y) {
    const auto& block = document->blocks[index];
    const int left = leftOf(block);
    const int wrap = std::max(em(1.0), requested - pad() - left);
    for (std::uint32_t k = 0; k < block.lines.size(); ++k) {
      const int lineHeight = extents(lineOf(block, k, wrap)).second;
      addPiece(index, k, left, y, wrap, lineHeight).wrap = wrap;
      y += lineHeight;
    }
  }

  void TextTileLayout::State::placeTable(std::uint32_t index, int& y) {
    const auto& block = document->blocks[index];
    const auto columns = block.columns;
    if (columns == 0) {
      return;
    }
    const int left = leftOf(block);
    const int line = rule();
    const int padX = em(0.5);
    const int padY = em(0.25);
    const int narrowest = narrowestCell();
    std::vector<int> natural(columns, narrowest);
    std::vector<int> words(columns, narrowest);
    for (std::size_t k = 0; k < block.lines.size(); ++k) {
      const auto cell = lineOf(block, k, 0);
      natural[k % columns] = std::max(natural[k % columns], extents(cell).first + (2 * padX));
      words[k % columns] = std::max(words[k % columns], text_tiles::longestWord(cell) + (2 * padX));
    }
    for (std::size_t c = 0; c < columns; ++c) {
      words[c] = std::min(words[c], natural[c]);
    }
    const int grid = line * static_cast<int>(columns + 1);
    // The requested width, or as much more of the layout as whole words need, and at least what the narrowest
    // columns need.
    const auto needed = std::max<std::int64_t>(left + sum(words) + grid + pad(), left + tableMinimum(block) + pad());
    const int outer = std::max(requested, static_cast<int>(std::min<std::int64_t>(width, needed)));
    const auto widths = fitColumns(natural, words, narrowest, outer - pad() - left - grid);
    std::vector<int> xs(columns + 1, left);
    for (std::size_t c = 0; c < columns; ++c) {
      xs[c + 1] = xs[c] + line + widths[c];
    }
    const int tableWidth = xs[columns] + line - left;
    truncated = truncated || left + tableWidth > width;
    const int top = y;
    decorations.push_back({.x = left, .y = y, .width = tableWidth, .height = line});
    y += line;
    // Each pass places one row of cells.
    for (std::size_t first = 0; first + columns <= block.lines.size(); first += columns) {
      int rowHeight = 0;
      for (std::size_t c = 0; c < columns; ++c) {
        rowHeight = std::max(rowHeight, extents(lineOf(block, first + c, widths[c] - (2 * padX))).second);
      }
      rowHeight += 2 * padY;
      for (std::size_t c = 0; c < columns; ++c) {
        auto& cell = addPiece(index, static_cast<std::uint32_t>(first + c), xs[c] + line, y, widths[c], rowHeight);
        cell.textX += padX;
        cell.textY += padY;
        cell.wrap = widths[c] - (2 * padX);
      }
      y += rowHeight;
      decorations.push_back({.x = left, .y = y, .width = tableWidth, .height = line});
      y += line;
    }
    for (std::size_t c = 0; c <= columns; ++c) {
      decorations.push_back({.x = xs[c], .y = top, .width = line, .height = y - top});
    }
  }

  void TextTileLayout::State::placeRule(const StyledBlock& block, int& y) {
    const int left = leftOf(block);
    y += em(0.5);
    decorations.push_back({.x = left, .y = y, .width = std::max(1, requested - pad() - left), .height = rule()});
    y += rule() + em(0.5);
  }
} // namespace luciole
