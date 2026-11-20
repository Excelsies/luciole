#include "ui/controls/code_editor_lines.h"
#include "ui/controls/code_editor_view.h"
#include "wayland/text_input_service.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace {
  namespace editing = luciole::editing;

  std::int32_t protocolInt(std::size_t value) {
    return static_cast<std::int32_t>(std::min<std::size_t>(value, std::numeric_limits<std::int32_t>::max()));
  }

  // The start of the code point at or before `byte`.
  std::size_t codePointStart(std::string_view text, std::size_t byte) {
    byte = std::min(byte, text.size());
    // Steps back over at most three continuation bytes of valid UTF-8.
    while (byte > 0 && byte < text.size() && (static_cast<unsigned char>(text[byte]) & 0xC0U) == 0x80U) {
      --byte;
    }
    return byte;
  }
} // namespace

TextInputState CodeEditorView::textInputState() const {
  const auto selection = m_buffer.selection();
  const auto cursor = selection.cursor;
  const auto anchor = selection.anchor.line == cursor.line ? selection.anchor.byte : cursor.byte;
  const auto caret = caretRect();
  float x = 0;
  float y = 0;
  Node::absolutePosition(this, x, y);
  return TextInputState{
      .surroundingText = std::string(m_buffer.line(cursor.line)),
      .cursor = protocolInt(cursor.byte),
      .anchor = protocolInt(anchor),
      .cursorRectX = static_cast<std::int32_t>(std::round(x + caret.x)),
      .cursorRectY = static_cast<std::int32_t>(std::round(y + caret.y)),
      .cursorRectWidth = static_cast<std::int32_t>(std::max(1.0F, std::round(caret.width))),
      .cursorRectHeight = static_cast<std::int32_t>(std::max(1.0F, std::round(caret.height))),
  };
}

void CodeEditorView::textInputApplyEdit(const TextInputEdit& edit) {
  if (!visible()) {
    return; // a hidden editor keeps its text as it is
  }
  const std::weak_ptr<int> alive = m_alive;
  m_inputMethodEdit = true;
  setComposing({}, 0);
  if (edit.hasDelete && (edit.deleteBeforeLength > 0 || edit.deleteAfterLength > 0)) {
    const auto cursor = m_buffer.selection().cursor;
    const auto line = m_buffer.line(cursor.line);
    const auto before = std::min<std::size_t>(edit.deleteBeforeLength, cursor.byte);
    const auto after = std::min<std::size_t>(edit.deleteAfterLength, line.size() - cursor.byte);
    m_buffer.setSelection({
        .anchor = {.line = cursor.line, .byte = codePointStart(line, cursor.byte - before)},
        .cursor = {.line = cursor.line, .byte = codePointStart(line, cursor.byte + after)},
    });
    applyEdit(m_buffer.insertText({}, nowMs()));
    if (alive.expired()) {
      return;
    }
  }
  if (edit.hasCommitText && !edit.commitText.empty()) {
    applyEdit(m_buffer.insertText(edit.commitText, nowMs()));
    if (alive.expired()) {
      return;
    }
  }
  if (edit.hasPreedit && !edit.preeditText.empty()) {
    const auto size = edit.preeditText.size();
    const auto cursor = edit.preeditCursorBegin < 0 ? size : static_cast<std::size_t>(edit.preeditCursorBegin);
    setComposing(edit.preeditText, codePointStart(edit.preeditText, std::min(cursor, size)));
  }
  m_inputMethodEdit = false;
}

void CodeEditorView::textInputResetPreedit() { setComposing({}, 0); }

void CodeEditorView::textInputActivated(TextInputService& service) { m_textInput = &service; }

void CodeEditorView::textInputDeactivated(TextInputService& service) {
  if (m_textInput == &service) {
    m_textInput = nullptr;
  }
}

void CodeEditorView::setComposing(std::string text, std::size_t cursor) {
  if (text == m_composing && cursor == m_composingCursor) {
    return;
  }
  m_composing = std::move(text);
  m_composingCursor = std::min(cursor, m_composing.size());
  m_lines->markStale(m_buffer.selection().cursor.line, 1);
  if (visible()) {
    computeCaret();
    reveal();
    restartBlink();
    markLayoutDirty();
    needFrame();
  }
}

void CodeEditorView::notifyInputMethod() {
  if (m_textInput != nullptr && !m_inputMethodEdit) {
    m_textInput->notifyClientStateChanged(this, TextInputChangeCause::Other);
  }
}
