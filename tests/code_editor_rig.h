#pragma once

#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "core/timer_manager.h"
#include "editing/source_lines.h"
#include "editing/text_buffer.h"
#include "formats/file_kind.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/controls/code_editor_view.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// A code editor as the root of a scene drawn offscreen, over a real buffer and styles, with an input dispatcher, the
// default main context and the timer manager.
namespace luciole::test::editor {
  using editing::TextPosition;
  using editing::TextSelection;

  // A clipboard in memory. It answers a request at once or, while held, when answer() is called.
  class MemoryClipboard final : public TextClipboard {
  public:
    std::optional<std::string> text;
    bool hold = false;

    std::optional<std::string> clipboardText() override { return text; }
    void setClipboardText(std::string value) override { text = std::move(value); }
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    void requestClipboardText(std::function<void(std::optional<std::string>)> callback) override {
      if (hold) {
        m_waiting.push_back(std::move(callback));
        return;
      }
      callback(text);
    }
    std::size_t waiting() const { return m_waiting.size(); }
    void answer() {
      auto waiting = std::exchange(m_waiting, {});
      for (auto& callback : waiting) {
        callback(text);
      }
    }

  private:
    std::vector<std::function<void(std::optional<std::string>)>> m_waiting;
  };

  inline editing::TextBuffer bufferOf(std::string_view text) {
    auto buffer = editing::TextBuffer::create(text);
    TEST_CHECK(buffer.has_value());
    return std::move(*buffer);
  }

  inline TextSelection at(std::size_t line, std::size_t byte) {
    return {.anchor = {.line = line, .byte = byte}, .cursor = {.line = line, .byte = byte}};
  }

  inline TextSelection span(TextPosition anchor, TextPosition cursor) { return {.anchor = anchor, .cursor = cursor}; }

  // Lines "line 0" to "line <count - 1>", each followed by a line feed but the last.
  inline std::string numberedLines(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
      out += (i == 0 ? "" : "\n") + std::string("line ") + std::to_string(i);
    }
    return out;
  }

  struct Rig {
    ::test::SceneCapture& capture;
    editing::TextBuffer buffer;
    editing::SourceStyles styles;
    MemoryClipboard clipboard;
    std::unique_ptr<CodeEditorView> view;
    InputDispatcher dispatcher;
    int width = 400;
    int height = 300;
    float scale = 1;
    std::uint32_t held = 0; // modifiers the view reads for the pointer
    std::uint32_t serial = 0;
    std::uint32_t time = 0;
    std::size_t edits = 0;
    std::size_t saves = 0;
    std::size_t escapes = 0;
    std::size_t framesAsked = 0;
    std::vector<std::string> refusals;

    Rig(::test::SceneCapture& rig, std::string_view text, std::string_view name = "notes.txt",
        editing::IndentUnit indent = {}, std::chrono::microseconds budget = CodeEditorView::kDrawBudget)
        : capture(rig), buffer(bufferOf(text)),
          styles(formats::fileKind(name), buffer.lineCount(), [this](std::size_t i) { return buffer.line(i); }),
          view(std::make_unique<CodeEditorView>(buffer, styles, clipboard, indent, budget)) {
      view->setTextSize(12);
      view->onEdited = [this] { ++edits; };
      view->onSaveRequested = [this] { ++saves; };
      view->onEscape = [this] { ++escapes; };
      view->onRefused = [this](const std::string& sentence) { refusals.push_back(sentence); };
      view->onFrameNeeded = [this] { ++framesAsked; };
      view->modifiers = [this] { return held; };
      dispatcher.setSceneRoot(view.get());
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // One frame: main-context work and due timers, the view's update, then layout and drawing.
    ::test::GlPixels frame() {
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      TimerManager::instance().tick();
      view->update();
      return capture.capture(*view, width, height, scale);
    }

    // Frames until every line in view is drawn and a frame then draws nothing and asks for nothing, so the lines kept
    // beside the view are drawn too, whatever the speed of the machine; fails after ten seconds.
    ::test::GlPixels settle() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      for (;;) {
        const auto asked = framesAsked;
        const auto drawn = view->drawnStrips();
        auto pixels = frame();
        if (view->settled() && framesAsked == asked && view->drawnStrips() == drawn) {
          return pixels;
        }
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    void focus() { dispatcher.setFocus(&view->inputArea()); }

    void key(std::uint32_t sym, std::uint32_t modifiers = 0, std::uint32_t utf32 = 0) {
      dispatcher.keyEvent(sym, utf32, modifiers, true);
      dispatcher.keyEvent(sym, utf32, modifiers, false);
    }

    // Types ASCII text one key at a time.
    void type(std::string_view text) {
      for (const char c : text) {
        const auto code = static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        key(code, 0, code);
      }
    }

    bool offer(std::uint32_t sym, std::uint32_t modifiers = 0, std::uint32_t utf32 = 0) {
      return view->key({.sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = true});
    }

    void press(float x, float y) {
      dispatcher.pointerMotion(x, y, ++serial);
      (void)dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, time += 10, false);
    }
    void release(float x, float y) {
      (void)dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, time += 10, false);
    }
    void click(float x, float y) {
      press(x, y);
      release(x, y);
    }

    bool wheel(float x, float y, bool vertical, int notches) {
      dispatcher.pointerEnter(x, y, ++serial);
      return dispatcher.pointerAxis(x, y, vertical ? 0 : 1, 0, 10.0 * notches, notches, 120 * notches, 0);
    }

    TextSelection selection() const { return buffer.selection(); }

    // The caret's place in the view at a position, reached with keys; the selection moves there.
    LayoutRect caretAt(TextPosition position) {
      focus();
      key(XKB_KEY_Home, KeyMod::Ctrl);
      for (std::size_t i = 0; i < position.line; ++i) {
        key(XKB_KEY_Down);
      }
      key(XKB_KEY_Home);
      if (buffer.selection().cursor.byte != 0) {
        key(XKB_KEY_Home);
      }
      while (buffer.selection().cursor.byte < position.byte) {
        key(XKB_KEY_Right);
      }
      TEST_CHECK(buffer.selection().cursor == position);
      return view->caretRect();
    }
  };
} // namespace luciole::test::editor
