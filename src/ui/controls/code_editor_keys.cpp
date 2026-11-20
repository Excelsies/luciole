#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/text_clipboard.h"
#include "editing/text_motion.h"
#include "formats/text_lines.h"
#include "imaging/text_line_strip.h"
#include "ui/controls/code_editor_view.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

namespace {
  namespace editing = luciole::editing;

  bool letter(std::uint32_t sym, char lower) {
    return sym == static_cast<std::uint32_t>(lower) || sym == static_cast<std::uint32_t>(lower - 'a' + 'A');
  }

  std::string utf8(std::uint32_t codePoint) {
    std::string out;
    if (codePoint < 0x80U) {
      out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800U) {
      out += static_cast<char>(0xC0U | (codePoint >> 6U));
      out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    } else if (codePoint < 0x10000U) {
      out += static_cast<char>(0xE0U | (codePoint >> 12U));
      out += static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    } else {
      out += static_cast<char>(0xF0U | (codePoint >> 18U));
      out += static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU));
      out += static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    }
    return out;
  }

  // A character a key types: printable, not DEL and within Unicode.
  bool typed(std::uint32_t utf32) { return utf32 >= 0x20U && utf32 != 0x7FU && utf32 <= 0x10FFFFU; }
} // namespace

bool CodeEditorView::key(const InputArea::KeyData& key) {
  if (!visible()) {
    return false; // a hidden editor keeps its text as it is
  }
  if (!key.pressed || KeySymbol::isModifier(key.sym) || (key.modifiers & (KeyMod::Alt | KeyMod::Super)) != 0) {
    return false;
  }
  const bool ctrl = (key.modifiers & KeyMod::Ctrl) != 0;
  const bool shift = (key.modifiers & KeyMod::Shift) != 0;
  if (key.preedit) {
    if (ctrl || !typed(key.utf32)) {
      return false;
    }
    auto text = utf8(key.utf32);
    const auto size = text.size();
    setComposing(std::move(text), size); // a dead key's preview
    return true;
  }
  setComposing({}, 0);
  if (!KeySymbol::isUp(key.sym)
      && !KeySymbol::isDown(key.sym)
      && !KeySymbol::isPageUp(key.sym)
      && !KeySymbol::isPageDown(key.sym)) {
    m_goalX.reset(); // only vertical moves keep aiming at the same place
  }
  if (ctrl && !shift && letter(key.sym, 's')) {
    if (onSaveRequested) {
      onSaveRequested();
    }
    return true;
  }
  if (!ctrl && !shift && KeySymbol::isEscape(key.sym)) {
    if (onEscape) {
      onEscape();
    }
    return true;
  }
  return editKey(key.sym, key.utf32, ctrl, shift) || moveKey(key.sym, ctrl, shift);
}

bool CodeEditorView::editKey(std::uint32_t sym, std::uint32_t utf32, bool ctrl, bool shift) {
  const auto selected = !m_buffer.selection().empty();
  if (ctrl && letter(sym, 'z')) {
    const auto span = shift ? m_buffer.redo() : m_buffer.undo();
    if (span) {
      applySpan(*span);
    }
    return true;
  }
  if (ctrl && !shift && letter(sym, 'y')) {
    if (const auto span = m_buffer.redo()) {
      applySpan(*span);
    }
    return true;
  }
  if (ctrl && !shift && letter(sym, 'a')) {
    m_buffer.setSelection(editing::selectAll(m_buffer));
    caretMoved();
    return true;
  }
  if (ctrl && !shift && (letter(sym, 'c') || KeySymbol::isInsert(sym))) {
    copy(false);
    return true;
  }
  if ((ctrl && !shift && letter(sym, 'x')) || (!ctrl && shift && KeySymbol::isDelete(sym) && selected)) {
    copy(true);
    return true;
  }
  if ((ctrl && !shift && letter(sym, 'v')) || (!ctrl && shift && KeySymbol::isInsert(sym))) {
    paste();
    return true;
  }
  const auto unit = ctrl ? editing::EraseUnit::Word : editing::EraseUnit::Cluster;
  editing::EditResult result;
  if (KeySymbol::isBackspace(sym)) {
    result = m_buffer.eraseBackward(unit, nowMs());
  } else if (KeySymbol::isDelete(sym)) {
    result = m_buffer.eraseForward(unit, nowMs());
  } else if (!ctrl && KeySymbol::isEnter(sym)) {
    result = m_buffer.newline(nowMs());
  } else if (!ctrl && KeySymbol::isTab(sym)) {
    const bool outward = shift || sym != XKB_KEY_Tab;
    result = outward ? m_buffer.outdent(m_indent, nowMs()) : m_buffer.indent(m_indent, nowMs());
  } else if (!ctrl && typed(utf32)) {
    result = m_buffer.insertText(utf8(utf32), nowMs());
  } else {
    return false;
  }
  applyEdit(result);
  return true;
}

