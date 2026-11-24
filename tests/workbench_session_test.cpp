#include "app/page/workbench_session.h"
#include "browser/workbench_services.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "metrics_renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/file_view.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_divider.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench/pane_layout.h"
#include "workbench_session_rig.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <ios>
#include <memory>
#include <poll.h>
#include <print>
#include <string>
#include <system_error>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::LookaheadPreview;
  using luciole::WorkbenchEntry;
  using luciole::WorkbenchSession;

  constexpr std::size_t kCycles = 20;

  using luciole::test::Fixture;
  using luciole::test::Rig;

  std::size_t views(const Node& node) {
    std::size_t count = dynamic_cast<const luciole::WorkbenchView*>(&node) != nullptr ? 1 : 0;
    for (const auto& child : node.children()) {
      count += views(*child);
    }
    return count;
  }

  bool inside(const Node& node, const Node& ancestor) {
    for (const Node* at = node.parent(); at != nullptr; at = at->parent()) {
      if (at == &ancestor) {
        return true;
      }
    }
    return false;
  }

  void paneMovesIntoTheViewAndBack() {
    Rig rig;
    auto session = rig.mount();
    TEST_CHECK(rig.parent.children().size() == 3);
    TEST_CHECK(rig.parent.children()[1].get() == &session->view());
    TEST_CHECK(session->view().center().list() == rig.pane);
    TEST_CHECK(inside(*rig.focus, session->view()));
    TEST_NEAR(session->view().flexGrow(), 1.0F, 0.0F);
    TEST_CHECK(views(rig.parent) == 1);
    session.reset();
    TEST_CHECK(rig.parent.children().size() == 3);
    TEST_CHECK(rig.parent.children()[1].get() == rig.pane);
    TEST_CHECK(rig.focus->parent() == rig.pane);
    TEST_CHECK(views(rig.parent) == 0);
  }

  void feedReadsTheBrowser() {
    Rig rig;
    auto feed = luciole::workbenchFeed(rig.browser);
    TEST_CHECK(feed.folder == rig.fixture.at(""));
    TEST_CHECK(!feed.cursor);
    TEST_CHECK(!feed.showHidden);
    rig.select("big");
    feed = luciole::workbenchFeed(rig.browser);
    TEST_CHECK((feed.cursor == WorkbenchEntry{.location = rig.fixture.at("big"), .folder = true}));
    rig.select("note.txt");
    feed = luciole::workbenchFeed(rig.browser);
    TEST_CHECK((feed.cursor == WorkbenchEntry{.location = rig.fixture.at("note.txt"), .folder = false}));
    rig.browser.toggleHidden();
    TEST_CHECK(luciole::workbenchFeed(rig.browser).showHidden);
  }

  void cursorSteersTheModelsNotTheBrowser() {
    Rig rig;
    auto session = rig.mount();
    rig.select("big");
    rig.frame(*session);
    TEST_CHECK((session->state().target() == WorkbenchEntry{.location = rig.fixture.at("big"), .folder = true}));
    TEST_CHECK(session->lookahead().count() == 1);
    TEST_CHECK(session->lookahead().location(0) == rig.fixture.at("big"));
    rig.until([&] {
      rig.frame(*session);
      const auto snapshot = session->model().snapshot();
      return snapshot && snapshot->location == rig.fixture.at("");
    });
    TEST_CHECK(rig.browser.model().snapshot().location == rig.fixture.at(""));
    TEST_CHECK(rig.browser.model().selectedIds().size() == 1);
    TEST_CHECK(!rig.browser.canBack());
  }

  void previewsFindTheirEntries() {
    Rig rig;
    auto session = rig.mount();
    rig.select("note.txt");
    rig.frame(*session);
    const auto preview = session->lookahead().preview();
    TEST_CHECK((preview == LookaheadPreview{.file = rig.fixture.at("note.txt"), .slot = 0}));
    const auto listed = luciole::previewedEntry(session->lookahead(), rig.browser.model(), *preview);
    TEST_CHECK(listed && listed->entry.name == "note.txt");
    TEST_CHECK(listed->typeLabel == rig.browser.model().typeLabel(listed->entry.id));
    TEST_CHECK(luciole::rowOf(rig.browser.model(), rig.fixture.at("note.txt")) == listed->entry.id);
    TEST_CHECK(!luciole::rowOf(rig.browser.model(), rig.fixture.at("big/b.bin")));
    TEST_CHECK(!luciole::rowOf(rig.browser.model(), rig.fixture.at("missing.txt")));

    rig.select("big");
    rig.frame(*session);
    rig.until([&] {
      return session->lookahead().count() == 1
          && session->lookahead().model(0).snapshot().status == luciole::DirectoryStatus::Ready;
    });
    const auto& lookahead = session->lookahead();
    const auto& list = rig.browser.model();
    const auto ahead = luciole::previewedEntry(lookahead, list, {.file = rig.fixture.at("big/b.bin"), .slot = 1});
    TEST_CHECK(ahead && ahead->entry.name == "b.bin");
    TEST_CHECK(!luciole::previewedEntry(lookahead, list, {.file = rig.fixture.at("big/none.bin"), .slot = 1}));
    TEST_CHECK(!luciole::previewedEntry(lookahead, list, {.file = rig.fixture.at("note.txt"), .slot = 1}));
    TEST_CHECK(!luciole::previewedEntry(lookahead, list, {.file = rig.fixture.at("big/b.bin"), .slot = 2}));
    TEST_CHECK(!luciole::previewedEntry(lookahead, list, {.file = rig.fixture.at("big/b.bin"), .slot = 0}));
  }

  void expandedInstrumentHidesThePane() {
    Rig rig;
    ::test::MetricsRenderer renderer;
    auto session = rig.mount();
    rig.frame(*session);
    const auto layout = [&] {
      for (std::size_t round = 0; round < 2; ++round) {
        session->view().arrange(renderer, {0, 0, 1400, 900});
      }
    };
    layout();
    TEST_CHECK(session->showsPane());
    TEST_CHECK(session->shows(*rig.focus));
    auto& shell = session->view().shell();
    shell.setState(luciole::workbench::withExpanded(shell.state(), true));
    layout();
    TEST_CHECK(shell.state().expanded);
    TEST_CHECK(rig.pane->visible() && rig.focus->visible()); // hidden by the slot above them
    TEST_CHECK(!session->showsPane());
    TEST_CHECK(!session->shows(*rig.focus));
    TEST_CHECK(session->shows(session->view()));
    shell.setState(luciole::workbench::withExpanded(shell.state(), false));
    layout();
    TEST_CHECK(session->showsPane());
    TEST_CHECK(session->shows(*rig.focus));
    Node outside;
    TEST_CHECK(!session->shows(outside));
  }

  void columnPreviewFollowsTheListing() {
    Rig rig;
    auto session = rig.mount();
    rig.select("note.txt");
    rig.frame(*session);
    const auto& lookahead = session->lookahead();
    const auto& list = rig.browser.model();
    auto& preview = session->columnPreview();
    const LayoutRect slot{.x = 10, .y = 20, .width = 300, .height = 400};
    TEST_CHECK(!preview.placement());
    TEST_CHECK(preview.update(lookahead, list, slot));
    TEST_CHECK(preview.placement() && preview.placement()->previewed.entry.name == "note.txt");
    TEST_CHECK(preview.placement()->previewed.entry.size == 50U);
    TEST_CHECK(preview.placement()->previewed.typeLabel == list.typeLabel(preview.placement()->previewed.entry.id));
    TEST_CHECK(!preview.update(lookahead, list, slot));

    const LayoutRect wider{.x = 10, .y = 20, .width = 320, .height = 400};
    TEST_CHECK(preview.update(lookahead, list, wider));
    TEST_NEAR(preview.placement()->bounds.width, 320.0F, 0.0F);
    TEST_CHECK(!preview.update(lookahead, list, wider));

    // The file grows on disk: once the listing has the new size, so has the placement, at the same place.
    std::ofstream(rig.fixture.root / "note.txt", std::ios::app) << std::string(4000, 'y');
    const auto note = rig.fixture.at("note.txt");
    rig.until([&] {
      const auto id = luciole::rowOf(list, note);
      return id && list.entry(*id)->size == 4050U;
    });
    TEST_CHECK(preview.update(lookahead, list, wider));
    TEST_CHECK(preview.placement() && preview.placement()->previewed.entry.size == 4050U);
    TEST_CHECK(!preview.update(lookahead, list, wider));

    TEST_CHECK(preview.update(lookahead, list, std::nullopt)); // the slot is hidden
    TEST_CHECK(!preview.placement());
    TEST_CHECK(!preview.update(lookahead, list, std::nullopt));

    rig.select("big"); // a folder: the columns stop at no file
    rig.frame(*session);
    TEST_CHECK(!lookahead.preview());
    TEST_CHECK(!preview.update(lookahead, list, wider));
    TEST_CHECK(!preview.placement());
  }

  // The page's preview pane, laid above everything at a column's slot, leaves the slot's divider the edge it presses.
  void previewLeavesTheDividerItsEdge() {
    const LayoutRect slot{.x = 100, .y = 20, .width = 300, .height = 400};
    const auto bounds = luciole::previewBounds(slot);
    TEST_CHECK(bounds.x == 103 && bounds.width == 297 && bounds.y == 20 && bounds.height == 400);
    Style::setRtl(true); // the list, and the divider, are on the slot's right
    const auto mirrored = luciole::previewBounds(slot);
    Style::setRtl(false);
    TEST_CHECK(mirrored.x == 100 && mirrored.width == 297);
    for (const float narrow : {2.0F, -4.0F, std::nanf("")}) { // never wider than the slot, nor negative
      TEST_CHECK(luciole::previewBounds({.x = 0, .y = 0, .width = narrow, .height = 5}).width == 0);
    }

    Rig rig;
    ::test::MetricsRenderer renderer;
    auto session = rig.mount();
    rig.select("note.txt");
    rig.frame(*session);
    session->view().arrange(renderer, {0, 0, 1400, 900});
    session->view().arrange(renderer, {0, 0, 1400, 900}); // the columns are planned, then placed
    const auto& placeholder = session->previewSlot();
    TEST_CHECK(session->shows(placeholder) && placeholder.width() > 0);
    LayoutRect at{.x = 0, .y = 0, .width = placeholder.width(), .height = placeholder.height()};
    Node::absolutePosition(&placeholder, at.x, at.y);
    const auto pane = luciole::previewBounds(at);
    auto made = std::make_unique<InputArea>();
    made->setZIndex(20);
    made->setPosition(pane.x, pane.y);
    made->setFrameSize(pane.width, pane.height);
    int previewPresses = 0;
    made->setOnPress([&previewPresses](const InputArea::PointerData& data) { previewPresses += data.pressed ? 1 : 0; });
    rig.parent.setSize(4000, 4000);
    rig.parent.addChild(std::move(made));
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&rig.parent);
    auto& divider = session->view().center().divider(0);
    float dividerX = 0;
    float dividerY = 0;
    Node::absolutePosition(&divider, dividerX, dividerY);
    const float line = dividerX + (PaneDivider::kHitSize / 2);
    TEST_NEAR(line, at.x, 0.5F); // the divider stands on the slot's edge toward the list
    const float y = at.y + 200;
    // just inside the slot, the divider takes the press
    dispatcher.pointerMotion(line + 2, y, 1);
    dispatcher.pointerButton(line + 2, y, BTN_LEFT, true, 2, 1000, false);
    TEST_CHECK(divider.dragging() && previewPresses == 0);
    dispatcher.pointerButton(line + 2, y, BTN_LEFT, false, 3, 1050, false);
    TEST_CHECK(!divider.dragging());
    // past its hit area, the preview does
    dispatcher.pointerMotion(line + 4, y, 4);
    dispatcher.pointerButton(line + 4, y, BTN_LEFT, true, 5, 3000, false);
    TEST_CHECK(previewPresses == 1 && !divider.dragging());
    dispatcher.pointerButton(line + 4, y, BTN_LEFT, false, 6, 3050, false);
    rig.parent.removeChild(rig.parent.children().back().get()).reset();
  }

  // The list a merged piece opens: a heading, a row per listed entry with its value, and what is left unlisted.
  void moreMenuListsTheMergedEntries() {
    const auto folder = Location::fromPath("/data/photos");
    const auto entry = [&folder](const char* name, bool isFolder, const char* value) {
      return luciole::MergedEntry{
          .entry = {.location = folder.child(name), .folder = isFolder}, .name = name, .value = value
      };
    };
    luciole::WorkbenchMoreRequest request{
        .pane = luciole::WorkbenchPane::Instrument,
        .folder = folder,
        .listed = {entry("2024", true, "3.1 GiB"), entry("cover.jpg", false, "812 KiB")},
        .count = 1502,
        .sceneX = 10,
        .sceneY = 20,
    };
    const auto rows = luciole::moreMenuEntries(request);
    TEST_CHECK(rows.size() == 4 && rows[0].header && rows[0].label == "1,502 more in photos");
    TEST_CHECK(rows[1].id == 1 && rows[1].label == "2024" && rows[1].shortcut == "3.1 GiB" && rows[1].enabled);
    TEST_CHECK(rows[2].id == 2 && rows[2].label == "cover.jpg" && rows[2].ellipsize == TextEllipsize::Middle);
    TEST_CHECK(!rows[3].enabled && rows[3].label == "1,500 more not listed");
    request.count = 2;
    TEST_CHECK(luciole::moreMenuEntries(request).size() == 3); // all listed: no last row
  }

  // The name label of a tile bound for `name`.
  const Label* nameLabel(const Node& node, const std::string& name) {
    if (const auto* label = dynamic_cast<const Label*>(&node); label != nullptr && label->text() == name) {
      return label;
    }
    for (const auto& child : node.children()) {
      if (const auto* found = nameLabel(*child, name)) {
        return found;
      }
    }
    return nullptr;
  }

  void linkedRowIsMarked() {
    Rig rig;
    Palette roles{};
    roles.primary = {.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
    roles.onSurface = {.r = 0.0F, .g = 0.0F, .b = 1.0F, .a = 1.0F};
    setPalette(roles);
    luciole::ThumbnailService thumbnails;
    luciole::FileViewAdapter adapter(
        rig.browser, rig.icons, thumbnails, luciole::ViewMode::List, [] { return 0U; }, [] { return false; }
    );
    const auto& model = rig.browser.model();
    const auto note = luciole::rowOf(model, rig.fixture.at("note.txt"));
    TEST_CHECK(note.has_value());
    const auto index = static_cast<std::size_t>(std::ranges::find(model.rows(), *note) - model.rows().begin());
    auto tile = adapter.createTile();
    const auto bound = [&] {
      adapter.bindTile(*tile, index, false, false);
      const auto* label = nameLabel(*tile, "note.txt");
      TEST_CHECK(label != nullptr);
      return label->color();
    };
    TEST_CHECK(bound() == colorForRole(ColorRole::OnSurface));
    adapter.setLinkedEntry(note);
    TEST_CHECK(adapter.linkedEntry() == note);
    TEST_CHECK(bound() == colorForRole(ColorRole::Primary));
    adapter.setLinkedEntry(std::nullopt);
    TEST_CHECK(bound() == colorForRole(ColorRole::OnSurface));
  }

  // The list's rows ask for a folder's size in List view; entering Workbench stops that, which measures its folders
  // itself, and leaving it starts it again.
  void folderSizesFollowTheViewMode() {
    Rig rig;
    luciole::ThumbnailService thumbnails;
    luciole::FileViewAdapter adapter(
        rig.browser, rig.icons, thumbnails, luciole::ViewMode::List, [] { return 0U; }, [] { return false; }
    );
    const auto& model = rig.browser.model();
    const auto big = luciole::rowOf(model, rig.fixture.at("big"));
    const auto small = luciole::rowOf(model, rig.fixture.at("small"));
    TEST_CHECK(big.has_value() && small.has_value());
    auto tile = adapter.createTile();
    const auto bind = [&](luciole::EntryId id) {
      const auto index = static_cast<std::size_t>(std::ranges::find(model.rows(), id) - model.rows().begin());
      adapter.bindTile(*tile, index, false, false);
    };
    const auto enter = [&](luciole::ViewMode mode) { adapter.setFolderSizeEnabled(luciole::rowsMeasureFolders(mode)); };
    enter(luciole::ViewMode::List);
    bind(*big);
    rig.until([&] { return rig.browser.folderSize(*big) != nullptr; });
    TEST_CHECK(rig.browser.folderSize(*big) != nullptr);

    enter(luciole::ViewMode::Workbench);
    bind(*small);
    for (int round = 0; round < 40; ++round) {
      rig.once();
    }
    TEST_CHECK(rig.browser.folderSize(*small) == nullptr);

    enter(luciole::ViewMode::List);
    bind(*small);
    rig.until([&] { return rig.browser.folderSize(*small) != nullptr; });
    TEST_CHECK(rig.browser.folderSize(*small) != nullptr);
  }

  // In Workbench the Size cell of a folder row shows the session's measure, and the list's own walk stays off. A row
  // whose text the measure has changed since it was bound is reported, and only that one.
  void workbenchFillsFolderSizeCells() {
    Rig rig;
    auto session = rig.mount();
    luciole::ThumbnailService thumbnails;
    luciole::FileViewAdapter adapter(
        rig.browser, rig.icons, thumbnails, luciole::ViewMode::List, [] { return 0U; }, [] { return false; }
    );
    adapter.setFolderSizeEnabled(luciole::rowsMeasureFolders(luciole::ViewMode::Workbench));
    adapter.setFolderSizeText([&](const luciole::DirectoryEntry& entry) { return session->folderSizeText(entry); });
    const auto& model = rig.browser.model();
    const auto big = luciole::rowOf(model, rig.fixture.at("big"));
    const auto note = luciole::rowOf(model, rig.fixture.at("note.txt"));
    TEST_CHECK(big.has_value() && note.has_value());
    auto tile = adapter.createTile();
    const auto bind = [&](luciole::EntryId id) {
      const auto index = static_cast<std::size_t>(std::ranges::find(model.rows(), id) - model.rows().begin());
      adapter.bindTile(*tile, index, false, false);
    };
    const std::string measured = "3.9\u00a0KiB"; // big holds 1000 and 3000 bytes: 4000 bytes
    const auto bigIndex = static_cast<std::size_t>(std::ranges::find(model.rows(), *big) - model.rows().begin());
    rig.frame(*session);
    bind(*big);
    TEST_CHECK(nameLabel(*tile, measured) == nullptr);
    TEST_CHECK(adapter.staleFolderSizes().empty());

    rig.settle(*session);
    TEST_CHECK(adapter.staleFolderSizes() == std::vector<std::size_t>{bigIndex});
    bind(*big);
    TEST_CHECK(nameLabel(*tile, measured) != nullptr);
    TEST_CHECK(adapter.staleFolderSizes().empty());
    TEST_CHECK(rig.browser.folderSize(*big) == nullptr); // no walk of the list's own
    bind(*note);
    TEST_CHECK(nameLabel(*tile, measured) == nullptr);
  }

  void switchingTwentyTimesLeavesNothing() {
    Rig rig;
    rig.select("big");
    std::optional<std::size_t> tables;
    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
      auto session = rig.mount();
      if (!tables) {
        rig.settle(*session); // the first session asks for every walk a session plans
      }
      for (std::size_t round = 0; round < 4; ++round) {
        rig.frame(*session);
        TEST_CHECK(rig.services.measureService().status().running <= luciole::WorkbenchServices::kWalks);
        rig.once();
      }
      session.reset();
      TEST_CHECK(views(rig.parent) == 0);
      TEST_CHECK(rig.parent.children()[1].get() == rig.pane);
      if (!tables) {
        rig.until([&] { return rig.idle(); });
        tables = rig.services.measureCache().tables();
      }
    }
    rig.until([&] { return rig.idle(); });
    TEST_CHECK(*tables > 0); // the first session walked the folder
    for (std::size_t round = 0; round < 20; ++round) {
      rig.once();
      TEST_CHECK(rig.idle()); // nothing left behind starts a walk
    }
    TEST_CHECK(rig.parent.children().size() == 3);
    TEST_CHECK(rig.focus->parent() == rig.pane);
    TEST_CHECK(rig.services.measureCache().tables() == *tables);
    std::println("{} mode switches: {} cache tables, no walk left running", kCycles, *tables);
  }
} // namespace

int main() {
  paneMovesIntoTheViewAndBack();
  feedReadsTheBrowser();
  cursorSteersTheModelsNotTheBrowser();
  previewsFindTheirEntries();
  expandedInstrumentHidesThePane();
  columnPreviewFollowsTheListing();
  previewLeavesTheDividerItsEdge();
  moreMenuListsTheMergedEntries();
  linkedRowIsMarked();
  folderSizesFollowTheViewMode();
  workbenchFillsFolderSizeCells();
  switchingTwentyTimesLeavesNothing();
  std::println("workbench session checks passed");
}
