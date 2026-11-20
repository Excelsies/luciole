#include "ui/controls/code_editor_view.h"

#include "core/text_clipboard.h"
#include "imaging/text_line_strip.h"
#include "render/core/renderer.h"
#include "ui/controls/box.h"
#include "ui/controls/code_editor_lines.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/text_tile_view_style.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/text_input_service.h"

#include <algorithm>
#include <cmath>
#include <expected>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
  namespace editing = luciole::editing;
  namespace formats = luciole::formats;
  using luciole::code_editor::codePoints;
  using luciole::code_editor::LineStrips;

  constexpr float kCodeScale = 0.95F; // code is set as the text preview sets it, a little smaller than prose
  constexpr float kMargin = 8.0F;     // logical pixels around the text, as around a preview's document
  constexpr float kCaretWidth = 1.25F;
  constexpr auto kBlinkInterval = std::chrono::milliseconds(530);
  constexpr auto kBlinkResume = std::chrono::milliseconds(650);

  struct Overflow {
    float x = 0;
    float y = 0;
  };

  // How far content scrolls each way in a viewport whose bars lie over the content's edges.
  Overflow overflow(float contentWidth, float contentHeight, float width, float height) {
    const float bar = Style::scrollbarWidth;
    const float pastX = contentWidth - width;
    const float pastY = contentHeight - height;
    bool across = pastX > 0;
    const bool down = pastY + (across ? bar : 0.0F) > 0;
    across = pastX + (down ? bar : 0.0F) > 0;
    return {
        .x = across ? pastX + (down ? bar : 0.0F) : 0.0F,
        .y = down ? pastY + (across ? bar : 0.0F) : 0.0F,
    };
  }
} // namespace

CodeEditorView::CodeEditorView(
    editing::TextBuffer& buffer, editing::SourceStyles& styles, TextClipboard& clipboard, editing::IndentUnit indent,
    std::chrono::microseconds drawBudget
)
    : m_buffer(buffer), m_styles(styles), m_clipboard(clipboard), m_indent(indent),
      m_drawBudget(std::max(drawBudget, std::chrono::microseconds{0})) {
  if (styles.lineCount() != buffer.lineCount()) {
    throw std::invalid_argument("The styles do not describe the buffer's lines.");
  }
  setClipChildren(true);
  m_background = static_cast<Box*>(addChild(std::make_unique<Box>()));
  m_background->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
  m_layer = addChild(std::make_unique<Node>());
  m_layer->setClipChildren(true);
  m_layer->setHitTestVisible(false);
  m_bandLayer = m_layer->addChild(std::make_unique<Node>());
  m_stripLayer = m_layer->addChild(std::make_unique<Node>());
  m_caret = static_cast<Box*>(m_layer->addChild(std::make_unique<Box>()));
  m_caret->setFill(colorSpecFromRole(ColorRole::Primary));
  m_caret->setVisible(false);
  m_lines = std::make_unique<LineStrips>(*m_stripLayer);
  m_area = static_cast<InputArea*>(addChild(std::make_unique<InputArea>()));
  wirePointer();
  m_verticalBar = static_cast<Scrollbar*>(addChild(std::make_unique<Scrollbar>()));
  m_verticalBar->setOnScrollChanged([this](float offset) { scrollTo(m_scrollX, offset); });
  m_horizontalBar = static_cast<Scrollbar*>(addChild(std::make_unique<Scrollbar>()));
  m_horizontalBar->setOrientation(ScrollOrientation::Horizontal);
  m_horizontalBar->setOnScrollChanged([this](float offset) { scrollTo(offset, m_scrollY); });
  m_paletteConn = paletteChanged().connect([this] {
    ++m_palette;
    needFrame();
  });
}

CodeEditorView::~CodeEditorView() {
  m_alive.reset();
  // The input method service resets the composition as it lets go, which must reach neither the owner nor a timer.
  onEdited = nullptr;
  onSaveRequested = nullptr;
  onEscape = nullptr;
  onRefused = nullptr;
  onFrameNeeded = nullptr;
  m_composing.clear();
  m_composingCursor = 0;
  m_blinkTimer.stop();
  if (m_textInput != nullptr) {
    m_textInput->clearFocusedClient(this);
  }
  m_lines->clear();
}

void CodeEditorView::setTextSize(float size) {
  const float clamped = std::isnan(size) ? kDefaultTextSize : std::clamp(size, kMinTextSize, kMaxTextSize);
  if (clamped == m_textSize) {
    return;
  }
  m_textSize = clamped;
  m_goalX.reset();
  markLayoutDirty();
  needFrame();
}

