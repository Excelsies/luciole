#include "imaging/text_line_glyphs.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace luciole::text_tiles {
  namespace {
    double device(std::int64_t units) { return static_cast<double>(units) / PANGO_SCALE; }
  } // namespace

  GlyphWindow::GlyphWindow(PangoLayout* layout, double from, double to) {
    const std::unique_ptr<PangoLayoutIter, decltype(&pango_layout_iter_free)> iter(
        pango_layout_get_iter(layout), &pango_layout_iter_free
    );
    // One pass per run of the layout.
    do {
      PangoLayoutRun* run = pango_layout_iter_get_run(iter.get());
      if (run == nullptr) {
        continue;
      }
      PangoRectangle logical{};
      pango_layout_iter_get_run_extents(iter.get(), nullptr, &logical);
      const std::int64_t baseline = pango_layout_iter_get_baseline(iter.get());
      std::int64_t x = logical.x;
      // One pass per glyph of the run, in visual order.
      for (int i = 0; i < run->glyphs->num_glyphs; ++i) {
        auto& glyph = run->glyphs->glyphs[i];
        const std::int64_t origin = x + glyph.geometry.x_offset;
        const std::int64_t top = baseline + glyph.geometry.y_offset;
        x += glyph.geometry.width;
        if (glyph.glyph == PANGO_GLYPH_EMPTY) {
          continue;
        }
        PangoRectangle ink{};
        pango_font_get_glyph_extents(run->item->analysis.font, glyph.glyph, &ink, nullptr);
        const InkBox box{
            .left = device(origin + ink.x),
            .top = device(top + ink.y),
            .right = device(origin + ink.x + ink.width),
            .bottom = device(top + ink.y + ink.height),
        };
        const bool shown = !box.empty() && box.right > from && box.left < to;
        if (shown && m_ink.empty()) {
          m_ink = box;
        } else if (shown) {
          m_ink = {
              .left = std::min(m_ink.left, box.left),
              .top = std::min(m_ink.top, box.top),
              .right = std::max(m_ink.right, box.right),
              .bottom = std::max(m_ink.bottom, box.bottom),
          };
        }
        if (!shown) {
          m_hidden.emplace_back(&glyph, glyph.glyph);
          glyph.glyph = PANGO_GLYPH_EMPTY;
        }
      }
    } while (pango_layout_iter_next_run(iter.get()));
  }

  GlyphWindow::~GlyphWindow() {
    for (const auto& [glyph, id] : m_hidden) {
      glyph->glyph = id;
    }
  }
} // namespace luciole::text_tiles
