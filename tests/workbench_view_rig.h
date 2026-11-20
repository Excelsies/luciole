#pragma once

#include "browser/lookahead_columns.h"
#include "browser/workbench_model.h"
#include "browser/workbench_services.h"
#include "browser/workbench_state.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "metrics_renderer.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/controls/label.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/quiet_column_view.h"
#include "ui/workbench/ribbon_view.h"
#include "ui/workbench/snapshot_places.h"
#include "ui/workbench/workbench_chrome.h"
#include "ui/workbench/workbench_columns.h"
#include "ui/workbench/workbench_view.h"
#include "workbench/scene_shapes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

// A Workbench view over real models on a fixture folder, a real input dispatcher and the shared text measurer.
namespace luciole::test::view {
  namespace fs = std::filesystem;
  using Clock = WorkbenchState::Clock;

  // base/home (the private home) and base/root:
  //   big/{b.bin 1000, deep/{d.bin 3000, inner/i.bin 2000}}, mid/{m.txt 400, sub/s.bin 800}, small/t.txt 100,
  //   note.txt 50. big is the heaviest folder, and big/deep, big/deep/inner and mid/sub are two levels down. The
  //   folder goes when the fixture does, and when a failed check ends the process before that.
  class Fixture {
  public:
    Fixture() {
      root = base / "root";
      write(root / "big" / "b.bin", 1000);
      write(root / "big" / "deep" / "d.bin", 3000);
      write(root / "big" / "deep" / "inner" / "i.bin", 2000);
      write(root / "mid" / "m.txt", 400);
      write(root / "mid" / "sub" / "s.bin", 800);
      write(root / "small" / "t.txt", 100);
      write(root / "note.txt", 50);
      fs::create_directories(base / "home");
      for (const char* variable : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_DATA_DIRS"}) {
        g_setenv(variable, (base / "home").c_str(), TRUE);
      }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    // A path below root; an absolute path as it is.
    Location at(const fs::path& path) const {
      return Location::fromPath(path.empty() ? root.native() : (root / path).native());
    }

    luciole::test::TempFolder tempFolder{"luciole-workbench-view"};
    fs::path base = tempFolder.path();
    fs::path root;

    static void write(const fs::path& path, std::size_t bytes) {
      fs::create_directories(path.parent_path());
      std::ofstream(path) << std::string(bytes, 'x');
    }
  };

  // Runs the default main context for at most 5 ms.
  class Pump {
  public:
    void once() {
      std::vector<pollfd> fds;
      const auto start = m_source.addPollFds(fds);
      const int requested = m_source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 5 : std::min(requested, 5)) >= 0);
      m_source.dispatch(fds, start);
    }

