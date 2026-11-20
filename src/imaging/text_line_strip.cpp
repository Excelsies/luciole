#include "imaging/text_line_strip.h"

#include "fs/gio_ptr.h"
#include "imaging/text_line_glyphs.h"
#include "imaging/text_tile_lines.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <pango/pangocairo.h>
#include <string>
#include <utility>
#include <vector>

namespace luciole {
  namespace {
    using text_tiles::pixels;

    struct SurfaceFree {
      void operator()(cairo_surface_t* surface) const { cairo_surface_destroy(surface); }
    };

    using SurfacePtr = std::unique_ptr<cairo_surface_t, SurfaceFree>;

    // Pango places glyphs at 32-bit positions in 1/PANGO_SCALE pixels; half of that range is kept as margin.
    constexpr double kShaperPixels = static_cast<double>(std::numeric_limits<int>::max() / PANGO_SCALE) / 2.0;
    // Generous advances in em, for a first cut that keeps the text the shaper sees near kShaperPixels.
    constexpr double kTabEm = 8.0;
    constexpr double kGlyphEm = 2.0;
    constexpr double kGapEm = 2.0;
    // How far beside a window, in em, the scratch surface holds the ink of the glyphs drawn in it.
    constexpr double kInkMarginEm = 8.0;

    bool sameLine(const formats::StyledLine& a, const formats::StyledLine& b) {
      return a.continues == b.continues && a.swatches == b.swatches && a.runs == b.runs;
    }

    std::size_t sequenceEnd(const std::string& text, std::size_t i) {
      return std::min(text.size(), i + static_cast<std::size_t>(g_utf8_skip[static_cast<guchar>(text[i])]));
    }

    // The line's columns before `column`, with their swatches; the line goes on no further.
    formats::StyledLine cutAt(const formats::StyledLine& line, std::size_t column) {
      formats::StyledLine cut;
      std::size_t at = 0;
      // Bounded by the line's runs.
      for (const auto& run : line.runs) {
        if (at >= column) {
          break;
        }
        std::size_t i = 0;
        // Every pass moves i forward by one sequence.
        while (i < run.text.size() && at < column) {
          i = sequenceEnd(run.text, i);
          ++at;
        }
        cut.runs.push_back(run);
        cut.runs.back().text.resize(i);
      }
      std::ranges::copy_if(line.swatches, std::back_inserter(cut.swatches), [&](const formats::ColorSwatch& s) {
        return s.column < column;
      });
      return cut;
    }

    // The first column past kShaperPixels at the generous advances; none when all of the line stays inside.
    std::optional<std::size_t> estimatedReach(const formats::StyledLine& line, double em) {
      const double budget = kShaperPixels / em;
      double used = 0.0;
      std::size_t column = 0;
      std::size_t swatch = 0;
      // Bounded by the line's runs.
      for (const auto& run : line.runs) {
        // Every pass moves i forward by one sequence.
        for (std::size_t i = 0; i < run.text.size(); i = sequenceEnd(run.text, i)) {
          // Bounded by the line's swatches.
          while (swatch < line.swatches.size() && line.swatches[swatch].column <= column) {
            used += kGapEm;
            ++swatch;
          }
          used += run.text[i] == '\t' ? kTabEm : kGlyphEm;
          if (used > budget) {
            return column;
          }
          ++column;
        }
      }
      return std::nullopt;
    }

    // The first column with a glyph whose right edge passes kShaperPixels, from the shaped advances summed in 64 bits;
    // none when the whole layout stays inside.
    std::optional<std::size_t> shapedReach(const text_tiles::LineLayout& line) {
      const PangoLayoutLine* shaped = pango_layout_get_line_readonly(line.layout.get(), 0);
      if (shaped == nullptr) {
        return std::nullopt;
      }
      const auto limit = static_cast<std::int64_t>(kShaperPixels * PANGO_SCALE);
      std::int64_t x = 0;
      int first = std::numeric_limits<int>::max();
      // One pass per run of the line, in visual order.
      for (const GSList* item = shaped->runs; item != nullptr; item = item->next) {
        const auto* run = static_cast<const PangoLayoutRun*>(item->data);
        // One pass per glyph of the run.
        for (int i = 0; i < run->glyphs->num_glyphs; ++i) {
          x += run->glyphs->glyphs[i].geometry.width;
          if (x > limit) {
            first = std::min(first, run->item->offset + run->glyphs->log_clusters[i]);
          }
        }
      }
      if (first == std::numeric_limits<int>::max()) {
        return std::nullopt;
      }
      return text_tiles::columnAt(line, first, 0);
    }
  } // namespace

  struct TextLineStrip::State {
    struct Shaped {
      formats::StyledLine line; // as asked, before any cut
      text_tiles::LineLayout layout;
      std::vector<text_tiles::SwatchSquare> squares; // in line coordinates, one per swatch the layout holds
      int top = 0; // device pixels from the strip's top to the layout's, keeping the shared baseline
      int width = 0;
    };

