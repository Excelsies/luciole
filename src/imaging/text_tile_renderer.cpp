#include "imaging/text_tile_renderer.h"

#include "imaging/text_tile_lines.h"
#include "imaging/text_tile_placement.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <format>
#include <utility>

namespace luciole {
  namespace {
    using text_tiles::clampOr;
    using text_tiles::kHeadingScale;
    using text_tiles::kMark;
    using text_tiles::Paint;
    using text_tiles::Piece;
    using text_tiles::pixels;

    constexpr double kMinWidth = 32.0;
  } // namespace

  void TextTileLayout::State::drawMarker(cairo_t* cr, const Piece& piece) const {
    const auto layout = text_tiles::plainLayout(context.get(), proseFont.get(), blockOf(piece).marker);
    const auto& color = theme.roles[static_cast<std::size_t>(formats::StyleRole::Punctuation)];
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, piece.textX - em(0.4) - text_tiles::logicalBox(layout.get()).width, piece.textY);
    pango_cairo_show_layout(cr, layout.get());
  }

  void TextTileLayout::State::drawPiece(cairo_t* cr, const Piece& piece, TextTile& tile) const {
    const auto& block = blockOf(piece);
    const auto line = lineOf(piece);
    const bool code = block.kind == BlockKind::Code;
    const int visible = piece.clipped ? piece.width - markWidth : piece.width;
    cairo_save(cr);
    if (code || block.kind == BlockKind::Table) {
      cairo_rectangle(cr, code ? piece.textX : piece.x, piece.y, visible, piece.height);
      cairo_clip(cr);
    }
    cairo_move_to(cr, piece.textX, piece.textY);
    pango_cairo_show_layout(cr, line.layout.get());
    cairo_restore(cr);
    if (piece.clipped) {
      const auto& color = theme.roles[static_cast<std::size_t>(formats::StyleRole::Comment)];
      cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
      cairo_move_to(cr, piece.textX + visible, piece.textY);
      pango_cairo_show_layout(cr, text_tiles::plainLayout(context.get(), codeFont.get(), kMark).get());
    }
    const auto& swatches = block.lines[piece.line].swatches;
    for (std::size_t k = 0; k < line.swatchBytes.size() && k < swatches.size(); ++k) {
      const auto square = text_tiles::swatchSquare(line, k, piece.textX, piece.textY);
      if (code && square.x + square.size > piece.textX + visible) {
        break;
      }
      text_tiles::drawSwatch(cr, square, swatches[k].color, theme, std::min(rule(), square.size / 2));
      tile.swatches.push_back({.x = square.x, .y = square.y, .size = square.size, .color = swatches[k].color});
    }
    if (piece.line == 0 && !block.marker.empty()) {
      drawMarker(cr, piece);
    }
  }

  TextTileLayout::TextTileLayout(
      std::shared_ptr<const formats::StyledDocument> document, const TextTileOptions& options
  )
      : m_state(std::make_unique<State>()) {
    auto& state = *m_state;
    state.document = document ? std::move(document) : std::make_shared<const formats::StyledDocument>();
    state.theme = options.theme;
    state.codeFamily = options.fonts.code;
    state.scale = clampOr(options.scale, text_tiles::kMinScale, text_tiles::kMaxScale, 1.0);
    const double width = clampOr(options.width, kMinWidth, kMaxTextTileWidth, TextTileOptions{}.width);
    state.requested = std::clamp(pixels(width * state.scale), 1, kMaxTextTileWidth);
    state.codeEm = text_tiles::emOf(options.fonts.codeSize, TextTileFonts{}.codeSize, state.scale);
    state.proseEm = text_tiles::emOf(options.fonts.proseSize, TextTileFonts{}.proseSize, state.scale);
    state.codeFont = text_tiles::fontOf(options.fonts.code, state.codeEm, false);
    state.proseFont = text_tiles::fontOf(options.fonts.prose, state.proseEm, false);
    for (std::size_t level = 0; level < kHeadingScale.size(); ++level) {
      state.headingFonts[level] = text_tiles::fontOf(options.fonts.prose, state.proseEm * kHeadingScale[level], true);
    }
    state.fontMap.reset(pango_cairo_font_map_new());
    state.context = text_tiles::contextOf(state.fontMap.get());
    state.layoutDocument();
  }

  TextTileLayout::~TextTileLayout() = default;
  TextTileLayout::TextTileLayout(TextTileLayout&&) noexcept = default;
  TextTileLayout& TextTileLayout::operator=(TextTileLayout&&) noexcept = default;

  int TextTileLayout::width() const { return m_state->width; }

  int TextTileLayout::height() const { return m_state->height; }

  std::size_t TextTileLayout::tileCount() const {
    return static_cast<std::size_t>((m_state->height + kTextTileHeight - 1) / kTextTileHeight);
  }

  bool TextTileLayout::truncated() const { return m_state->truncated; }

  std::expected<TextTile, std::string> TextTileLayout::renderTile(std::size_t index) {
    const auto& state = *m_state;
    if (index >= tileCount()) {
      return std::unexpected(std::format("tile {} is past the layout's {} tiles", index, tileCount()));
    }
    TextTile tile;
    tile.top = static_cast<int>(index) * kTextTileHeight;
    tile.width = state.width;
    tile.height = std::min(kTextTileHeight, state.height - tile.top);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tile.width, tile.height);
    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
      const std::string reason = cairo_status_to_string(cairo_status(cr));
      cairo_destroy(cr);
      cairo_surface_destroy(surface);
      return std::unexpected("cannot draw a text tile: " + reason);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    const auto& background = state.theme.background;
    cairo_set_source_rgba(cr, background.r, background.g, background.b, background.a);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, 0, -tile.top);
    const int bottom = tile.top + tile.height;
    for (const auto paint : {Paint::CodeBackground, Paint::Outline}) {
      const auto& color = paint == Paint::CodeBackground ? state.theme.codeBackground : state.theme.outline;
      for (const auto& d : state.decorations) {
        if (d.paint == paint && d.y < bottom && d.y + d.height > tile.top) {
          text_tiles::fillRect(cr, color, d.x, d.y, d.width, d.height);
        }
      }
    }
    const auto first =
        std::ranges::partition_point(state.pieces, [&](const Piece& p) { return p.bottom() <= tile.top; });
    for (auto it = first; it != state.pieces.end() && it->y < bottom; ++it) {
      state.drawPiece(cr, *it, tile);
    }
    cairo_destroy(cr);
    text_tiles::copyPixels(surface, tile.width, tile.height, tile.rgba);
    cairo_surface_destroy(surface);
    return tile;
  }

  std::optional<TextBox> TextTileLayout::characterBox(const TextPosition& position) {
    const auto& state = *m_state;
    if (position.block >= state.blockPieces.size() || position.line >= state.blockPieces[position.block].count) {
      return std::nullopt;
    }
    const auto& piece = state.pieces[state.blockPieces[position.block].first + position.line];
    if (piece.y >= state.height) {
      return std::nullopt;
    }
    return text_tiles::characterBox(state.lineOf(piece), position.column, piece.textX, piece.textY);
  }

  std::optional<TextPosition> TextTileLayout::positionAt(double x, double y) {
    const auto& state = *m_state;
    if (!std::isfinite(x) || !std::isfinite(y) || y < 0.0 || y >= state.height) {
      return std::nullopt;
    }
    const auto found = std::ranges::partition_point(state.pieces, [&](const Piece& p) { return p.bottom() <= y; });
    if (found == state.pieces.end() || found->y > y) {
      return std::nullopt;
    }
    // Cells of a table row share a top: take the cell under x, or the nearest one left of it.
    auto hit = found;
    for (auto it = found; it != state.pieces.end() && it->y == found->y; ++it) {
      if (x >= it->x) {
        hit = it;
      }
    }
    const auto line = state.lineOf(*hit);
    int index = 0;
    int trailing = 0;
    pango_layout_xy_to_index(
        line.layout.get(), pixels((x - hit->textX) * PANGO_SCALE), pixels((y - hit->textY) * PANGO_SCALE), &index,
        &trailing
    );
    return TextPosition{.block = hit->block, .line = hit->line, .column = text_tiles::columnAt(line, index, trailing)};
  }

  std::vector<int> TextTileLayout::lineTops() const {
    const auto& state = *m_state;
    std::vector<int> tops;
    tops.reserve(state.pieces.size());
    for (const auto& piece : state.pieces) { // in document order, by top
      if (piece.y >= state.height) {
        break;
      }
      tops.push_back(piece.y);
    }
    return tops;
  }
} // namespace luciole