  private:
    GlibPollSource m_source;
  };

  struct Point {
    float x = 0;
    float y = 0;
  };

  // The middle of a node in scene coordinates.
  inline Point middle(const Node& node) {
    Point at;
    Node::absolutePosition(&node, at.x, at.y);
    return {.x = at.x + (node.width() / 2), .y = at.y + (node.height() / 2)};
  }

  inline bool shown(const Node* node) {
    for (; node != nullptr; node = node->parent()) {
      if (!node->visible()) {
        return false;
      }
    }
    return true;
  }

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

  template <typename T> std::vector<const T*> visible(const Node& node) {
    std::vector<const T*> out;
    collect(node, out);
    return out;
  }

  struct Rig {
    Fixture fixture;
    Pump pump;
    WorkbenchServices services;
    WorkbenchModel model{services.measureCache(), {.ancestorLevels = 1}};
    LookaheadColumns lookahead{{}, false, true};
    WorkbenchState state;
    FileIconCache icons;
    ::test::MetricsRenderer renderer;
    AnimationManager animations;
    Clock::time_point now = Clock::now();
    FolderTrail trail; // the folders open() entered, as the page records them
    ::Node root;
    WorkbenchView* view = nullptr;
    InputArea* list = nullptr;
    InputDispatcher dispatcher;
    float width = 1400;
    float height = 900;
    std::uint32_t held = 0; // the keyboard modifiers the view reads
    std::vector<Location> gone;
    std::vector<Location> selected;
    std::vector<Location> opened;
    std::vector<WorkbenchEntryRequest> requests; // every go, select and open, in order
    std::vector<WorkbenchContextRequest> menus;
    std::vector<WorkbenchMoreRequest> mores;
    std::vector<WorkbenchInstrument> instruments;
    std::vector<WorkbenchRibbon> ribbons;
    std::vector<WorkbenchGeometry> geometries;
    std::vector<std::optional<LookaheadPreview>> previews;
    std::vector<std::optional<Location>> listMarks;
    std::vector<InputArea*> focuses;

    Rig() {
      root.setSize(4000, 4000);
      root.setAnimationManager(&animations);
      auto made = std::make_unique<WorkbenchView>(icons, [this] { return held; }, [this] { return now; });
      view = static_cast<WorkbenchView*>(root.addChild(std::move(made)));
      auto area = std::make_unique<InputArea>();
      area->setFocusable(true);
      list = area.get();
      TEST_CHECK(view->setList(std::move(area), list) == nullptr);
      view->onGo = [this](const WorkbenchEntryRequest& asked) {
        gone.push_back(asked.location);
        requests.push_back(asked);
      };
      view->onSelect = [this](const WorkbenchEntryRequest& asked) {
        selected.push_back(asked.location);
        requests.push_back(asked);
      };
      view->onActivate = [this](const WorkbenchEntryRequest& asked) {
        opened.push_back(asked.location);
        requests.push_back(asked);
      };
      view->onContextMenu = [this](const WorkbenchContextRequest& request) { menus.push_back(request); };
      view->onShowMore = [this](const WorkbenchMoreRequest& request) { mores.push_back(request); };
      view->onInstrumentChosen = [this](WorkbenchInstrument kind) { instruments.push_back(kind); };
      view->onRibbonChosen = [this](WorkbenchRibbon kind) { ribbons.push_back(kind); };
      view->onGeometry = [this](const WorkbenchGeometry& geometry) { geometries.push_back(geometry); };
      view->onPreview = [this](const std::optional<LookaheadPreview>& preview) { previews.push_back(preview); };
      view->onListPointed = [this](const std::optional<Location>& at) { listMarks.push_back(at); };
      view->onFocus = [this](InputArea* area) { focuses.push_back(area); };
      view->setModel(&model);
      view->setLookahead(&lookahead);
      view->setState(&state);
      view->setTrail(&trail);
      dispatcher.setSceneRoot(&root);
      open({});
    }
    ~Rig() {
      if (view != nullptr) { // the sources go first, as the page lets go of them
        view->setModel(nullptr);
        view->setLookahead(nullptr);
        view->setState(nullptr);
      }
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // What the page does when its folder changes.
    void open(const fs::path& relative) {
      trail.visit(fixture.at(relative));
      model.setLocation(fixture.at(relative));
      view->setFolder(fixture.at(relative));
    }

    void frame() {
      view->update();
      view->arrange(renderer, {0, 0, width, height});
    }

    void resize(float w, float h) {
      width = w;
      height = h;
      frame();
      frame();
    }

    // Frames until `done`, letting workers publish and tweens run; fails after 20 seconds.
    void until(const std::function<bool()>& done) {
      const auto deadline = Clock::now() + std::chrono::seconds(20);
      frame();
      while (!done()) {
        TEST_CHECK(Clock::now() < deadline);
        pump.once();
        animations.tick(16);
        frame();
      }
    }

    // The folder is measured, column A listed, and both parts show the folder's latest snapshot at rest.
    bool loaded() {
      const auto snapshot = model.snapshot();
      if (!snapshot || model.measuring() || lookahead.count() == 0 || animations.hasActive()) {
        return false;
      }
      if (lookahead.model(0).snapshot().status != DirectoryStatus::Ready) {
        return false;
      }
      const auto shows = [&](const PartView& part) {
        return !part.shapeView().shapes().empty() && part.shownSnapshot().serial == snapshot->serial;
      };
      return shows(view->instrument()) && shows(view->ribbon());
    }

    void settle() {
      until([this] { return loaded(); });
    }

    // Ticks until every tween ends, at most two seconds.
    void finishTransitions() {
      const auto deadline = Clock::now() + std::chrono::seconds(2);
      while (animations.hasActive()) {
        TEST_CHECK(Clock::now() < deadline);
        animations.tick(16);
      }
      frame();
    }

    PlacedSnapshot placed() const { return placeSnapshot(model.snapshot()); }
    workbench::NodeId node(const fs::path& relative) const { return nodeOf(placed(), fixture.at(relative)); }

    void move(Point at) { dispatcher.pointerMotion(at.x, at.y, 1); }
    void click(Point at, std::uint32_t button = BTN_LEFT) {
      move(at);
      dispatcher.pointerButton(at.x, at.y, button, true, 2, 0, false);
      dispatcher.pointerButton(at.x, at.y, button, false, 3, 0, false);
    }
    void click(const Node& node) { click(middle(node)); }

    // A point inside the piece a part draws for `node`, in scene coordinates.
    std::optional<Point> piece(const PartView& part, workbench::NodeId node) const {
      for (const auto& shape : part.shapeView().shapes()) {
        if (shape.id.node == node
            && shape.hittable
            && workbench::sceneRole(shape.id.role) == workbench::SceneRole::Piece
            && shape.kind != workbench::ShapeKind::Sector
            && shape.kind != workbench::ShapeKind::Circle) {
          Point at;
          Node::absolutePosition(&part.shapeView(), at.x, at.y);
          return Point{
              .x = at.x + shape.rect.x + (shape.rect.width / 2), .y = at.y + shape.rect.y + (shape.rect.height / 2)
          };
        }
      }
      return std::nullopt;
    }

    const QuietRow* columnRow(std::size_t slot, const std::string& name) const {
      for (const auto* row : visible<QuietRow>(view->column(slot))) {
        if (row->nameLabel().text() == name) {
          return row;
        }
      }
      return nullptr;
    }

    const JumpRowTile* jumpRow(const std::string& path) const {
      for (const auto* row : visible<JumpRowTile>(view->jumpList())) {
        if (row->row().path == path) {
          return row;
        }
      }
      return nullptr;
    }
  };
} // namespace luciole::test::view
