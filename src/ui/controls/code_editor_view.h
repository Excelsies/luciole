#pragma once

#include "core/timer_manager.h"
#include "editing/source_lines.h"
#include "editing/text_buffer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/signal.h"
#include "ui/text_input_client.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Box;
class Scrollbar;
class TextClipboard;
class TextureManager;

namespace luciole {
  class TextLineStrip;
  namespace code_editor {
    class LineStrips;
  }
} // namespace luciole

// Edits a text in place: every line drawn as its own strip in the colors of the read-only preview, with a caret, a
// selection, the keyboard, the pointer, the clipboard and input methods. Nothing wraps; long lines scroll sideways.
//
// Only the lines in view and a few beside them hold textures. A strip is kept until its line's content or style
// changes, so typing redraws one line and inserting lines moves the others without drawing them. Strips missing
// after a scroll are drawn within the draw budget each update, the caret's line first. A hidden or destroyed view
// holds no texture and runs no timer; an idle one runs only the caret's blink while it has the keyboard.
//
// The owner calls update() once per frame before layout; onFrameNeeded asks for that frame.
class CodeEditorView final : public Node, public TextInputClient {
public:
  static constexpr std::chrono::microseconds kDrawBudget{4000};
  static constexpr float kDefaultTextSize = 12.0F;
  static constexpr float kMinTextSize = 6.0F;
  static constexpr float kMaxTextSize = 48.0F;
  // Lines beside the viewport that keep their strips.
  static constexpr std::size_t kLinesBeside = 4;

  // The buffer, the styles of its lines and the clipboard belong to the caller and outlive the view; every change to
  // the buffer goes through the view. Tab and Shift+Tab indent and outdent by `indent`. Each update draws strips for
  // at most `drawBudget`, and always at least one. Throws std::invalid_argument when the styles do not have the
  // buffer's line count.
  CodeEditorView(
      luciole::editing::TextBuffer& buffer, luciole::editing::SourceStyles& styles, TextClipboard& clipboard,
      luciole::editing::IndentUnit indent, std::chrono::microseconds drawBudget = kDrawBudget
  );
  ~CodeEditorView() override;
  CodeEditorView(const CodeEditorView&) = delete;
  CodeEditorView& operator=(const CodeEditorView&) = delete;

  // Handles a pressed key the editor knows; true when it was consumed. Other keys, releases and chords with Alt or
  // Super are left to the caller, and so is every key while the view is hidden. The view's own input area calls this
  // for the keys it receives.
  bool key(const InputArea::KeyData& key);

  // Logical pixels of prose; code, as in the preview, is a little smaller. Clamped to [kMinTextSize, kMaxTextSize].
  void setTextSize(float size);
  // Hiding releases every texture and stops the blink; showing draws the lines in view again at the same place.
  void setShown(bool shown);
  void update();
  // Logical pixels, clamped to the content.
  void scrollTo(float x, float y);

  float scrollX() const noexcept { return m_scrollX; }
  float scrollY() const noexcept { return m_scrollY; }
  float maxScrollX() const noexcept;
  float maxScrollY() const noexcept;
  // The caret in logical pixels from the view's top left, a line high.
  LayoutRect caretRect() const;
  // Text an input method is composing at the caret; it is shown, not part of the buffer.
  const std::string& composing() const noexcept { return m_composing; }
  std::size_t residentStrips() const;
  std::size_t textureBytes() const;
  // Strips drawn since the view was made.
  std::uint64_t drawnStrips() const noexcept { return m_drawn; }
  // Every line in view holds a current strip.
  bool settled() const;
  // The caret's blink timer runs.
  bool blinking() const noexcept { return m_blinkTimer.active(); }

  InputArea& inputArea() const noexcept { return *m_area; }
  const Scrollbar& verticalBar() const noexcept { return *m_verticalBar; }
  const Scrollbar& horizontalBar() const noexcept { return *m_horizontalBar; }

  // After every change to the text, undo and redo included.
  std::function<void()> onEdited;
  std::function<void()> onSaveRequested;
  std::function<void()> onEscape;
  // An edit the buffer refused, in its words; nothing changed.
  std::function<void(const std::string& sentence)> onRefused;
  std::function<void()> onFrameNeeded;
  // The keyboard modifiers held now (KeyMod bits), for Shift with a click or the wheel; none when unset.
  std::function<std::uint32_t()> modifiers;

  TextInputState textInputState() const override;
  void textInputApplyEdit(const TextInputEdit& edit) override;
  void textInputResetPreedit() override;
  void textInputActivated(TextInputService& service) override;
  void textInputDeactivated(TextInputService& service) override;

protected:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;

private:
  enum class Granularity : std::uint8_t { Character, Word, Line };

  // What the strips are drawn with.
  struct StripKey {
    double scale = 1;
    float textSize = kDefaultTextSize;
    std::uint64_t palette = 0;
    bool operator==(const StripKey&) const = default;
  };

  // A selected stretch of one line in device pixels from the text's left edge; `right` is empty when the selection
  // runs on past the line's end.
  struct Band {
    std::size_t line = 0;
    double left = 0;
    std::optional<double> right;
  };