void CodeEditorView::setShown(bool shown) {
  if (shown == visible()) {
    return;
  }
  setVisible(shown);
  if (!shown) {
    stopBlink();
    setComposing({}, 0);
    m_lines->clear();
    m_strip.reset(); // the key stays, so showing again keeps the measured widths
    m_bands.clear();
    m_bandsDirty = true;
    return;
  }
  if (m_area->focused()) {
    restartBlink();
  }
  needFrame();
}

void CodeEditorView::scrollTo(float x, float y) {
  const float clampedX = std::isnan(x) ? 0.0F : std::clamp(x, 0.0F, maxScrollX());
  const float clampedY = std::isnan(y) ? 0.0F : std::clamp(y, 0.0F, maxScrollY());
  if (clampedX == m_scrollX && clampedY == m_scrollY) {
    return;
  }
  m_scrollX = clampedX;
  m_scrollY = clampedY;
  m_bandsDirty = true;
  markLayoutDirty();
  needFrame();
}

LayoutSize CodeEditorView::contentSize() const {
  const double s = deviceScale();
  const double width = (2.0 * margin()) + m_widest + std::ceil(kCaretWidth * s);
  const double height = (2.0 * margin()) + (static_cast<double>(m_buffer.lineCount()) * m_lineHeight);
  return {.width = static_cast<float>(width / s), .height = static_cast<float>(height / s)};
}

float CodeEditorView::maxScrollX() const noexcept {
  const auto content = contentSize();
  return overflow(content.width, content.height, m_viewportWidth, m_viewportHeight).x;
}

float CodeEditorView::maxScrollY() const noexcept {
  if (m_lineHeight == 0) {
    return 0;
  }
  const auto content = contentSize();
  return overflow(content.width, content.height, m_viewportWidth, m_viewportHeight).y;
}

LayoutRect CodeEditorView::caretRect() const {
  const double s = deviceScale();
  const auto line = static_cast<double>(m_buffer.selection().cursor.line);
  const double x = margin() - std::round(m_scrollX * s) + std::round(m_caretX);
  const double y = margin() - std::round(m_scrollY * s) + (line * m_lineHeight);
  return {
      .x = static_cast<float>(x / s),
      .y = static_cast<float>(y / s),
      .width = static_cast<float>(std::max(1.0, std::round(kCaretWidth * s)) / s),
      .height = static_cast<float>(m_lineHeight / s),
  };
}

std::size_t CodeEditorView::residentStrips() const { return m_lines->count(); }

std::size_t CodeEditorView::textureBytes() const { return m_lines->bytes(); }

bool CodeEditorView::ensureStrip() {
  const StripKey wanted{.scale = m_scale, .textSize = m_textSize, .palette = m_palette};
  if (m_strip && m_stripKey == wanted) {
    return false;
  }
  const bool samePlaces = m_stripKey && m_stripKey->scale == wanted.scale && m_stripKey->textSize == wanted.textSize;
  if (samePlaces) {
    m_lines->markStale(0, std::numeric_limits<std::size_t>::max());
  } else {
    m_lines->clear();
    m_widest = 0;
  }
  m_strip = std::make_unique<luciole::TextLineStrip>(luciole::TextLineStripOptions{
      .scale = m_scale,
      .theme = luciole::text_tile_view::paletteTheme(),
      .fonts = {.proseSize = m_textSize, .codeSize = m_textSize * kCodeScale},
  });
  m_stripKey = wanted;
  m_lineHeight = m_strip->lineHeight();
  m_bandsDirty = true;
  return true;
}

luciole::TextLineStrip& CodeEditorView::strip() {
  (void)ensureStrip();
  return *m_strip;
}

formats::StyledLine CodeEditorView::styledLine(std::size_t index) {
  auto line = m_styles.line(index);
  if (!line) {
    throw std::logic_error("The editor's styles lost track of its text.");
  }
  return std::move(*line);
}

formats::StyledLine CodeEditorView::displayLine(std::size_t index) {
  auto line = styledLine(index);
  const auto cursor = m_buffer.selection().cursor;
  if (m_composing.empty() || index != cursor.line) {
    return line;
  }
  return luciole::code_editor::withText(line, columnOf(cursor), m_composing);
}

std::size_t CodeEditorView::columnOf(editing::TextPosition position) const {
  const auto line = m_buffer.line(position.line);
  return codePoints(line.substr(0, std::min(position.byte, line.size())));
}

std::size_t CodeEditorView::composingColumns() const {
  return codePoints(std::string_view(m_composing).substr(0, m_composingCursor));
}

int CodeEditorView::margin() const { return static_cast<int>(std::round(kMargin * deviceScale())); }

double CodeEditorView::deviceScale() const { return m_stripKey ? m_stripKey->scale : m_scale; }

