#include "imaging/text_tile_lines.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::text_tiles {
  namespace {
    constexpr std::string_view kGap = "\xEF\xBF\xBC"; // U+FFFC, shaped as the swatch gap
    constexpr std::string_view kEllipsis = "…";
    constexpr double kSwatchEm = 0.8;
    constexpr double kSwatchMarginEm = 0.15;

    struct AttrListUnref {
      void operator()(PangoAttrList* list) const { pango_attr_list_unref(list); }
    };

    guint16 channel(float value) { return static_cast<guint16>(std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F)); }

    void insert(PangoAttrList* list, PangoAttribute* attribute, std::size_t begin, std::size_t end) {
      attribute->start_index = static_cast<guint>(begin);
      attribute->end_index = static_cast<guint>(end);
      pango_attr_list_insert(list, attribute);
    }

    void insertColor(PangoAttrList* list, const Color& color, std::size_t begin, std::size_t end) {
      insert(list, pango_attr_foreground_new(channel(color.r), channel(color.g), channel(color.b)), begin, end);
      if (color.a < 1.0F) {
        insert(list, pango_attr_foreground_alpha_new(channel(color.a)), begin, end);
      }
    }

    void insertStyle(
        PangoAttrList* list, const formats::StyledRun& run, std::size_t begin, std::size_t end,
        const TextTileTheme& theme, const std::string& codeFamily
    ) {
      insertColor(list, theme.roles[static_cast<std::size_t>(run.role)], begin, end);
      if (run.bold) {
        insert(list, pango_attr_weight_new(PANGO_WEIGHT_BOLD), begin, end);
      }
      if (run.italic) {
        insert(list, pango_attr_style_new(PANGO_STYLE_ITALIC), begin, end);
      }
      if (run.role == formats::StyleRole::Link) {
        insert(list, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), begin, end);
      }
      if (run.role == formats::StyleRole::InlineCode) {
        const auto& back = theme.codeBackground;
        insert(list, pango_attr_family_new(codeFamily.c_str()), begin, end);
        insert(list, pango_attr_background_new(channel(back.r), channel(back.g), channel(back.b)), begin, end);
        if (back.a < 1.0F) {
          insert(list, pango_attr_background_alpha_new(channel(back.a)), begin, end);
        }
      }
    }

    // Bytes of the UTF-8 sequence a lead byte starts.
    std::size_t sequenceLength(char lead) {
      const auto byte = static_cast<unsigned char>(lead);
      return byte >= 0xF0U ? 4 : byte >= 0xE0U ? 3 : byte >= 0xC0U ? 2 : 1;
    }
  } // namespace

  double clampOr(double value, double low, double high, double fallback) {
    return std::isnan(value) ? fallback : std::clamp(value, low, high);
  }

  double emOf(double size, double fallback, double scale) {
    return clampOr(size, kMinFont, kMaxFont, fallback) * scale;
  }

  int ruleWidth(double scale) { return std::max(1, pixels(scale)); }

  GioPtr<PangoContext> contextOf(PangoFontMap* fontMap) {
    GioPtr<PangoContext> context(pango_font_map_create_context(fontMap));
    cairo_font_options_t* fontOptions = cairo_font_options_create();
    cairo_font_options_set_antialias(fontOptions, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(fontOptions, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(fontOptions, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(context.get(), fontOptions);
    cairo_font_options_destroy(fontOptions);
    pango_context_set_round_glyph_positions(context.get(), FALSE);
    return context;
  }

  void copyPixels(cairo_surface_t* surface, int width, int height, std::vector<std::uint8_t>& rgba) {
    cairo_surface_flush(surface);
    const auto* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        std::uint32_t pixel = 0;
        std::memcpy(&pixel, data + (static_cast<std::size_t>(y) * stride) + (static_cast<std::size_t>(x) * 4), 4);
        const auto index = ((static_cast<std::size_t>(y) * width) + x) * 4;
        rgba[index] = static_cast<std::uint8_t>(pixel >> 16U);
        rgba[index + 1] = static_cast<std::uint8_t>(pixel >> 8U);
        rgba[index + 2] = static_cast<std::uint8_t>(pixel);
        rgba[index + 3] = static_cast<std::uint8_t>(pixel >> 24U);
      }
    }
  }

  FontPtr fontOf(const std::string& family, double pixels, bool bold) {
    FontPtr font(pango_font_description_new());
    pango_font_description_set_family(font.get(), family.c_str());
    pango_font_description_set_absolute_size(font.get(), pixels * PANGO_SCALE);
    if (bold) {
      pango_font_description_set_weight(font.get(), PANGO_WEIGHT_BOLD);
    }
    return font;
  }

  LineLayout lineLayout(
      PangoContext* context, const formats::StyledLine& line, const LineFace& face, const TextTileTheme& theme,
      const std::string& codeFamily
  ) {
    LineLayout out;
    out.swatchSize = std::max(2, static_cast<int>(std::lround(face.em * kSwatchEm)));
    out.swatchGap = out.swatchSize + (2 * std::max(1, static_cast<int>(std::lround(face.em * kSwatchMarginEm))));
    const std::unique_ptr<PangoAttrList, AttrListUnref> attributes(pango_attr_list_new());
    std::string text;
    std::size_t column = 0;
    std::size_t swatch = 0;
    const auto gapsBefore = [&](std::size_t at) {
      // Bounded by the line's swatches.
      while (swatch < line.swatches.size() && line.swatches[swatch].column <= at) {
        PangoRectangle box{
            .x = 0,
            .y = -out.swatchSize * PANGO_SCALE,
            .width = out.swatchGap * PANGO_SCALE,
            .height = out.swatchSize * PANGO_SCALE
        };
        out.swatchBytes.push_back(static_cast<int>(text.size()));
        insert(attributes.get(), pango_attr_shape_new(&box, &box), text.size(), text.size() + kGap.size());
        text += kGap;
        ++swatch;
      }
    };
    for (const auto& run : line.runs) {
      const auto begin = text.size();
      std::size_t i = 0;
      // Every pass moves i forward by one sequence.
      while (i < run.text.size()) {
        gapsBefore(column);
        out.columnBytes.push_back(static_cast<int>(text.size()));
        const auto length = std::min(sequenceLength(run.text[i]), run.text.size() - i);
        text.append(run.text, i, length);
        i += length;
        ++column;
      }
      insertStyle(attributes.get(), run, begin, text.size(), theme, codeFamily);
    }
    gapsBefore(column);
    out.columnBytes.push_back(static_cast<int>(text.size()));
    if (line.continues) {
      const auto begin = text.size();
      text += kEllipsis;
      insertColor(
          attributes.get(), theme.roles[static_cast<std::size_t>(formats::StyleRole::Comment)], begin, text.size()
      );
    }
    out.layout.reset(pango_layout_new(context));
    auto* layout = out.layout.get();
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_font_description(layout, face.font);
    pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
    pango_layout_set_attributes(layout, attributes.get());
    pango_layout_set_alignment(layout, face.align);
    if (face.wrapWidth > 0) {
      pango_layout_set_width(layout, face.wrapWidth * PANGO_SCALE);
      pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    }
    return out;
  }

  std::size_t columnAt(const LineLayout& line, int byte, int trailing) {
    const auto& starts = line.columnBytes;
    const auto end = starts.size() - 1;
    const auto found = std::ranges::lower_bound(starts, byte);
    auto column = static_cast<std::size_t>(found - starts.begin());
    if (found != starts.end() && *found == byte && trailing > 0) {
      column += static_cast<std::size_t>(trailing);
    }
    return std::min(column, end);
  }

  SwatchSquare swatchSquare(const LineLayout& line, std::size_t swatch, double originX, double originY) {
    PangoRectangle at{};
    pango_layout_index_to_pos(line.layout.get(), line.swatchBytes[swatch], &at);
    const double left = originX + (static_cast<double>(at.x) / PANGO_SCALE);
    const double top = originY + (static_cast<double>(at.y) / PANGO_SCALE);
    const double height = static_cast<double>(at.height) / PANGO_SCALE;
    return {
        .x = static_cast<int>(std::lround(left + ((line.swatchGap - line.swatchSize) / 2.0))),
        .y = static_cast<int>(std::lround(top + ((height - line.swatchSize) / 2.0))),
        .size = line.swatchSize,
    };
  }

  PangoAlignment alignmentOf(formats::CellAlign align) {
    switch (align) {
    case formats::CellAlign::Center:
      return PANGO_ALIGN_CENTER;
    case formats::CellAlign::End:
      return PANGO_ALIGN_RIGHT;
    case formats::CellAlign::Start:
      return PANGO_ALIGN_LEFT;
    }
    return PANGO_ALIGN_LEFT;
  }

  GioPtr<PangoLayout> plainLayout(PangoContext* context, const PangoFontDescription* font, std::string_view text) {
    GioPtr<PangoLayout> layout(pango_layout_new(context));
    pango_layout_set_single_paragraph_mode(layout.get(), TRUE);
    pango_layout_set_font_description(layout.get(), font);
    pango_layout_set_text(layout.get(), text.data(), static_cast<int>(text.size()));
    return layout;
  }

  PangoRectangle logicalBox(PangoLayout* layout) {
    PangoRectangle logical{};
    pango_layout_get_pixel_extents(layout, nullptr, &logical);
    return logical;
  }

  int longestWord(const LineLayout& line) {
    auto* layout = line.layout.get();
    const std::string_view text = pango_layout_get_text(layout);
    int attributeCount = 0;
    const auto* attributes = pango_layout_get_log_attrs_readonly(layout, &attributeCount);
    // Byte offset and word of each character, one character per pass; a word starts where a line may break.
    std::vector<std::size_t> starts;
    std::vector<std::size_t> words;
    std::size_t word = 0;
    for (std::size_t i = 0; i < text.size() && std::cmp_less(starts.size(), attributeCount);
         i += sequenceLength(text[i])) {
      if (!starts.empty() && attributes[starts.size()].is_line_break) {
        ++word;
      }
      starts.push_back(i);
      words.push_back(word);
    }
    std::vector<std::pair<int, int>> spans(
        word + 1, {std::numeric_limits<int>::max(), std::numeric_limits<int>::min()}
    );
    const std::unique_ptr<PangoLayoutIter, decltype(&pango_layout_iter_free)> iter(
        pango_layout_get_iter(layout), &pango_layout_iter_free
    );
    // One pass per cluster of the layout.
    do {
      const auto found =
          std::ranges::upper_bound(starts, static_cast<std::size_t>(pango_layout_iter_get_index(iter.get())));
      if (found == starts.begin()) {
        continue;
      }
      const auto character = static_cast<std::size_t>(found - starts.begin() - 1);
      if (attributes[character].is_white) {
        continue;
      }
      PangoRectangle logical{};
      pango_layout_iter_get_cluster_extents(iter.get(), nullptr, &logical);
      auto& span = spans[words[character]];
      span.first = std::min({span.first, logical.x, logical.x + logical.width});
      span.second = std::max({span.second, logical.x, logical.x + logical.width});
    } while (pango_layout_iter_next_cluster(iter.get()));
    int widest = 0;
    for (const auto& [left, right] : spans) {
      if (left <= right) {
        widest = std::max(widest, right - left);
      }
    }
    return PANGO_PIXELS_CEIL(widest);
  }

  std::optional<TextBox> characterBox(const LineLayout& line, std::size_t column, double originX, double originY) {
    if (column >= line.columnBytes.size()) {
      return std::nullopt;
    }
    PangoRectangle at{};
    pango_layout_index_to_pos(line.layout.get(), line.columnBytes[column], &at);
    const bool end = column + 1 == line.columnBytes.size();
    const double left = at.width < 0 ? at.x + at.width : at.x;
    return TextBox{
        .x = originX + (left / PANGO_SCALE),
        .y = originY + (static_cast<double>(at.y) / PANGO_SCALE),
        .width = end ? 0.0 : std::abs(static_cast<double>(at.width)) / PANGO_SCALE,
        .height = static_cast<double>(at.height) / PANGO_SCALE,
    };
  }

  void fillRect(cairo_t* cr, const Color& color, int x, int y, int width, int height) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_rectangle(cr, x, y, width, height);
    cairo_fill(cr);
  }

  void drawSwatch(
      cairo_t* cr, const SwatchSquare& square, const formats::Rgba& color, const TextTileTheme& theme, int border
  ) {
    const int size = square.size;
    if (color.a < 255) {
      const int cell = std::max(2, size / 4);
      // At most (size / 2) squared cells.
      for (int row = 0; row * cell < size; ++row) {
        for (int column = 0; column * cell < size; ++column) {
          fillRect(
              cr, (row + column) % 2 == 0 ? theme.checkerLight : theme.checkerDark, square.x + (column * cell),
              square.y + (row * cell), std::min(cell, size - (column * cell)), std::min(cell, size - (row * cell))
          );
        }
      }
    }
    const auto byte = [](std::uint8_t value) { return static_cast<float>(value) / 255.0F; };
    fillRect(cr, Color{byte(color.r), byte(color.g), byte(color.b), byte(color.a)}, square.x, square.y, size, size);
    border = std::clamp(border, 0, size / 2);
    fillRect(cr, theme.outline, square.x, square.y, size, border);
    fillRect(cr, theme.outline, square.x, square.y + size - border, size, border);
    fillRect(cr, theme.outline, square.x, square.y + border, border, size - (2 * border));
    fillRect(cr, theme.outline, square.x + size - border, square.y + border, border, size - (2 * border));
  }
} // namespace luciole::text_tiles
