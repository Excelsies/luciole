#pragma once

#include "formats/table_grid.h"
#include "render/core/renderer.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/table_view.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A table view alone in a scene with a real input dispatcher, and helpers to find what it shows.
namespace luciole::test::tableview {
  using formats::GridCellKind;
  using formats::TableGrid;

  inline constexpr float kWidth = 600;
  inline constexpr float kHeight = 400;
  // At the default text size of 12: rows of 22, padding of 6, columns of 42 to 264.
  inline constexpr float kRow = 22;
  inline constexpr float kPad = 6;

  // Text metrics without fonts, as tests/metrics_renderer.h gives them: each byte is `advance` font sizes wide and
  // every line one font size tall. A test widens the font and moves the metrics generation the way a font change does.
  class FontRenderer final : public Renderer {
  public:
    float advance = 0.5F;
    std::uint64_t generation = 1;

    TextMetrics measureText(
        std::string_view text, float size, const TextFont&, float width, int, TextAlign, TextEllipsize, bool
    ) override {
      const float natural = static_cast<float>(text.size()) * size * advance;
      return {
          .width = width > 0 ? std::min(width, natural) : natural, .bottom = size, .capHeight = size, .lineCount = 1
      };
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.bottom = size, .capHeight = size}; }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .bottom = size}; }
    void measureTextCursorStops(
        std::string_view, float size, const std::vector<std::size_t>& offsets, std::vector<float>& out, const TextFont&
    ) override {
      out.clear();
      for (const std::size_t offset : offsets) {
        out.push_back(static_cast<float>(offset) * size * advance);
      }
    }
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1; }
    std::uint64_t textMetricsGeneration() const noexcept override { return generation; }
  };

  inline double threadMs() {
    timespec now{};
    TEST_CHECK(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return (static_cast<double>(now.tv_sec) * 1000.0) + (static_cast<double>(now.tv_nsec) * 1.0E-6);
  }

  inline std::shared_ptr<const TableGrid> grid(
      std::size_t rows, std::size_t columns, const std::function<std::string(std::size_t, std::size_t)>& text,
      const std::function<void(TableGrid&)>& adjust = {}
  ) {
    formats::TableGridBuilder builder(rows, columns);
    for (std::size_t row = 0; row < rows; ++row) {
      for (std::size_t column = 0; column < columns; ++column) {
        TEST_CHECK(builder.set(row, column, GridCellKind::Text, text(row, column)));
      }
    }
    auto out = builder.finish();
    if (adjust) {
      adjust(out);
    }
    return std::make_shared<const TableGrid>(std::move(out));
  }

  inline bool shown(const Node& node) {
    for (const Node* at = &node; at != nullptr; at = at->parent()) {
      if (!at->visible()) {
        return false;
      }
    }
    return true;
  }

  inline void collect(const Node& node, std::vector<const Label*>& out) {
    if (!node.visible()) {
      return;
    }
    if (const auto* label = dynamic_cast<const Label*>(&node)) {
      out.push_back(label);
    }
    for (const auto& child : node.children()) {
      collect(*child, out);
    }
  }

  inline std::vector<const Label*> labels(const Node& root) {
    std::vector<const Label*> out;
    collect(root, out);
    return out;
  }

  inline const Label* labelWith(const Node& root, std::string_view text) {
    for (const auto* label : labels(root)) {
      if (label->text() == text) {
        return label;
      }
    }
    return nullptr;
  }

  inline std::size_t nodes(const Node& node) {
    std::size_t count = 1;
    for (const auto& child : node.children()) {
      count += nodes(*child);
    }
    return count;
  }

  struct Point {
    float x = 0;
    float y = 0;
  };

  inline Point center(const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return {.x = x + (node.width() / 2), .y = y + (node.height() / 2)};
  }

  // The view alone in a scene, laid out the way a page does it: update, then layout, until nothing waits.
  struct Rig {
    FontRenderer renderer;
    Node root;
    TableView* view = nullptr;
    InputDispatcher dispatcher;
    int frames = 0;
    std::uint32_t serial = 0;
    std::uint32_t clock = 0; // milliseconds of pointer time; each click comes a second after the last

    Rig() {
      root.setSize(kWidth, kHeight);
      view = static_cast<TableView*>(root.addChild(std::make_unique<TableView>()));
      view->onFrameNeeded = [this] { ++frames; };
      view->onFocus = [this](InputArea* area) { dispatcher.setFocus(area); };
      dispatcher.setSceneRoot(&root);
    }

    void settle() {
      for (int pass = 0; pass < 8; ++pass) {
        frames = 0;
        view->update();
        view->arrange(renderer, {.x = 0, .y = 0, .width = kWidth, .height = kHeight});
        if (frames == 0) {
          return;
        }
      }
      TEST_CHECK(frames == 0);
    }

    void show(std::shared_ptr<const TableGrid> table) {
      view->setTable(std::move(table));
      settle();
    }

    void click(Point at, std::uint32_t after = 1000) {
      clock += after;
      dispatcher.pointerEnter(at.x, at.y, ++serial);
      dispatcher.pointerButton(at.x, at.y, BTN_LEFT, true, ++serial, clock, false);
      dispatcher.pointerButton(at.x, at.y, BTN_LEFT, false, ++serial, clock, false);
      settle();
    }

    void doubleClick(Point at) {
      click(at);
      click(at, 150);
    }

    void hover(Point at) { dispatcher.pointerMotion(at.x, at.y, ++serial); }

    // A key the way a page delivers it: offered to the table first, then to the focused area.
    void key(std::uint32_t sym, std::uint32_t utf32 = 0, std::uint32_t modifiers = 0) {
      for (const bool pressed : {true, false}) {
        const InputArea::KeyData data{.sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = pressed};
        if (!view->offerKey(data)) {
          dispatcher.keyEvent(sym, utf32, modifiers, pressed);
        }
      }
      settle();
    }

    void wheel(std::uint32_t axis, double value) {
      const Point at = center(view->canvas());
      dispatcher.pointerMotion(at.x, at.y, ++serial);
      dispatcher.pointerAxis(at.x, at.y, axis, 0, value, 0, 0, 0);
      settle();
    }

    void press(std::uint32_t sym, std::uint32_t modifiers = 0) {
      dispatcher.setFocus(&view->canvas());
      dispatcher.keyEvent(sym, 0, modifiers, true);
      dispatcher.keyEvent(sym, 0, modifiers, false);
      settle();
    }
  };
} // namespace luciole::test::tableview