CodeEditorView::Lines CodeEditorView::linesInView() const {
  const std::size_t count = m_buffer.lineCount();
  if (m_lineHeight <= 0) {
    return {.first = 0, .last = 0};
  }
  const double s = deviceScale();
  const double top = std::round(m_scrollY * s) - margin();
  const double bottom = top + (m_viewportHeight * s);
  const auto lineAt = [&](double y) {
    return static_cast<std::size_t>(std::clamp(std::floor(y / m_lineHeight), 0.0, static_cast<double>(count - 1)));
  };
  return {.first = lineAt(top), .last = lineAt(std::max(top, bottom - 1))};
}

void CodeEditorView::computeCaret() {
  if (!visible()) {
    return;
  }
  auto& drawer = strip();
  const auto cursor = m_buffer.selection().cursor;
  const auto shown = displayLine(cursor.line);
  m_widest = std::max(m_widest, drawer.lineWidth(shown));
  m_caretX = drawer.caretX(shown, columnOf(cursor) + composingColumns());
}

void CodeEditorView::reveal() {
  if (!visible() || m_lineHeight == 0 || m_viewportWidth <= 0 || m_viewportHeight <= 0) {
    return;
  }
  const double s = deviceScale();
  const double bar = Style::scrollbarWidth * s;
  const auto line = static_cast<double>(m_buffer.selection().cursor.line);
  const double top = line * m_lineHeight;
  const double bottom = top + m_lineHeight + (2.0 * margin());
  const double roomY = (m_viewportHeight * s) - (maxScrollX() > 0 ? bar : 0.0);
  double y = std::round(m_scrollY * s);
  if (bottom - y > roomY) {
    y = bottom - roomY;
  }
  y = std::min(y, top);
  const double right = m_caretX + (2.0 * margin()) + std::ceil(kCaretWidth * s);
  const double roomX = (m_viewportWidth * s) - (maxScrollY() > 0 ? bar : 0.0);
  double x = std::round(m_scrollX * s);
  if (right - x > roomX) {
    x = right - roomX;
  }
  x = std::min(x, m_caretX);
  scrollTo(static_cast<float>(x / s), static_cast<float>(y / s));
}

CodeEditorView::Place CodeEditorView::place() const {
  if (m_lineHeight <= 0) {
    return {};
  }
  const double s = deviceScale();
  const auto caret = caretRect();
  return {
      .lineHeight = m_lineHeight,
      .scale = s,
      .topLine = (std::round(m_scrollY * s) - margin()) / m_lineHeight,
      .caretShown = caret.x >= 0
          && caret.y >= 0
          && caret.x + caret.width <= m_viewportWidth
          && caret.y + caret.height <= m_viewportHeight,
  };
}

void CodeEditorView::keepPlace(const Place& before) {
  if (before.lineHeight <= 0 || m_lineHeight <= 0) {
    return;
  }
  const double s = deviceScale();
  if (before.lineHeight == m_lineHeight && before.scale == s) {
    return;
  }
  scrollTo(m_scrollX, static_cast<float>(((before.topLine * m_lineHeight) + margin()) / s));
  if (before.caretShown) {
    reveal();
  }
}

void CodeEditorView::caretMoved() {
  computeCaret();
  reveal();
  m_bandsDirty = true;
  restartBlink();
  notifyInputMethod();
  markLayoutDirty();
  needFrame();
}

void CodeEditorView::applyEdit(const editing::EditResult& result) {
  if (!result) {
    if (onRefused) {
      onRefused(result.error());
    }
    return;
  }
  if (!*result) {
    caretMoved();
    return;
  }
  applySpan(**result);
}

void CodeEditorView::applySpan(const editing::LineSpan& span) {
  const auto changed = m_styles.edited(span.firstLine, span.removedLines, span.insertedLines);
  if (!changed) {
    throw std::logic_error("The editor's styles lost track of its text.");
  }
  m_lines->edited(span.firstLine, span.removedLines, span.insertedLines);
  m_lines->markStale(changed->first, changed->count);
  caretMoved();
  if (onEdited) {
    onEdited();
  }
}

void CodeEditorView::restartBlink() {
  m_caretOn = true;
  if (!m_area->focused() || !visible()) {
    m_blinkTimer.stop();
    return;
  }
  m_blinkTimer.start(kBlinkResume, [this] {
    m_blinkTimer.startRepeating(kBlinkInterval, [this] {
      m_caretOn = !m_caretOn;
      m_caret->setVisible(m_caretOn && m_area->focused());
    });
  });
}

void CodeEditorView::stopBlink() {
  m_blinkTimer.stop();
  m_caretOn = false;
  m_caret->setVisible(false);
}

void CodeEditorView::needFrame() {
  if (onFrameNeeded && visible()) {
    onFrameNeeded();
  }
}