bool CodeEditorView::moveKey(std::uint32_t sym, bool ctrl, bool shift) {
  const auto selection = m_buffer.selection();
  const auto cursor = selection.cursor;
  if (KeySymbol::isLeft(sym) || KeySymbol::isRight(sym)) {
    const bool left = KeySymbol::isLeft(sym);
    if (!shift && !selection.empty()) {
      moveTo(left ? selection.start() : selection.end(), false);
    } else if (ctrl) {
      moveTo(left ? editing::wordLeft(m_buffer, cursor) : editing::wordRight(m_buffer, cursor), shift);
    } else {
      moveTo(left ? editing::clusterLeft(m_buffer, cursor) : editing::clusterRight(m_buffer, cursor), shift);
    }
    return true;
  }
  if (KeySymbol::isHome(sym)) {
    moveTo(ctrl ? editing::documentStart(m_buffer) : editing::lineStart(m_buffer, cursor), shift);
    return true;
  }
  if (KeySymbol::isEnd(sym)) {
    moveTo(ctrl ? editing::documentEnd(m_buffer) : editing::lineEnd(m_buffer, cursor), shift);
    return true;
  }
  if (ctrl) {
    return false;
  }
  if (KeySymbol::isUp(sym) || KeySymbol::isDown(sym)) {
    moveVertically(KeySymbol::isUp(sym) ? -1 : 1, shift);
    return true;
  }
  if (KeySymbol::isPageUp(sym) || KeySymbol::isPageDown(sym)) {
    const double s = deviceScale();
    const auto page = m_lineHeight > 0
        ? std::max<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(std::floor(m_viewportHeight * s / m_lineHeight)))
        : 1;
    const auto lines = KeySymbol::isPageUp(sym) ? -page : page;
    scrollTo(m_scrollX, m_scrollY + static_cast<float>(static_cast<double>(lines) * m_lineHeight / s));
    moveVertically(lines, shift);
    return true;
  }
  return false;
}

void CodeEditorView::moveVertically(std::ptrdiff_t lines, bool shift) {
  const auto selection = m_buffer.selection();
  const auto cursor = selection.cursor;
  const auto last = m_buffer.lineCount() - 1;
  if (!m_goalX) {
    m_goalX = m_caretX;
  }
  editing::TextPosition target;
  if (lines < 0 && cursor.line == 0) {
    target = {.line = 0, .byte = 0};
  } else if (lines > 0 && cursor.line == last) {
    target = editing::lineEnd(m_buffer, cursor);
  } else {
    const auto distance = static_cast<std::size_t>(std::abs(lines));
    const auto line =
        lines < 0 ? cursor.line - std::min(distance, cursor.line) : std::min(cursor.line + distance, last);
    const auto column = strip().columnAt(styledLine(line), *m_goalX);
    target = {.line = line, .byte = luciole::formats::tokenize::prefixBytes(m_buffer.line(line), column)};
  }
  m_buffer.setSelection({.anchor = shift ? selection.anchor : target, .cursor = target});
  caretMoved();
}

void CodeEditorView::moveTo(editing::TextPosition position, bool shift) {
  const auto anchor = shift ? m_buffer.selection().anchor : position;
  m_buffer.setSelection({.anchor = anchor, .cursor = position});
  caretMoved();
}

void CodeEditorView::copy(bool cut) {
  const auto selection = m_buffer.selection();
  if (selection.empty()) {
    return;
  }
  m_clipboard.setClipboardText(m_buffer.textBetween(selection.anchor, selection.cursor));
  if (cut) {
    applyEdit(m_buffer.insertText({}, nowMs()));
  }
}

void CodeEditorView::paste() {
  const std::weak_ptr<int> alive = m_alive;
  const auto revision = m_buffer.revision();
  const auto losses = m_focusLosses;
  m_clipboard.requestClipboardText([this, alive, revision, losses](std::optional<std::string> text) {
    if (alive.expired() || !text || text->empty()) {
      return;
    }
    if (revision != m_buffer.revision() || losses != m_focusLosses || !m_area->focused() || !visible()) {
      return; // the text or the focus changed while the clipboard answered
    }
    m_goalX.reset();
    setComposing({}, 0);
    applyEdit(m_buffer.insertText(*text, nowMs()));
  });
}

std::uint64_t CodeEditorView::nowMs() const {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}