    TextTileTheme theme;
    std::string codeFamily;
    double scale = 1.0;
    double em = 0.0;
    GioPtr<PangoFontMap> fontMap;
    GioPtr<PangoContext> context;
    text_tiles::FontPtr font;
    int height = 0;
    int baseline = 0; // Pango units from the top
    int underlineTop = 0;
    int underlineHeight = 1;
    std::optional<Shaped> last;

    const Shaped& shaped(const formats::StyledLine& line);
    double caretX(const Shaped& shaped, std::size_t column) const;
    // Draws in line coordinates: the line's start at x 0 and the strip's top at y 0.
    void draw(cairo_t* cr, const Shaped& shaped, ColumnRange composing) const;
    // Draws the window [x0, x0 + width) of the line at the target's top left.
    std::expected<void, std::string>
    paint(cairo_surface_t* target, const Shaped& shaped, int x0, int width, ColumnRange composing) const;
  };

  const TextLineStrip::State::Shaped& TextLineStrip::State::shaped(const formats::StyledLine& line) {
    if (last && sameLine(last->line, line)) {
      return *last;
    }
    last.reset();
    Shaped out;
    out.line = line;
    const text_tiles::LineFace face{.font = font.get(), .em = em, .wrapWidth = 0, .align = PANGO_ALIGN_LEFT};
    const auto estimated = estimatedReach(line, em);
    out.layout =
        text_tiles::lineLayout(context.get(), estimated ? cutAt(line, *estimated) : line, face, theme, codeFamily);
    // The glyphs before the first one past the limit took separate places inside it, so shaped alone they fit too.
    if (const auto reach = shapedReach(out.layout)) {
      out.layout = text_tiles::lineLayout(context.get(), cutAt(line, *reach), face, theme, codeFamily);
    }
    auto* layout = out.layout.layout.get();
    out.top = pixels(static_cast<double>(baseline - pango_layout_get_baseline(layout)) / PANGO_SCALE);
    PangoRectangle ink{};
    PangoRectangle logical{};
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    out.width = std::max({0, logical.x + logical.width, ink.width > 0 ? ink.x + ink.width : 0});
    const auto& swatches = out.layout.swatchBytes;
    // Bounded by the line's swatches.
    for (std::size_t k = 0; k < swatches.size() && k < line.swatches.size(); ++k) {
      out.squares.push_back(text_tiles::swatchSquare(out.layout, k, 0.0, out.top));
    }
    last = std::move(out);
    return *last;
  }

  double TextLineStrip::State::caretX(const Shaped& shaped, std::size_t column) const {
    // Pango places an index inside a grapheme cluster at the cluster's start, and at the character's leading edge: its
    // right edge in right-to-left text.
    const auto c = std::min(column, shaped.layout.columnBytes.size() - 1);
    PangoRectangle at{};
    pango_layout_index_to_pos(shaped.layout.layout.get(), shaped.layout.columnBytes[c], &at);
    return static_cast<double>(at.x) / PANGO_SCALE;
  }

  void TextLineStrip::State::draw(cairo_t* cr, const Shaped& shaped, ColumnRange composing) const {
    cairo_move_to(cr, 0.0, shaped.top);
    pango_cairo_show_layout(cr, shaped.layout.layout.get());
    for (std::size_t k = 0; k < shaped.squares.size(); ++k) {
      const auto& square = shaped.squares[k];
      text_tiles::drawSwatch(
          cr, square, shaped.line.swatches[k].color, theme, std::min(text_tiles::ruleWidth(scale), square.size / 2)
      );
    }
    if (composing.end > composing.begin) {
      const int begin = pixels(caretX(shaped, composing.begin));
      const int end = pixels(caretX(shaped, composing.end));
      const int left = std::min(begin, end);
      const int right = std::max(begin, end);
      const auto& color = theme.roles[static_cast<std::size_t>(formats::StyleRole::Plain)];
      text_tiles::fillRect(cr, color, left, underlineTop, right - left, underlineHeight);
    }
  }

