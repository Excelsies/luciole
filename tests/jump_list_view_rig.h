#pragma once

#include "core/timer_manager.h"
#include "metrics_renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/tooltip_presenter.h"
#include "ui/style.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/list_rows.h"
#include "workbench/jump_rank.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The jump list view tests' fixture trees, rig and probes.
namespace luciole::test::jump_rig {
  using luciole::ActivationMode;
  using luciole::JumpListContent;
  using luciole::JumpListView;
  using luciole::JumpNotice;
  using luciole::JumpRowTile;
  using luciole::workbench::JumpList;
  using luciole::workbench::JumpRow;
  using luciole::workbench::LayoutSettings;
  using luciole::workbench::Tree;
  using Strings = std::vector<std::string>;

  // Ids: 0 r, 1 a, 2 b, 3 x (a/x, 3 opens), 4 y (a/y), 5 z (b/z), 6 deep (a/x/deep, 1 open), 7 f.txt.
  inline Tree fixtureTree() {
    return luciole::test::workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true, .bytes = 1000, .items = 2},
            {.parent = 0, .name = "a", .folder = true, .bytes = 700, .items = 2},
            {.parent = 0, .name = "b", .folder = true, .bytes = 300, .items = 1},
            {.parent = 1, .name = "x", .folder = true, .bytes = 400, .items = 1, .visits = 3},
            {.parent = 1, .name = "y", .folder = true, .bytes = 300},
            {.parent = 2, .name = "z", .folder = true, .bytes = 200},
            {.parent = 3, .name = "deep", .folder = true, .bytes = 100, .items = 1, .visits = 1},
            {.parent = 6, .name = "f.txt", .bytes = 100},
        },
        6000
    );
  }

  inline JumpList fixtureList() { return luciole::workbench::jumpAhead(fixtureTree(), LayoutSettings{}); }

  template <typename T> void collect(const Node& node, std::vector<const T*>& out) {
    if (!node.visible()) {
      return;
    }
    if (const auto* match = dynamic_cast<const T*>(&node)) {
      out.push_back(match);
    }
    for (const auto& child : node.children()) {
      collect(*child, out);
    }
  }

  inline float sceneY(const Node* node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(node, x, y);
    return y;
  }

  inline std::vector<const JumpRowTile*> rowsOf(const Node& view) {
    std::vector<const JumpRowTile*> rows;
    collect(view, rows);
    std::ranges::sort(rows, {}, [](const JumpRowTile* row) { return sceneY(row); });
    return rows;
  }

  inline const JumpRowTile* rowAt(const Node& view, const std::string& path) {
    for (const auto* row : rowsOf(view)) {
      if (row->row().path == path) {
        return row;
      }
    }
    TEST_CHECK(false);
    return nullptr;
  }

  struct Center {
    float x = 0;
    float y = 0;
  };

  inline Center centerOf(const Node* node) {
    TEST_CHECK(node != nullptr);
    Center center;
    Node::absolutePosition(node, center.x, center.y);
    center.x += node->width() / 2;
    center.y += node->height() / 2;
    return center;
  }

  struct Rig {
    ::test::MetricsRenderer renderer;
    Node root;
    JumpListView* view = nullptr;
    InputDispatcher dispatcher;
    std::uint32_t serial = 0;
    std::uint64_t contentSerial = 0;
    std::vector<std::optional<std::string>> hovers;
    Strings activations;
    Strings middles;

    Rig() {
      root.setSize(2000, 2000);
      view = static_cast<JumpListView*>(root.addChild(std::make_unique<JumpListView>()));
      view->onHover = [this](const JumpRow* row) {
        hovers.push_back(row != nullptr ? std::optional(row->path) : std::nullopt);
      };
      view->onActivate = [this](const JumpRow& row) { activations.push_back(row.path); };
      view->onMiddleClick = [this](const JumpRow& row) { middles.push_back(row.path); };
      dispatcher.setSceneRoot(&root);
      layout();
    }

    void layout() { view->arrange(renderer, {20, 30, 340, 300}); }
    void show(JumpListContent content) {
      view->setContent(std::move(content), ++contentSerial);
      layout();
    }
    void move(Center at) {
      dispatcher.pointerMotion(at.x, at.y, ++serial);
      layout();
    }
    void click(Center at, std::uint32_t button = BTN_LEFT) {
      dispatcher.pointerEnter(at.x, at.y, ++serial);
      dispatcher.pointerButton(at.x, at.y, button, true, ++serial, 1000, false);
      dispatcher.pointerButton(at.x, at.y, button, false, ++serial, 1050, false);
      layout();
    }
    void key(std::uint32_t sym) {
      dispatcher.keyEvent(sym, 0, 0, true);
      dispatcher.keyEvent(sym, 0, 0, false);
      layout();
    }
    Strings names() const {
      Strings result;
      for (const auto* row : rowsOf(*view)) {
        result.push_back(row->nameLabel().text());
      }
      return result;
    }
  };

  inline bool noticeShows(const Rig& rig, const std::string& text) {
    std::vector<const Label*> labels;
    collect(*rig.view, labels);
    return std::ranges::any_of(labels, [&](const Label* label) { return label->text() == text; });
  }

  inline JumpList longList(std::size_t count) {
    const auto source = fixtureList();
    JumpList list;
    for (std::size_t i = 0; i < count; ++i) {
      auto row = source.rows[i % source.rows.size()];
      row.id = static_cast<luciole::workbench::NodeId>(100 + i);
      row.path = std::format("p/f{:02}", i);
      list.rows.push_back(std::move(row));
    }
    return list;
  }
  inline bool tooltipShows(const TooltipPresenter& presenter, const std::string& text) {
    std::vector<const Label*> labels;
    collect(presenter, labels);
    return std::ranges::any_of(labels, [&](const Label* label) { return label->text() == text; });
  }

} // namespace luciole::test::jump_rig
