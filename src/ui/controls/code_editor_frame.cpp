#include "imaging/text_line_strip.h"
#include "render/core/renderer.h"
#include "ui/controls/box.h"
#include "ui/controls/code_editor_lines.h"
#include "ui/controls/code_editor_view.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <memory>
#include <string>

namespace {
  namespace formats = luciole::formats;
  using luciole::code_editor::codePoints;
} // namespace

bool CodeEditorView::settled() const {
  if (!visible()) {
    return true;
  }
  const StripKey wanted{.scale = m_scale, .textSize = m_textSize, .palette = m_palette};
  if (!m_strip || m_stripKey != wanted || m_lineHeight == 0) {
    return false;
  }
  const auto view = linesInView();
  const auto s = deviceScale();
  const int left = static_cast<int>(std::round(m_scrollX * s)) - margin();
  const auto width = static_cast<int>(std::ceil(m_viewportWidth * s));
  for (auto line = view.first; line <= view.last; ++line) {
    if (!m_lines->current(line, left, width)) {
      return false;
    }
  }
  return !m_bandsDirty;
}

void CodeEditorView::update() {
  if (!visible() || m_textures == nullptr || m_viewportWidth <= 0 || m_viewportHeight <= 0) {
    return;
  }
  const auto before = place();
  if (ensureStrip()) {
    computeCaret();
    keepPlace(before);
  }
  const auto view = linesInView();
  const std::size_t first = view.first > kLinesBeside ? view.first - kLinesBeside : 0;
  const std::size_t last = std::min(view.last + kLinesBeside, m_buffer.lineCount() - 1);
  m_lines->keepOnly(first, last);
  const double s = deviceScale();
  const int left = static_cast<int>(std::round(m_scrollX * s)) - margin();
  const auto width = static_cast<int>(std::ceil(m_viewportWidth * s));
  const auto start = std::chrono::steady_clock::now();
  bool drew = false;
  bool more = false;
  // Draws the line when it is due and the budget allows; false once the budget is spent.
  const auto visit = [&](std::size_t line) {
    if (m_lines->current(line, left, width)) {
      return true;
    }
    if (drew && std::chrono::steady_clock::now() - start >= m_drawBudget) {
      more = true;
      return false;
    }
    drawStrip(line);
    drew = true;
    return true;
  };
  const auto caretLine = m_buffer.selection().cursor.line;
  bool going = caretLine < view.first || caretLine > view.last || visit(caretLine);
  // At most the lines in view, then the few kept beside them.
  for (auto line = view.first; going && line <= view.last; ++line) {
    going = visit(line);
  }
  for (auto line = view.last + 1; going && line <= last; ++line) {
    going = visit(line);
  }
  for (auto line = first; going && line < view.first; ++line) {
    going = visit(line);
  }
  if (m_bandsDirty) {
    computeBands();
  }
  if (more) {
    needFrame();
  }
  if (drew) {
    markLayoutDirty();
  }
}

void CodeEditorView::drawStrip(std::size_t line) {
  auto& drawer = strip();
  const auto shown = displayLine(line);
  const int lineWidth = drawer.lineWidth(shown);
  m_widest = std::max(m_widest, lineWidth);
  const double s = deviceScale();
  const int left = static_cast<int>(std::round(m_scrollX * s)) - margin();
  const auto window =
      luciole::code_editor::stripWindow(lineWidth, left, static_cast<int>(std::ceil(m_viewportWidth * s)));
  luciole::ColumnRange composing;
  if (!m_composing.empty() && line == m_buffer.selection().cursor.line) {
    composing.begin = columnOf(m_buffer.selection().cursor);
    composing.end = composing.begin + codePoints(m_composing);
  }
  auto drawn = window.width > 0 ? drawer.render(shown, window.x0, window.width, composing)
                                : std::expected<luciole::TextStrip, std::string>(luciole::TextStrip{});
  if (!drawn) {
    drawn = luciole::TextStrip{}; // a line Cairo cannot draw stays blank
  }
  m_lines->put(*m_textures, line, *drawn, lineWidth);
  ++m_drawn;
}