  std::expected<void, std::string> TextLineStrip::State::paint(
      cairo_surface_t* target, const Shaped& shaped, int x0, int width, ColumnRange composing
  ) const {
    // Cairo trims a glyph run that does not fit its surface to the glyphs' metric boxes, which drops the top row of
    // hinted ascenders. Only the glyphs inked in the window are drawn, on a scratch surface that holds all their ink.
    const text_tiles::GlyphWindow shown(shaped.layout.layout.get(), x0, static_cast<double>(x0) + width);
    auto ink = shown.ink();
    ink.top += shaped.top;
    ink.bottom += shaped.top;
    if (ink.empty()) {
      ink = {.left = static_cast<double>(x0), .top = 0.0, .right = static_cast<double>(x0) + width, .bottom = 0.0};
    }
    // Line coordinates of the scratch surface's edges, at most the ink margin beside the window and a line height
    // above and below it.
    const auto edge = [](double value, std::int64_t low, std::int64_t high) {
      return static_cast<int>(std::clamp(static_cast<std::int64_t>(value), low, high));
    };
    const auto margin = static_cast<std::int64_t>(std::ceil(kInkMarginEm * em));
    const std::int64_t windowRight = static_cast<std::int64_t>(x0) + width;
    const int left = edge(std::floor(std::min<double>(x0, ink.left)) - 1.0, x0 - margin, x0);
    const int right =
        edge(std::ceil(std::max<double>(windowRight, ink.right)) + 1.0, windowRight, windowRight + margin);
    const int top = edge(std::floor(std::min(0.0, ink.top)) - 1.0, -height, 0);
    const int bottom = edge(std::ceil(std::max<double>(height, ink.bottom)) + 1.0, height, 2 * height);
    const SurfacePtr scratch(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, right - left, bottom - top));
    cairo_t* cr = cairo_create(scratch.get());
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
      std::string reason = cairo_status_to_string(cairo_status(cr));
      cairo_destroy(cr);
      return std::unexpected("cannot draw a text line: " + reason);
    }
    cairo_translate(cr, -static_cast<double>(left), -static_cast<double>(top));
    draw(cr, shaped, composing);
    cairo_destroy(cr);
    // Nothing is copied left of the line's start, as a tile clips its code lines.
    cairo_t* copy = cairo_create(target);
    cairo_set_operator(copy, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(copy, scratch.get(), static_cast<double>(left) - x0, static_cast<double>(top));
    cairo_rectangle(copy, std::max(0.0, -static_cast<double>(x0)), 0.0, width, height);
    cairo_fill(copy);
    cairo_destroy(copy);
    return {};
  }

  TextLineStrip::TextLineStrip(const TextLineStripOptions& options) : m_state(std::make_unique<State>()) {
    auto& state = *m_state;
    state.theme = options.theme;
    state.codeFamily = options.fonts.code;
    state.scale = text_tiles::clampOr(options.scale, text_tiles::kMinScale, text_tiles::kMaxScale, 1.0);
    state.em = text_tiles::emOf(options.fonts.codeSize, TextTileFonts{}.codeSize, state.scale);
    state.font = text_tiles::fontOf(options.fonts.code, state.em, false);
    state.fontMap.reset(pango_cairo_font_map_new());
    state.context = text_tiles::contextOf(state.fontMap.get());
    const auto probe = text_tiles::plainLayout(state.context.get(), state.font.get(), "");
    state.height = std::max(1, text_tiles::logicalBox(probe.get()).height);
    state.baseline = pango_layout_get_baseline(probe.get());
    PangoFontMetrics* metrics = pango_context_get_metrics(state.context.get(), state.font.get(), nullptr);
    state.underlineHeight =
        std::clamp(PANGO_PIXELS(pango_font_metrics_get_underline_thickness(metrics)), 1, state.height);
    const int below = state.baseline - pango_font_metrics_get_underline_position(metrics);
    state.underlineTop = std::clamp(PANGO_PIXELS(below), 0, state.height - state.underlineHeight);
    pango_font_metrics_unref(metrics);
  }

  TextLineStrip::~TextLineStrip() = default;
  TextLineStrip::TextLineStrip(TextLineStrip&&) noexcept = default;
  TextLineStrip& TextLineStrip::operator=(TextLineStrip&&) noexcept = default;

  int TextLineStrip::lineHeight() const { return m_state->height; }

  int TextLineStrip::lineWidth(const formats::StyledLine& line) { return m_state->shaped(line).width; }

  std::expected<TextStrip, std::string>
  TextLineStrip::render(const formats::StyledLine& line, int x0, int width, ColumnRange composing) {
    auto& state = *m_state;
    TextStrip strip;
    strip.x0 = x0;
    strip.width = std::clamp(width, 0, kMaxTextTileWidth);
    strip.height = state.height;
    if (strip.width == 0) {
      return strip;
    }
    const auto& shaped = state.shaped(line);
    const SurfacePtr surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, strip.width, strip.height));
    const auto right = static_cast<std::int64_t>(x0) + strip.width;
    if (x0 < shaped.width && right > 0) {
      auto painted = state.paint(surface.get(), shaped, x0, strip.width, composing);
      if (!painted) {
        return std::unexpected(std::move(painted.error()));
      }
    }
    text_tiles::copyPixels(surface.get(), strip.width, strip.height, strip.rgba);
    return strip;
  }

  double TextLineStrip::caretX(const formats::StyledLine& line, std::size_t column) {
    return m_state->caretX(m_state->shaped(line), column);
  }

  std::size_t TextLineStrip::columnAt(const formats::StyledLine& line, double x) {
    const auto& shaped = m_state->shaped(line);
    if (std::isnan(x) || x <= 0.0) {
      return 0;
    }
    const int position = pixels(std::min(x, kShaperPixels) * PANGO_SCALE);
    int index = 0;
    int trailing = 0;
    pango_layout_line_x_to_index(
        pango_layout_get_line_readonly(shaped.layout.layout.get(), 0), position, &index, &trailing
    );
    // Pango answers with a grapheme cluster's start and, past its middle, its length in characters.
    return text_tiles::columnAt(shaped.layout, index, trailing);
  }
} // namespace luciole
