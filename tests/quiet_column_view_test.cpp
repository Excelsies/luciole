#include "browser/lookahead_columns.h"
#include "core/input/key_modifiers.h"
#include "core/timer_manager.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "metrics_renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/tooltip_presenter.h"
#include "ui/style.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/quiet_column_view.h"
#include "workbench/column_plan.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::DirectoryStatus;
  using luciole::EntryId;
  using luciole::Location;
  using luciole::LookaheadColumns;
  using luciole::QuietColumnContent;
  using luciole::QuietColumnView;
  using luciole::QuietNotice;
  using luciole::QuietRow;
  using luciole::QuietRowEvent;
  using luciole::WorkbenchEntry;

  constexpr std::size_t kBigFolder = 20'000;

  // root/a/{nested/, other/, b.txt, file.txt}, root/empty/, root/locked/ (unreadable) and `big` files in root/big/.
  // Icon lookups see an empty private theme.
  class Fixture {
  public:
    explicit Fixture(std::size_t big = 0) {
      fs::create_directories(path / "a" / "nested");
      fs::create_directories(path / "a" / "other");
      fs::create_directories(path / "empty");
      fs::create_directories(path / "locked" / "inside");
      fs::create_directories(path / "home");
      std::ofstream(path / "a" / "b.txt") << "b";
      std::ofstream(path / "a" / "file.txt") << "file";
      fs::permissions(path / "locked", fs::perms::none);
      fs::create_directories(path / "big");
      for (std::size_t i = 0; i < big; ++i) {
        std::ofstream(path / "big" / std::format("f{:05}", i));
      }
      for (const char* name : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_DATA_DIRS"}) {
        g_setenv(name, (path / "home").c_str(), TRUE);
      }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    WorkbenchEntry folder(const char* name) const {
      return {.location = Location::fromPath((path / name).native()), .folder = true};
    }

    luciole::test::TempFolder tempFolder{"luciole-quiet-column"};
    fs::path path = tempFolder.path();
  };

  // Runs the main context until column one has loaded, for at most 20 seconds.
  void settle(GlibPollSource& source, const LookaheadColumns& columns) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (columns.count() == 0 || columns.model(0).snapshot().status == DirectoryStatus::Loading) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      const auto start = source.addPollFds(fds);
      const int requested = source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
      source.dispatch(fds, start);
    }
  }

  // Column one of a LookaheadColumns over the fixture.
  struct Listed {
    Fixture fixture;
    GlibPollSource source;
    LookaheadColumns columns{{}, false, true};

    explicit Listed(const char* folder = "a", std::size_t big = 0) : fixture(big) {
      if (folder != nullptr) {
        follow(folder);
      }
    }
    void follow(const char* folder) {
      columns.follow({.target = fixture.folder(folder)});
      settle(source, columns);
    }
    QuietColumnContent content(std::uint64_t rowsSerial = 1) const {
      return {.listing = &columns.model(0), .folder = columns.location(0), .rowsSerial = rowsSerial};
    }
    EntryId id(const std::string& name) const {
      const auto& model = columns.model(0);
      const auto found =
          std::ranges::find_if(model.rows(), [&](EntryId row) { return model.entry(row)->name == name; });
      TEST_CHECK(found != model.rows().end());
      return *found;
    }
  };

  template <typename T> void collect(const Node& node, std::vector<const T*>& out, bool visibleOnly) {
    if (visibleOnly && !node.visible()) {
      return;
    }
    if (const auto* match = dynamic_cast<const T*>(&node)) {
      out.push_back(match);
    }
    for (const auto& child : node.children()) {
      collect(*child, out, visibleOnly);
    }
  }

  // Shown rows, top first.
  std::vector<const QuietRow*> rowsOf(const Node& view) {
    std::vector<const QuietRow*> rows;
    collect(view, rows, true);
    std::ranges::sort(rows, {}, [](const QuietRow* row) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(row, x, y);
      return y;
    });
    return rows;
  }

  const QuietRow* rowNamed(const Node& view, const std::string& name) {
    const auto rows = rowsOf(view);
    const auto found = std::ranges::find(rows, name, [](const QuietRow* row) { return row->nameLabel().text(); });
    TEST_CHECK(found != rows.end());
    return *found;
  }

  // Every shown label with text.
  std::vector<const Label*> textLabels(const Node& view) {
    std::vector<const Label*> labels;
    collect(view, labels, true);
    std::erase_if(labels, [](const Label* label) { return label->text().empty(); });
    return labels;
  }

  bool hasText(const Node& view, const std::string& text) {
    return std::ranges::any_of(textLabels(view), [&](const Label* label) { return label->text() == text; });
  }

  struct Center {
    float x = 0;
    float y = 0;
  };

  Center centerOf(const Node* node) {
    TEST_CHECK(node != nullptr);
    Center center;
    Node::absolutePosition(node, center.x, center.y);
    center.x += node->width() / 2;
    center.y += node->height() / 2;
    return center;
  }

  // The shown row whose bounds hold `at`; none over empty space.
  const QuietRow* rowUnder(const Node& view, Center at) {
    for (const auto* row : rowsOf(view)) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(row, x, y);
      if (at.x >= x && at.x < x + row->width() && at.y >= y && at.y < y + row->height()) {
        return row;
      }
    }
    return nullptr;
  }

  std::vector<std::string> hoveredNames(const Node& view) {
    std::vector<std::string> names;
    for (const auto* row : rowsOf(view)) {
      if (row->marks().hovered) {
        names.push_back(row->nameLabel().text());
      }
    }
    return names;
  }

  std::string tooltipOf(const Button* button) {
    TEST_CHECK(button != nullptr);
    const auto content = button->inputArea()->tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  bool same(const QuietRowEvent& left, const QuietRowEvent& right) {
    return left.entry == right.entry && left.button == right.button && left.modifiers == right.modifiers;
  }

  struct Moved {
    std::optional<EntryId> row;
    Center at;
  };

  // A column view in a scene, with every callback recorded.
  struct Rig {
    luciole::FileIconCache icons;
    test::MetricsRenderer renderer;
    Node root;
    QuietColumnView* view = nullptr;
    InputDispatcher dispatcher;
    std::uint32_t modifiers = 0;
    std::uint32_t serial = 0;
    std::uint64_t contentSerial = 0;
    float width = 260;
    float height = 400;
    std::map<EntryId, double> shares;
    std::map<EntryId, int> shareReads;
    std::vector<Moved> moves;
    int leaves = 0;
    std::vector<QuietRowEvent> clicks;
    std::vector<QuietRowEvent> activations;
    int pins = 0;
    std::vector<EntryId> contexts;

    explicit Rig(std::size_t column = 0) {
      root.setSize(2000, 2000);
      auto view = std::make_unique<QuietColumnView>(icons, column, [this] { return modifiers; });
      this->view = static_cast<QuietColumnView*>(root.addChild(std::move(view)));
      this->view->setListTextSize(14);
      this->view->share = [this](EntryId id) -> std::optional<double> {
        ++shareReads[id];
        const auto found = shares.find(id);
        return found == shares.end() ? std::nullopt : std::optional(found->second);
      };
      this->view->onPointerMoved = [this](std::optional<EntryId> row, float x, float y) {
        moves.push_back({.row = row, .at = {.x = x, .y = y}});
      };
      this->view->onPointerLeft = [this] { ++leaves; };
      this->view->onRowClick = [this](const QuietRowEvent& event) { clicks.push_back(event); };
      this->view->onRowActivate = [this](const QuietRowEvent& event) { activations.push_back(event); };
      this->view->onPinControl = [this] { ++pins; };
      this->view->onContextRequest = [this](EntryId entry, float, float) { contexts.push_back(entry); };
      dispatcher.setSceneRoot(&root);
      layout();
    }

    void layout() { view->arrange(renderer, {10, 20, width, height}); }
    void show(QuietColumnContent content) {
      view->setContent(std::move(content), ++contentSerial);
      layout();
    }
    void press(Center at, std::uint32_t button = BTN_LEFT) {
      dispatcher.pointerEnter(at.x, at.y, ++serial);
      dispatcher.pointerButton(at.x, at.y, button, true, ++serial, 1000, false);
    }
    void release(Center at, std::uint32_t button = BTN_LEFT) {
      dispatcher.pointerButton(at.x, at.y, button, false, ++serial, 1050, false);
      layout();
    }
    void click(Center at) {
      press(at);
      release(at);
    }
    void move(Center at) { dispatcher.pointerMotion(at.x, at.y, ++serial); }
    // One wheel notch down at `at`.
    void wheel(Center at) {
      TEST_CHECK(dispatcher.pointerAxis(at.x, at.y, 0, 0, 20, 1, 120, 0));
      layout();
    }
    std::map<EntryId, int> takeReads() { return std::exchange(shareReads, {}); }
    std::string firstShown() const {
      const auto rows = rowsOf(*view);
      return rows.empty() ? std::string{} : rows.front()->nameLabel().text();
    }
  };

  void testTextSizeRule() {
    TEST_NEAR(luciole::quietTextSize(14, 0), 13, 1e-6);
    TEST_NEAR(luciole::quietTextSize(14, 1), 12.5, 1e-6);
    TEST_NEAR(luciole::quietTextSize(14, 2), 12, 1e-6);
    TEST_NEAR(luciole::quietTextSize(14, 7), 12, 1e-6); // past the third column, the third's size
    TEST_NEAR(luciole::quietTextSize(11, 0), 10, 1e-6); // the floor of 11 would reach the list's size
    TEST_NEAR(luciole::quietTextSize(std::numeric_limits<float>::quiet_NaN(), 0), 13, 1e-6);
    TEST_NEAR(luciole::quietTextSize(std::numeric_limits<float>::infinity(), 1), 12.5, 1e-6);
    TEST_NEAR(luciole::quietTextSize(0, 0), 5, 1e-6);
    TEST_NEAR(luciole::quietTextSize(-3, 2), 5, 1e-6);
    TEST_NEAR(luciole::quietTextSize(500, 0), 95, 1e-6);
  }

  void testRowsShowTheListingInOrder() {
    Listed listed;
    Rig rig;
    rig.show(listed.content());
    TEST_CHECK(rig.view->notice() == QuietNotice::None);
    std::vector<std::string> names;
    for (const auto* row : rowsOf(*rig.view)) {
      names.push_back(row->nameLabel().text());
      TEST_CHECK(row->entry() == listed.id(names.back()));
      TEST_NEAR(row->height(), luciole::workbench::kColumnRowHeights[0], 1e-3);
    }
    TEST_CHECK((names == std::vector<std::string>{"nested", "other", "b.txt", "file.txt"}));
    TEST_CHECK(hasText(*rig.view, "a"));
    TEST_CHECK(hasText(*rig.view, "4"));
  }

  void testTextStaysBelowTheListAtEveryWidth() {
    Listed listed;
    for (std::size_t column = 0; column < 3; ++column) {
      Rig rig(column);
      rig.shares[listed.id("nested")] = 0.5;
      rig.show(listed.content());
      for (const float list : {11.0F, 14.0F, 20.0F}) {
        rig.view->setListTextSize(list);
        TEST_CHECK(rig.view->textSize() < list);
        // The narrowest to the widest column the contract allows, 26 widths.
        for (double width = luciole::workbench::kColumnMin; width <= luciole::workbench::kColumnMax; width += 20) {
          rig.width = static_cast<float>(width);
          rig.layout();
          const auto labels = textLabels(*rig.view);
          TEST_CHECK(labels.size() >= 6); // title, count and four names
          for (const auto* label : labels) {
            TEST_CHECK(label->fontSize() > 0 && label->fontSize() < list);
          }
          for (const auto* row : rowsOf(*rig.view)) {
            TEST_NEAR(row->width(), rig.view->width() - 16, 1.0);
          }
        }
      }
    }
  }

  void testPointerReportsTheRow() {
    Listed listed;
    Rig rig;
    rig.show(listed.content());
    const auto other = centerOf(rowNamed(*rig.view, "other"));
    rig.move(other);
    TEST_CHECK(!rig.moves.empty() && rig.moves.back().row == listed.id("other"));
    TEST_NEAR(rig.moves.back().at.x, other.x, 1e-3);
    TEST_NEAR(rig.moves.back().at.y, other.y, 1e-3);
    rig.layout();
    TEST_CHECK(rowNamed(*rig.view, "other")->marks().hovered);
    TEST_CHECK(!rowNamed(*rig.view, "nested")->marks().hovered);
    // Small travel inside one row still reports, so the owner can measure the rest.
    const auto before = rig.moves.size();
    rig.move({.x = other.x + 3, .y = other.y + 1});
    TEST_CHECK(rig.moves.size() == before + 1);
    TEST_CHECK(rig.moves.back().row == listed.id("other"));
    const auto file = centerOf(rowNamed(*rig.view, "file.txt"));
    rig.move(file);
    TEST_CHECK(rig.moves.back().row == listed.id("file.txt"));
    rig.layout();
    TEST_CHECK(!rowNamed(*rig.view, "other")->marks().hovered);
    TEST_CHECK(rowNamed(*rig.view, "file.txt")->marks().hovered);
    // Empty space below the rows is no row.
    rig.move({.x = file.x, .y = 380});
    TEST_CHECK(rig.moves.back().row == std::nullopt && rig.leaves == 0);
    rig.move({.x = 1500, .y = 1500});
    TEST_CHECK(rig.leaves == 1);
    rig.layout();
    TEST_CHECK(!rowNamed(*rig.view, "file.txt")->marks().hovered);
  }

  void testClicksActivationPinAndContext() {
    Listed listed;
    Rig rig;
    rig.show(listed.content());
    rig.modifiers = KeyMod::Ctrl;
    const auto nested = centerOf(rowNamed(*rig.view, "nested"));
    rig.click(nested);
    TEST_CHECK(rig.clicks.size() == 1 && rig.activations.empty());
    TEST_CHECK(same(rig.clicks[0], {.entry = listed.id("nested"), .button = BTN_LEFT, .modifiers = KeyMod::Ctrl}));
    rig.modifiers = 0;
    rig.click(nested);
    TEST_CHECK(rig.activations.size() == 1);
    TEST_CHECK(same(rig.activations[0], {.entry = listed.id("nested"), .button = BTN_LEFT, .modifiers = 0}));
    // The second click reports the activation only; a click on another row is a new first click.
    rig.click(centerOf(rowNamed(*rig.view, "b.txt")));
    TEST_CHECK(rig.clicks.size() == 2 && rig.clicks[1].entry == listed.id("b.txt") && rig.activations.size() == 1);
    // A press released over another row is no click.
    rig.press(centerOf(rowNamed(*rig.view, "other")));
    rig.release(centerOf(rowNamed(*rig.view, "file.txt")));
    TEST_CHECK(rig.clicks.size() == 2);
    const auto file = centerOf(rowNamed(*rig.view, "file.txt"));
    rig.press(file, BTN_RIGHT);
    rig.release(file, BTN_RIGHT);
    TEST_CHECK((rig.contexts == std::vector<EntryId>{listed.id("file.txt")}));
    TEST_CHECK(rig.clicks.size() == 2);
    std::vector<const Button*> buttons;
    collect(*rig.view, buttons, true);
    TEST_CHECK(buttons.size() == 1);
    rig.click(centerOf(buttons[0]));
    TEST_CHECK(rig.pins == 1);
    TEST_CHECK(rig.clicks.size() == 2);
    // The pin control shows whether the column is pinned.
    TEST_CHECK(tooltipOf(buttons[0]) == "Pin this column");
    auto pinned = listed.content();
    pinned.columnPinned = true;
    rig.show(pinned);
    TEST_CHECK(tooltipOf(buttons[0]) == "Unpin this column");
    pinned.columnPinned = false;
    rig.show(pinned);
    TEST_CHECK(tooltipOf(buttons[0]) == "Pin this column");
  }

  // Entry ids start again in every listing, so a click in one folder never pairs with a click in the next.
  void testNewListingForgetsClicksAndHover() {
    const Listed first;
    const Listed second("big", 10);
    Rig rig;
    rig.show(first.content());
    const auto nested = first.id("nested");
    const auto at = centerOf(rowNamed(*rig.view, "nested"));
    rig.click(at);
    TEST_CHECK(rig.clicks.size() == 1);
    rig.show(second.content());
    rig.layout();
    // The row now under the pointer is the hovered one, not the row that shares the old row's id.
    const auto* under = rowUnder(*rig.view, at);
    TEST_CHECK(under != nullptr && under->entry() != nested);
    TEST_CHECK((hoveredNames(*rig.view) == std::vector<std::string>{under->nameLabel().text()}));
    TEST_CHECK(!rig.moves.empty() && rig.moves.back().row == under->entry());
    const auto shown = rowsOf(*rig.view);
    const auto twin = std::ranges::find(shown, nested, &QuietRow::entry);
    TEST_CHECK(twin != shown.end());
    rig.click(centerOf(*twin));
    TEST_CHECK(rig.activations.empty());
    TEST_CHECK(rig.clicks.size() == 2 && rig.clicks[1].entry == nested);
  }

  // A press held while the listing changes is no click, even when released over the row with the pressed id.
  void testNewListingDropsAHeldPress() {
    const Listed first;
    const Listed second("big", 10);
    Rig rig;
    rig.show(first.content());
    const auto nested = first.id("nested");
    rig.press(centerOf(rowNamed(*rig.view, "nested")));
    rig.show(second.content());
    const auto shown = rowsOf(*rig.view);
    const auto twin = std::ranges::find(shown, nested, &QuietRow::entry);
    TEST_CHECK(twin != shown.end());
    rig.release(centerOf(*twin));
    TEST_CHECK(rig.clicks.empty() && rig.activations.empty());
  }

  void testWheelMovesTheHover() {
    const Listed listed("big", 300);
    Rig rig;
    rig.show(listed.content());
    const auto at = centerOf(rowNamed(*rig.view, "f00005"));
    rig.move(at);
    rig.layout();
    TEST_CHECK((hoveredNames(*rig.view) == std::vector<std::string>{"f00005"}));
    for (int notch = 0; notch < 4; ++notch) {
      rig.wheel(at);
    }
    rig.layout();
    TEST_CHECK(rig.firstShown() != "f00000");
    const auto* under = rowUnder(*rig.view, at);
    TEST_CHECK(under != nullptr && under->nameLabel().text() != "f00005");
    TEST_CHECK((hoveredNames(*rig.view) == std::vector<std::string>{under->nameLabel().text()}));
    TEST_CHECK(rig.moves.back().row == under->entry());
    TEST_NEAR(rig.moves.back().at.y, at.y, 1e-3);
  }

  // Rests the pointer at `at` and runs the window's tooltip presenter until it shows; false when nothing shows
  // within five seconds.
  bool restTooltip(Rig& rig, TooltipPresenter& presenter, Center at) {
    rig.move(at);
    rig.layout();
    presenter.resume();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) { // bounded by the deadline
      TimerManager::instance().tick();
      presenter.sync(rig.renderer, rig.root, at.x, at.y, {rig.root.width(), rig.root.height()});
      if (presenter.visible()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  // A row's tooltip names it and sits by that row, also far down a scrolled column, not by the header.
  void testTooltipSitsByItsRow() {
    const Listed listed("big", 300);
    Rig rig;
    rig.show(listed.content());
    const auto top = centerOf(rowNamed(*rig.view, "f00003"));
    for (int notch = 0; notch < 4; ++notch) {
      rig.wheel(top);
    }
    TEST_CHECK(rig.firstShown() != "f00000");
    const auto* row = rowUnder(*rig.view, {.x = top.x, .y = 20 + rig.height - 40});
    TEST_CHECK(row != nullptr);
    const std::string name = row->nameLabel().text();
    float rowX = 0;
    float rowY = 0;
    Node::absolutePosition(row, rowX, rowY);
    const float rowBottom = rowY + row->height();
    TooltipPresenter presenter([] {});
    TEST_CHECK(restTooltip(rig, presenter, centerOf(row)));
    TEST_CHECK(hasText(presenter, name));
    const bool below = presenter.y() >= rowBottom && presenter.y() <= rowBottom + Style::spaceSm;
    const bool above =
        presenter.y() + presenter.height() <= rowY && presenter.y() + presenter.height() >= rowY - Style::spaceMd;
    TEST_CHECK(below || above);
    presenter.dismiss();
  }

  void testNewListingStartsAtTheTop() {
    const Listed first("big", 300);
    const Listed second("big", 300);
    Rig rig;
    rig.show(first.content());
    const auto at = centerOf(rowNamed(*rig.view, "f00003"));
    for (int notch = 0; notch < 4; ++notch) {
      rig.wheel(at);
    }
    TEST_CHECK(rig.firstShown() != "f00000");
    // New rows of the same listing keep the scroll.
    const auto scrolled = rig.firstShown();
    rig.show(first.content(2));
    TEST_CHECK(rig.firstShown() == scrolled);
    rig.show(second.content());
    TEST_CHECK(rig.firstShown() == "f00000");
  }

  // Keys stay with whatever had focus: a click on a row takes none.
  void testClickLeavesFocusAlone() {
    Listed listed;
    Rig rig;
    auto* list = static_cast<InputArea*>(rig.root.addChild(std::make_unique<InputArea>()));
    list->setFocusable(true);
    list->setPosition(1500, 1500);
    list->setFrameSize(100, 100);
    rig.show(listed.content());
    rig.dispatcher.setFocus(list);
    rig.click(centerOf(rowNamed(*rig.view, "b.txt")));
    TEST_CHECK(rig.clicks.size() == 1);
    TEST_CHECK(rig.dispatcher.focusedArea() == list);
  }

  // A size that is not finite reads as the body size, so it binds nothing again when the text size stays.
  void testListTextSizeThatIsNotFinite() {
    Listed listed;
    Rig rig;
    rig.show(listed.content());
    TEST_NEAR(rig.view->textSize(), 13, 1e-6);
    (void)rig.takeReads();
    for (int call = 0; call < 2; ++call) {
      rig.view->setListTextSize(std::numeric_limits<float>::quiet_NaN());
      rig.layout();
      TEST_CHECK(rig.takeReads().empty());
      TEST_NEAR(rig.view->textSize(), 13, 1e-6);
    }
    rig.view->setListTextSize(20);
    rig.layout();
    TEST_CHECK(rig.takeReads().size() == 4);
  }

  void testMarksRebindOnlyTheirRows() {
    Listed listed;
    Rig rig;
    const auto nested = listed.id("nested");
    const auto other = listed.id("other");
    const auto file = listed.id("file.txt");
    auto content = listed.content();
    rig.show(content);
    TEST_CHECK(rig.takeReads().size() == 4);
    content.followed = other;
    rig.show(content);
    TEST_CHECK((rig.takeReads() == std::map<EntryId, int>{{other, 1}}));
    TEST_CHECK(rowNamed(*rig.view, "other")->marks().followed);
    TEST_CHECK(!rowNamed(*rig.view, "nested")->marks().followed);
    content.pinned = file;
    rig.show(content);
    TEST_CHECK((rig.takeReads() == std::map<EntryId, int>{{file, 1}}));
    TEST_CHECK(rowNamed(*rig.view, "file.txt")->marks().pinned);
    TEST_CHECK(!rowNamed(*rig.view, "other")->marks().pinned);
    content.linked = nested;
    rig.show(content);
    TEST_CHECK((rig.takeReads() == std::map<EntryId, int>{{nested, 1}}));
    TEST_CHECK(rowNamed(*rig.view, "nested")->marks().linked);
    TEST_CHECK(rowNamed(*rig.view, "b.txt")->marks() == luciole::RowMarks{});
    // Moving a mark binds the row it left and the row it reached.
    content.followed = nested;
    rig.show(content);
    TEST_CHECK((rig.takeReads() == std::map<EntryId, int>{{nested, 1}, {other, 1}}));
    TEST_CHECK(!rowNamed(*rig.view, "other")->marks().followed);
    // The same serial changes nothing; a new rowsSerial binds every row.
    content.linked = other;
    rig.view->setContent(content, rig.contentSerial);
    rig.layout();
    TEST_CHECK(rig.takeReads().empty());
    TEST_CHECK(rowNamed(*rig.view, "nested")->marks().linked);
    content.rowsSerial = 2;
    rig.show(content);
    TEST_CHECK(rig.takeReads().size() == 4);
    TEST_CHECK(rowNamed(*rig.view, "other")->marks().linked);
  }

  void testShareBar() {
    Listed listed;
    Rig rig(1);
    rig.shares[listed.id("nested")] = 0.5;
    rig.shares[listed.id("other")] = 3;
    rig.shares[listed.id("b.txt")] = std::numeric_limits<double>::quiet_NaN();
    rig.shares[listed.id("file.txt")] = 0;
    rig.show(listed.content());
    TEST_CHECK(rowNamed(*rig.view, "nested")->share() == 0.5F);
    TEST_CHECK(rowNamed(*rig.view, "other")->share() == 1.0F); // clamped
    TEST_CHECK(!rowNamed(*rig.view, "b.txt")->share().has_value());
    TEST_CHECK(rowNamed(*rig.view, "file.txt")->share() == 0.0F);
    rig.shares[listed.id("nested")] = 0.25;
    rig.shares.erase(listed.id("other"));
    rig.show(listed.content(2));
    TEST_CHECK(rowNamed(*rig.view, "nested")->share() == 0.25F);
    TEST_CHECK(!rowNamed(*rig.view, "other")->share().has_value());
  }

  void checkNotice(const Rig& rig, QuietNotice notice, const std::string& text) {
    TEST_CHECK(rig.view->notice() == notice);
    TEST_CHECK(rowsOf(*rig.view).empty());
    TEST_CHECK(hasText(*rig.view, text));
  }

  void testNotices() {
    Listed listed(nullptr);
    Rig rig;
    listed.columns.follow({.target = listed.fixture.folder("a")});
    rig.show(listed.content(1));
    checkNotice(rig, QuietNotice::Loading, "Loading…");
    settle(listed.source, listed.columns);
    rig.show(listed.content(2));
    TEST_CHECK(rig.view->notice() == QuietNotice::None);
    TEST_CHECK(rowsOf(*rig.view).size() == 4);
    listed.follow("empty");
    rig.show(listed.content(3));
    checkNotice(rig, QuietNotice::Empty, "This folder is empty");
    listed.follow("locked");
    rig.show(listed.content(4));
    checkNotice(rig, QuietNotice::Unreadable, "This folder could not be read");
    listed.follow("missing");
    rig.show(listed.content(5));
    checkNotice(rig, QuietNotice::Gone, "This folder no longer exists");
    for (const auto* label : textLabels(*rig.view)) {
      TEST_CHECK(label->fontSize() < 14);
    }
    rig.show({});
    TEST_CHECK(rig.view->notice() == QuietNotice::None && rowsOf(*rig.view).empty());
  }

  // A column with nothing to show draws no header, no pin and no outline; a column that says why has the sentence
  // alone.
  void testNothingToShow() {
    Rig rig;
    rig.show({});
    std::vector<const Button*> buttons;
    collect(*rig.view, buttons, true);
    TEST_CHECK(rig.view->blank() && rig.view->notice() == QuietNotice::None);
    TEST_CHECK(buttons.empty() && textLabels(*rig.view).empty() && rowsOf(*rig.view).empty());
    rig.show({.noFolders = true});
    buttons.clear();
    collect(*rig.view, buttons, true);
    TEST_CHECK(!rig.view->blank() && rig.view->notice() == QuietNotice::NoFolders);
    TEST_CHECK(buttons.empty() && textLabels(*rig.view).size() == 1 && hasText(*rig.view, "No folders ahead"));
    const Listed listed;
    rig.show(listed.content(1));
    buttons.clear();
    collect(*rig.view, buttons, true);
    TEST_CHECK(!rig.view->blank() && !buttons.empty() && rowsOf(*rig.view).size() == 4);
    rig.show({});
    TEST_CHECK(rig.view->blank() && rowsOf(*rig.view).empty());
  }

  void testBigListingCreatesAScreenful() {
    const Listed listed("big", kBigFolder);
    TEST_CHECK(listed.columns.model(0).rows().size() == kBigFolder);
    Rig rig;
    rig.height = 600;
    rig.show(listed.content());
    const auto shown = rowsOf(*rig.view);
    TEST_CHECK(shown.size() >= 20);
    TEST_CHECK(shown.front()->nameLabel().text() == "f00000");
    std::vector<const QuietRow*> made;
    collect(*rig.view, made, false);
    // 600 px minus the header holds 22 rows of 26 px; the grid adds at most two rows of overscan on each side.
    TEST_CHECK(made.size() <= 30);
    TEST_CHECK(std::ranges::fold_left(rig.shareReads | std::views::values, 0, std::plus{}) <= 30);
    TEST_CHECK(hasText(*rig.view, "20,000"));
    TEST_CHECK(shown.back()->nameLabel().text() < "f00030");
  }
} // namespace

int main() {
  testTextSizeRule();
  testRowsShowTheListingInOrder();
  testTextStaysBelowTheListAtEveryWidth();
  testPointerReportsTheRow();
  testClicksActivationPinAndContext();
  testNewListingForgetsClicksAndHover();
  testNewListingDropsAHeldPress();
  testWheelMovesTheHover();
  testTooltipSitsByItsRow();
  testNewListingStartsAtTheTop();
  testClickLeavesFocusAlone();
  testListTextSizeThatIsNotFinite();
  testMarksRebindOnlyTheirRows();
  testShareBar();
  testNotices();
  testNothingToShow();
  testBigListingCreatesAScreenful();
  std::println("quiet column view checks passed");
}