void CodeEditorView::computeBands() {
  m_bandsDirty = false;
  m_bands.clear();
  const auto selection = m_buffer.selection();
  if (!selection.empty() && visible() && m_lineHeight > 0) {
    const auto view = linesInView();
    const auto start = selection.start();
    const auto end = selection.end();
    // At most the lines in view.
    for (auto line = std::max(start.line, view.first); line <= std::min(end.line, view.last); ++line) {
      Band band{.line = line, .left = 0, .right = std::nullopt};
      if (line == start.line) {
        band.left = strip().caretX(styledLine(line), columnOf(start));
      }
      if (line == end.line) {
        band.right = strip().caretX(styledLine(line), columnOf(end));
      }
      if (!band.right || *band.right > band.left) {
        m_bands.push_back(band);
      }
    }
  }
  // At most one box per line in view is made.
  while (m_bandBoxes.size() < m_bands.size()) {
    auto box = std::make_unique<Box>();
    box->setFill(colorSpecFromRole(ColorRole::PrimaryContainer));
    m_bandBoxes.push_back(static_cast<Box*>(m_bandLayer->addChild(std::move(box))));
  }
  markLayoutDirty();
}

void CodeEditorView::doLayout(Renderer& renderer) {
  if (!visible()) {
    return;
  }
  m_textures = &renderer.textureManager();
  const float scale = renderer.renderScale();
  const double pixelScale = std::isfinite(scale) && scale > 0 ? scale : 1.0;
  const float w = width();
  const float h = height();
  if (w != m_viewportWidth || h != m_viewportHeight || pixelScale != m_scale) {
    m_viewportWidth = w;
    m_viewportHeight = h;
    m_scale = pixelScale;
    m_bandsDirty = true;
    needFrame();
  }
  m_scrollX = std::min(m_scrollX, maxScrollX());
  m_scrollY = std::min(m_scrollY, maxScrollY());
  m_background->setFrameSize(w, h);
  m_layer->setFrameSize(w, h);
  m_area->setFrameSize(w, h);
  placeOverlay();
  layoutBars(renderer, w, h);
}

void CodeEditorView::placeOverlay() {
  const double s = deviceScale();
  const int originX = margin() - static_cast<int>(std::round(m_scrollX * s));
  const int originY = margin() - static_cast<int>(std::round(m_scrollY * s));
  m_lines->place(s, originX, originY, m_lineHeight);
  for (std::size_t i = 0; i < m_bandBoxes.size(); ++i) {
    auto* box = m_bandBoxes[i];
    box->setVisible(i < m_bands.size());
    if (i >= m_bands.size()) {
      continue;
    }
    const auto& band = m_bands[i];
    const double left = originX + std::round(band.left);
    const double right = band.right ? originX + std::round(*band.right) : m_viewportWidth * s;
    const double top = originY + (static_cast<double>(band.line) * m_lineHeight);
    box->setPosition(static_cast<float>(left / s), static_cast<float>(top / s));
    box->setFrameSize(static_cast<float>(std::max(0.0, right - left) / s), static_cast<float>(m_lineHeight / s));
  }
  const auto caret = caretRect();
  m_caret->setPosition(caret.x, caret.y);
  m_caret->setFrameSize(caret.width, caret.height);
  m_caret->setVisible(m_caretOn && m_area->focused() && m_lineHeight > 0);
}

void CodeEditorView::layoutBars(Renderer& renderer, float width, float height) {
  const float thickness = Style::scrollbarWidth;
  const auto content = contentSize();
  const bool vertical = maxScrollY() > 0;
  const bool horizontal = maxScrollX() > 0;
  m_verticalBar->setVisible(vertical);
  m_horizontalBar->setVisible(horizontal);
  if (vertical) {
    m_verticalBar->update(height, content.height + (horizontal ? thickness : 0.0F), m_scrollY);
    m_verticalBar->arrange(renderer, {width - thickness, 0, thickness, height - (horizontal ? thickness : 0.0F)});
  }
  if (horizontal) {
    m_horizontalBar->update(width, content.width + (vertical ? thickness : 0.0F), m_scrollX);
    m_horizontalBar->arrange(renderer, {0, height - thickness, width - (vertical ? thickness : 0.0F), thickness});
  }
}

void CodeEditorView::doInvalidateGpuResources(Renderer& /*renderer*/) {
  m_lines->forget();
  markLayoutDirty();
  needFrame();
}