  struct Lines {
    std::size_t first = 0;
    std::size_t last = 0; // inclusive
  };

  // Where the view stands: the line at its top, in lines from the text's top, and whether the caret is in view.
  struct Place {
    int lineHeight = 0; // device pixels; 0 before the first strip drawer
    double scale = 1;
    double topLine = 0;
    bool caretShown = false;
  };

  // code_editor_view.cpp
  // Makes the strip drawer for the current scale, text size and palette; true when it was made anew.
  bool ensureStrip();
  luciole::TextLineStrip& strip();
  // Throws std::logic_error when the styles and the buffer disagree.
  luciole::formats::StyledLine styledLine(std::size_t index);
  // The line as drawn: with the composing text at the caret on the caret's line.
  luciole::formats::StyledLine displayLine(std::size_t index);
  std::size_t columnOf(luciole::editing::TextPosition position) const;
  std::size_t composingColumns() const;
  int margin() const;
  double deviceScale() const;
  // Logical pixels of the text with its margins, as far as its lines were measured.
  LayoutSize contentSize() const;
  Lines linesInView() const;
  void drawStrip(std::size_t line);
  void computeCaret();
  void computeBands();
  void reveal();
  Place place() const;
  // After lines changed height: brings the caret back into view when it was there, else the top line to the top.
  void keepPlace(const Place& before);
  void caretMoved();
  void applyEdit(const luciole::editing::EditResult& result);
  void applySpan(const luciole::editing::LineSpan& span);
  void restartBlink();
  void stopBlink();
  void needFrame();
  void layoutBars(Renderer& renderer, float width, float height);
  void placeOverlay();

  // code_editor_keys.cpp
  bool editKey(std::uint32_t sym, std::uint32_t utf32, bool ctrl, bool shift);
  bool moveKey(std::uint32_t sym, bool ctrl, bool shift);
  void moveVertically(std::ptrdiff_t lines, bool shift);
  void moveTo(luciole::editing::TextPosition position, bool shift);
  void copy(bool cut);
  void paste();
  std::uint64_t nowMs() const;

  // code_editor_pointer.cpp
  void wirePointer();
  luciole::editing::TextPosition positionAt(float x, float y);
  void press(const InputArea::PointerData& pointer);
  void drag(const InputArea::PointerData& pointer);
  bool wheel(const InputArea::PointerData& pointer);
  void extendTo(luciole::editing::TextPosition position);

  // code_editor_input_method.cpp
  void setComposing(std::string text, std::size_t cursor);
  void notifyInputMethod();

  luciole::editing::TextBuffer& m_buffer;
  luciole::editing::SourceStyles& m_styles;
  TextClipboard& m_clipboard;
  luciole::editing::IndentUnit m_indent;
  std::chrono::microseconds m_drawBudget;

  Box* m_background = nullptr;
  Node* m_layer = nullptr;
  Node* m_bandLayer = nullptr;
  Node* m_stripLayer = nullptr;
  Box* m_caret = nullptr;
  InputArea* m_area = nullptr;
  Scrollbar* m_verticalBar = nullptr;
  Scrollbar* m_horizontalBar = nullptr;
  std::vector<Box*> m_bandBoxes;

  std::unique_ptr<luciole::TextLineStrip> m_strip;
  std::optional<StripKey> m_stripKey; // what m_strip and the strips were made with
  std::unique_ptr<luciole::code_editor::LineStrips> m_lines;
  TextureManager* m_textures = nullptr;
  std::uint64_t m_drawn = 0;

  Signal<>::ScopedConnection m_paletteConn;
  std::uint64_t m_palette = 0;
  float m_textSize = kDefaultTextSize;
  double m_scale = 1;
  float m_viewportWidth = 0;
  float m_viewportHeight = 0;
  float m_scrollX = 0;
  float m_scrollY = 0;
  int m_lineHeight = 0; // device pixels; 0 before the first strip drawer
  int m_widest = 0;     // device pixels of the widest line measured since the strip drawer was made

  double m_caretX = 0;           // device pixels from the text's left edge
  std::optional<double> m_goalX; // where Up, Down and the page keys aim
  std::vector<Band> m_bands;
  bool m_bandsDirty = true;

  std::string m_composing;
  std::size_t m_composingCursor = 0; // bytes into m_composing
  TextInputService* m_textInput = nullptr;
  bool m_inputMethodEdit = false; // an input method's edit is being applied, so it is not told about it
  std::uint64_t m_focusLosses = 0;

  Timer m_blinkTimer;
  bool m_caretOn = false;

  Granularity m_granularity = Granularity::Character;
  luciole::editing::TextSelection m_pivot; // what the press selected, which a drag extends
  std::chrono::steady_clock::time_point m_lastPress;
  float m_lastPressX = 0;
  float m_lastPressY = 0;
  int m_clicks = 0;

  std::shared_ptr<int> m_alive = std::make_shared<int>(0); // answers that arrive later check it is still there
};
