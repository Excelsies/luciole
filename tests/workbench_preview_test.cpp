#include "app/page/workbench_preview.h"
#include "browser/workbench_services.h"
#include "column_preview_fixture.h"
#include "imaging/text_document_loader.h"
#include "imaging/text_tile_service.h"
#include "media/media_player.h"
#include "render/animation/animation_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/sheet_preview.h"
#include "thumbnails/thumbnail_service.h"
#include "thumbnails/thumbnail_worker.h"
#include "ui/controls/animated_image_view.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/media_view.h"
#include "ui/controls/mesh_view.h"
#include "ui/controls/table_view.h"
#include "ui/controls/text_tile_view.h"
#include "ui/style.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/pane_shell.h"
#include "workbench/pane_layout.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <map>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <wayland-client-protocol.h>

// The column preview of a mounted Workbench session, following real files of each kind in a private folder through
// the real loaders, helpers and views, laid out and drawn offscreen frame by frame as a page does.
namespace {
  namespace fs = std::filesystem;
  using luciole::ColumnPreviewKind;
  using luciole::ColumnPreviewStatus;
  using luciole::WorkbenchPreview;
  using Clock = std::chrono::steady_clock;

  constexpr int kWidth = 1100;
  constexpr int kHeight = 700;

  struct Rig {
    const luciole::test::preview::PreviewFiles& files;
    ::test::SceneCapture& capture;
    luciole::WorkbenchServices services;
    luciole::TextTileService text;
    luciole::ThumbnailService thumbnails;
    luciole::FileIconCache icons;
    AnimationManager animations;
    luciole::BrowserController browser{{}, false};
    Node parent;
    Node* pane = nullptr;
    InputArea* focus = nullptr;
    std::unique_ptr<luciole::WorkbenchSession> session;
    std::unique_ptr<WorkbenchPreview> preview;
    Clock::duration skew{}; // added to the preview's clock, to let a dwell pass at once
    std::vector<std::optional<luciole::ColumnPreview::Placement>> overlays;
    std::vector<float> overlayTextSizes; // the text size each overlay was given
    std::vector<std::string> opened;
    std::vector<int> sizes; // the preview sizes the user chose

    Rig(const luciole::test::preview::PreviewFiles& fixture, ::test::SceneCapture& rig) : files(fixture), capture(rig) {
      parent.setAnimationManager(&animations);
      auto made = std::make_unique<Node>();
      made->setFlexGrow(1);
      focus = static_cast<InputArea*>(made->addChild(std::make_unique<InputArea>()));
      pane = parent.addChild(std::move(made));
      browser.open(luciole::Location::fromPath(files.folder().native()));
      until([this] { return browser.model().snapshot().status == luciole::DirectoryStatus::Ready; });
      session = std::make_unique<luciole::WorkbenchSession>(
          *pane, focus, services.measureCache(), icons, [] { return 0U; },
          luciole::WorkbenchModelOptions{.ancestorLevels = 0}
      );
      preview = std::make_unique<WorkbenchPreview>(
          session->previewSlot(), luciole::WorkbenchPreviewWorkers{.text = &text, .thumbnails = &thumbnails},
          luciole::MediaPlayer::available(),
          WorkbenchPreview::Hooks{
              .frameNeeded = {},
              .open = [this](const luciole::DirectoryEntry& entry) { opened.push_back(entry.name); },
              .overlay =
                  [this](const std::optional<luciole::ColumnPreview::Placement>& placement, float textSize) {
                    overlays.push_back(placement);
                    overlayTextSizes.push_back(textSize);
                  },
              .previewSizeChosen = [this](int percent) { sizes.push_back(percent); },
              .clock = [this] { return Clock::now() + skew; },
          }
      );
    }
    ~Rig() {
      preview.reset();
      session.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // At most 4000 rounds of the main context and the timers.
    void until(const std::function<bool()>& done) {
      for (std::size_t round = 0; round < 4000 && !done(); ++round) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(done());
    }

    static void pump() {
      for (int i = 0; i < 64 && g_main_context_iteration(nullptr, FALSE) != FALSE; ++i) {
      }
      TimerManager::instance().tick();
    }

    // One page frame: work that arrived, the session and the preview before layout, then layout and drawing, then
    // the slot's placement after layout.
    void frame(float scale = 1) {
      pump();
      animations.tick(16);
      session->update(luciole::workbenchFeed(browser), luciole::WorkbenchChoices{});
      preview->update();
      (void)capture.capture(session->view(), kWidth, kHeight, scale);
      auto& slot = session->previewSlot();
      std::optional<LayoutRect> bounds;
      if (session->shows(slot) && slot.width() > 0 && slot.height() > 0) {
        LayoutRect at{.x = 0, .y = 0, .width = slot.width(), .height = slot.height()};
        Node::absolutePosition(&slot, at.x, at.y);
        bounds = luciole::previewBounds(at);
      }
      preview->place(*session, browser.model(), bounds, scale);
      thumbnails.endFrame();
    }

    // Frames until `done`, for at most ten seconds; returns how long that took.
    Clock::duration frames(const std::function<bool()>& done) {
      const auto start = Clock::now();
      while (!done() && Clock::now() - start < std::chrono::seconds(10)) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(done());
      return Clock::now() - start;
    }

    void select(std::string_view name) {
      auto& model = browser.model();
      for (const auto id : model.rows()) {
        if (model.entry(id)->name == name) {
          TEST_CHECK(model.select(id));
          return;
        }
      }
      std::println("no row {}", name);
      TEST_CHECK(false);
    }

    luciole::ColumnPreviewView& slot() const { return session->previewSlot(); }
    // The page's own preview was last asked to lie over the slot, at the file `name`.
    bool overlay(std::string_view name) const {
      return !overlays.empty() && overlays.back() && overlays.back()->previewed.entry.name == name;
    }
    bool noOverlay() const { return overlays.empty() || !overlays.back(); }
    std::optional<ColumnPreviewStatus> status() const {
      return preview->load() ? std::optional(preview->load()->status()) : std::nullopt;
    }
    template <typename View> View* content() const { return dynamic_cast<View*>(slot().content()); }

    // The file's content is on screen: whatever the view needs to draw it has arrived.
    bool shown() const {
      if (!status() || *status() == ColumnPreviewStatus::Loading) {
        return false;
      }
      if (const auto* tiles = content<TextTileView>()) {
        return tiles->settled();
      }
      if (const auto* animation = content<AnimatedImageView>()) {
        return animation->pictureShown();
      }
      return true;
    }

    // Selects `name`, lets the dwell pass and frames until its content shows; returns how long the load took.
    double visit(std::string_view name) {
      select(name);
      frame();
      skew += WorkbenchPreview::kDwell;
      const auto took = frames([this] {
        return shown() || (preview->kind() && preview->overlaid() && !preview->load() && !preview->waiting());
      });
      return std::chrono::duration<double, std::milli>(took).count();
    }
  };

  // Each kind reaches the view that shows it, with its facts, and text smaller than the list's.
  void eachKindReachesItsView(Rig& rig, std::map<std::string, double>& times) {
    const auto expect = [&](std::string_view name, ColumnPreviewKind kind) {
      times[std::string(name)] = rig.visit(name);
      TEST_CHECK(rig.preview->kind() == kind);
      TEST_CHECK(rig.slot().nameLabel().text() == name);
      TEST_CHECK(!rig.slot().kindLabel().text().empty());
      TEST_CHECK(rig.slot().textSize() < Style::fontSizeBody);
      TEST_NEAR(rig.slot().textSize(), luciole::quietTextSize(Style::fontSizeBody, 0), 0.0F);
    };
    for (const auto* name :
         {"style.css", "notes.md", "data.json", "config.yaml", "setup.ini", "feed.xml", "fix.diff"}) {
      expect(name, ColumnPreviewKind::Text);
      auto* tiles = rig.content<TextTileView>();
      TEST_CHECK(tiles != nullptr && tiles->layoutRevision() > 0 && tiles->residentTiles() > 0);
      TEST_CHECK(rig.slot().factsLabel().text().contains("line"));
      TEST_CHECK(!rig.preview->overlaid() && rig.noOverlay());
    }

    expect("data.csv", ColumnPreviewKind::Table);
    const auto* csv = rig.content<luciole::TableView>();
    TEST_CHECK(csv != nullptr && csv->table() && csv->table()->rowCount == 40 && csv->table()->columnCount == 3);
    TEST_CHECK(rig.slot().factsLabel().text().contains("40 rows, 3 columns"));
    TEST_NEAR(csv->textSize(), rig.slot().textSize(), 0.0F);
    expect("data.tsv", ColumnPreviewKind::Table);
    TEST_CHECK(rig.content<luciole::TableView>() != nullptr);
    expect("book.ods", ColumnPreviewKind::Table);
    TEST_CHECK(rig.content<luciole::TableView>() != nullptr);

    expect("book.xlsx", ColumnPreviewKind::Table);
    auto* sheet = rig.content<luciole::TableView>();
    TEST_CHECK(sheet != nullptr && sheet->table() && sheet->table()->sheetNames.size() == 2);
    TEST_CHECK(sheet->table()->sheetIndex == 0 && sheet->table()->rowCount == 7); // the header row too
    TEST_CHECK(sheet->onSheetChosen != nullptr);
    sheet->onSheetChosen(1); // picked in the sheet list: the same view loads the other sheet
    (void)rig.frames([&] { return sheet->table() && sheet->table()->sheetIndex == 1; });
    TEST_CHECK(rig.content<luciole::TableView>() == sheet);

    expect("cube.stl", ColumnPreviewKind::Mesh);
    const auto* mesh = rig.content<MeshView>();
    TEST_CHECK(mesh != nullptr && mesh->geometry() && mesh->geometry()->triangleCount() == 4);
    TEST_CHECK(rig.slot().factsLabel().text().contains("4 triangles"));

    expect("pack.zip", ColumnPreviewKind::Archive);
    const auto* archive = rig.content<luciole::TableView>();
    TEST_CHECK(archive != nullptr && archive->table() && archive->table()->rowCount == 5);
    TEST_CHECK(luciole::formats::gridCellText(*archive->table(), 4, 0) == "data.bin");
    TEST_CHECK(rig.slot().factsLabel().text().contains("5 entries"));

    expect("blob.bin", ColumnPreviewKind::Binary);
    const auto* hex = rig.content<TextTileView>();
    TEST_CHECK(hex != nullptr && hex->message().contains("hex") && hex->layoutRevision() > 0);

    // A private key is refused by its bytes, whatever its type: the words show and no text is laid out.
    rig.select("id_test");
    rig.frame();
    rig.skew += WorkbenchPreview::kDwell;
    (void)rig.frames([&] { return rig.status() && *rig.status() != ColumnPreviewStatus::Loading; });
    const auto* key = rig.content<TextTileView>();
    TEST_CHECK(key != nullptr && key->message() == "This file holds a private key, so it is not shown.");
    TEST_CHECK(key->layoutRevision() == 0 && key->residentTiles() == 0);

    // A GIF whose frames move plays in the slot, with the page's own preview never over it.
    rig.select("spin.gif");
    rig.frame();
    TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Animation && !rig.preview->overlaid() && rig.noOverlay());
    rig.skew += WorkbenchPreview::kDwell;
    times["spin.gif"] = std::chrono::duration<double, std::milli>(rig.frames([&] { return rig.shown(); })).count();
    TEST_CHECK(!rig.preview->overlaid() && rig.noOverlay() && rig.slot().factsLabel().text().contains("96 × 64"));
    TEST_CHECK(rig.content<AnimatedImageView>() != nullptr && rig.content<AnimatedImageView>()->playing());
    TEST_CHECK(rig.slot().sizeAvailable());

    // A PNG whose frames do not move, and an SVG, are drawn in the slot from the preview decoder, at their own size
    // in a slot larger than they are.
    times["still.png"] = rig.visit("still.png");
    TEST_CHECK(!rig.preview->overlaid() && rig.noOverlay() && rig.status() == ColumnPreviewStatus::Shown);
    const auto* still = rig.content<AnimatedImageView>();
    TEST_CHECK(still != nullptr && still->pictureShown() && !still->playing());
    TEST_CHECK(rig.slot().factsLabel().text().contains("96 × 64") && rig.slot().sizeAvailable());
    TEST_NEAR(still->image().width(), 96, 0.01F);
    TEST_NEAR(still->image().height(), 64, 0.01F);
    times["shape.svg"] = rig.visit("shape.svg");
    TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Picture && !rig.preview->overlaid() && rig.noOverlay());
    TEST_CHECK(rig.content<AnimatedImageView>() != nullptr && rig.content<AnimatedImageView>()->pictureShown());
    // A vector picture has no pixels of its own: it is drawn fitted to the slot and its facts name no pixel size.
    const auto* vector = rig.content<AnimatedImageView>();
    TEST_CHECK(!rig.slot().factsLabel().text().contains(" × "));
    // The box is the slot less 12 pixels of room for the picture's shadow on each side.
    constexpr float kRoom = 24;
    TEST_NEAR(vector->image().width(), std::min(vector->width() - kRoom, (vector->height() - kRoom) * 1.5F), 2.0F);
    // A picture no decoder reads goes back to the page's own preview.
    times["broken.jpg"] = rig.visit("broken.jpg");
    TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Picture && rig.preview->overlaid());
    TEST_CHECK(rig.overlay("broken.jpg") && rig.slot().content() == nullptr && rig.preview->load() == nullptr);

    // Kinds the page's own preview shows are laid over the slot below its header at once, with nothing loading
    // under them; the header's size controls are disabled and say why.
    rig.select("paper.pdf");
    rig.frame();
    rig.frame();
    TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Still && rig.preview->overlaid());
    TEST_CHECK(!rig.preview->waiting() && rig.preview->load() == nullptr);
    TEST_CHECK(rig.overlay("paper.pdf"));
    float slotX = 0;
    float slotY = 0;
    Node::absolutePosition(&rig.slot(), slotX, slotY);
    const auto& below = rig.overlays.back()->bounds;
    TEST_CHECK(below.width > 0 && below.height > 0 && rig.slot().headerHeight() > 20);
    TEST_NEAR(below.y, slotY + rig.slot().headerHeight(), 0.5F);
    TEST_NEAR(below.y + below.height, slotY + rig.slot().height(), 1.0F);
    TEST_CHECK(!rig.slot().sizeAvailable() && !rig.slot().largerButton().enabled());
    TEST_CHECK(rig.slot().largerButton().inputArea()->hasTooltip());
    // Its text is the header's, below the list's, and follows the list's size.
    const float headerText = rig.slot().textSize();
    TEST_NEAR(rig.overlayTextSizes.back(), headerText, 0.001F);
    TEST_CHECK(headerText < Style::fontSizeBody);
    const auto overlaysBefore = rig.overlays.size();
    rig.preview->setListTextSize(18);
    rig.frame();
    TEST_CHECK(rig.overlays.size() == overlaysBefore + 1 && rig.overlay("paper.pdf"));
    TEST_NEAR(rig.overlayTextSizes.back(), rig.slot().textSize(), 0.001F);
    TEST_CHECK(rig.overlayTextSizes.back() > headerText && rig.overlayTextSizes.back() < 18);
    rig.preview->setListTextSize(Style::fontSizeBody);
    rig.frame();
    TEST_NEAR(rig.overlayTextSizes.back(), headerText, 0.001F);

    // Sound plays in the media view where the build has playback; nothing decodes before Play.
    rig.select("tone.wav");
    rig.frame();
    rig.skew += WorkbenchPreview::kDwell;
    rig.frame();
    if (luciole::MediaPlayer::available()) {
      TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Media);
      const auto* media = rig.content<MediaView>();
      TEST_CHECK(media != nullptr && media->player() == nullptr && !rig.preview->overlaid());
      // Video and sound stay fitted: the size controls are disabled and say why.
      TEST_CHECK(!rig.slot().sizeAvailable() && !rig.slot().smallerButton().enabled());
      TEST_CHECK(rig.slot().smallerButton().inputArea()->hasTooltip());
    } else {
      TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Still && rig.preview->overlaid());
    }

    // The Open button opens the file shown.
    rig.slot().onOpen();
    TEST_CHECK(!rig.opened.empty() && rig.opened.back() == "tone.wav");
  }

  // Moving the cursor across files loads none of them; the file it stops on loads once the dwell passed. A newer
  // file cancels the load in flight, whose result never shows.
  void dwellAndCancel(Rig& rig) {
    (void)rig.visit("style.css");
    const auto before = rig.preview->loadsStarted();
    for (const auto* name : {"data.json", "notes.md", "config.yaml", "setup.ini"}) {
      rig.select(name);
      rig.frame();
      TEST_CHECK(rig.preview->waiting());
      TEST_CHECK(rig.slot().content() == nullptr); // the previous file's content went at once
    }
    TEST_CHECK(rig.preview->loadsStarted() == before);
    (void)rig.frames([&] { return rig.preview->loadsStarted() == before + 1; }); // the real dwell passes
    TEST_CHECK(rig.slot().nameLabel().text() == "setup.ini");

    // The workbook's helper takes a while: the slot says it is loading, and the next file takes over from it.
    rig.select("book.xlsx");
    rig.frame();
    rig.skew += WorkbenchPreview::kDwell;
    rig.frame();
    TEST_CHECK(rig.status() == ColumnPreviewStatus::Loading);
    TEST_CHECK(rig.slot().message() == "Loading…");
    const auto started = rig.preview->loadsStarted();
    (void)rig.visit("cube.stl");
    TEST_CHECK(rig.preview->loadsStarted() == started + 1);
    for (int i = 0; i < 40; ++i) { // the workbook's answer would have come by now
      rig.frame();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    TEST_CHECK(rig.content<MeshView>() != nullptr && rig.preview->kind() == ColumnPreviewKind::Mesh);

    // A folder under the cursor previews nothing: the preview goes.
    rig.select("folder");
    rig.frame();
    rig.frame();
    TEST_CHECK(!rig.preview->kind() && rig.preview->load() == nullptr && rig.slot().content() == nullptr);
    TEST_CHECK(rig.slot().nameLabel().text().empty());
  }

  // The same file changed on disk: its new facts show and it loads again.
  void changedFileReloads(Rig& rig) {
    (void)rig.visit("data.csv");
    const auto loads = rig.preview->loadsStarted();
    std::ofstream(rig.files.folder() / "data.csv", std::ios::app) << "file-new.txt,1,2026-11-18\n";
    (void)rig.frames([&] {
      const auto* table = rig.content<luciole::TableView>();
      return rig.preview->loadsStarted() > loads
          && table != nullptr
          && table->table()
          && table->table()->rowCount == 41;
    });
  }

  // The expanded instrument hides the slot: its content goes and comes back once the slot shows again.
  void hiddenSlotLetsGo(Rig& rig) {
    (void)rig.visit("notes.md");
    const auto loads = rig.preview->loadsStarted();
    auto& shell = rig.session->view().shell();
    shell.setState(luciole::workbench::withExpanded(shell.state(), true));
    rig.frame();
    rig.frame();
    TEST_CHECK(!rig.session->shows(rig.slot()));
    TEST_CHECK(rig.preview->load() == nullptr && rig.slot().content() == nullptr);
    TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Text); // the file stays
    shell.setState(luciole::workbench::withExpanded(shell.state(), false));
    (void)rig.frames([&] { return rig.shown(); }); // at once: it was chosen already
    TEST_CHECK(rig.preview->loadsStarted() == loads + 1 && rig.content<TextTileView>() != nullptr);
  }

  // The page goes behind another tab and draws no frames while the application's timers and animations run on: the
  // playing image and the media view go at once, the file stays, and the page's next frame loads it again.
  void hiddenPageLetsGo(Rig& rig) {
    rig.select("spin.gif");
    rig.frame();
    rig.skew += WorkbenchPreview::kDwell;
    (void)rig.frames([&] { return rig.shown(); });
    TEST_CHECK(rig.content<AnimatedImageView>() != nullptr && rig.content<AnimatedImageView>()->playing());
    const auto loads = rig.preview->loadsStarted();
    rig.preview->hide();
    TEST_CHECK(rig.preview->load() == nullptr && rig.slot().content() == nullptr && !rig.preview->waiting());
    TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Animation); // the file stays
    for (int round = 0; round < 20; ++round) {
      Rig::pump();
      rig.animations.tick(16);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    TEST_CHECK(rig.preview->loadsStarted() == loads && rig.slot().content() == nullptr);
    (void)rig.frames([&] { return rig.shown(); });
    TEST_CHECK(rig.preview->loadsStarted() == loads + 1 && rig.content<AnimatedImageView>() != nullptr);
    if (luciole::MediaPlayer::available()) {
      (void)rig.visit("tone.wav");
      TEST_CHECK(rig.content<MediaView>() != nullptr);
      rig.preview->hide();
      TEST_CHECK(rig.content<MediaView>() == nullptr && rig.preview->load() == nullptr);
      TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Media); // the file stays
    }
  }

  // A picture the preview decoder will not take, here one without a modification time, goes back to the page's own
  // preview instead of loading forever.
  void refusedStillGoesToThePage(Rig& rig) {
    const auto& model = rig.browser.model();
    for (const std::string_view name : {"still.png", "shape.svg"}) {
      auto entry =
          *model.entry(*std::ranges::find_if(model.rows(), [&](auto id) { return model.entry(id)->name == name; }));
      entry.modifiedSeconds.reset();
      rig.preview->follow(
          luciole::WorkbenchPreviewTarget{.previewed = {.entry = entry, .typeLabel = "Image"}, .rested = true}, true
      );
      // Frames without placing the slot, which would follow the listing's own entry again.
      const auto start = Clock::now();
      while ((!rig.preview->overlaid() || rig.preview->load() != nullptr)
             && Clock::now() - start < std::chrono::seconds(10)) {
        Rig::pump();
        rig.animations.tick(16);
        rig.preview->update();
        (void)rig.capture.capture(rig.session->view(), kWidth, kHeight, 1);
        rig.thumbnails.endFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      std::println("{} refused by the decoder: overlaid {}", name, rig.preview->overlaid());
      TEST_CHECK(rig.preview->overlaid() && rig.preview->load() == nullptr && rig.slot().content() == nullptr);
    }
  }

  // A text file past the read limit and a table past the grid's rows show their first part, and the facts leave out
  // a line or row count that would pass for the whole file's.
  void cutFilesLeaveTheirCountsOut(Rig& rig) {
    namespace fixture = luciole::test::preview;
    const auto folder = rig.files.folder();
    std::string log;
    for (std::size_t line = 0; line < 1'000'000 && log.size() <= luciole::kTextDocumentReadBytes; ++line) {
      log += "log line\n";
    }
    fixture::writeFile(folder / "huge.log", log);
    fixture::writeFile(folder / "long.csv", fixture::csvText(luciole::formats::kMaxGridRows + 50));
    (void)rig.frames([&] {
      const auto& model = rig.browser.model();
      return luciole::rowOf(model, luciole::Location::fromPath((folder / "huge.log").native()))
          && luciole::rowOf(model, luciole::Location::fromPath((folder / "long.csv").native()));
    });

    (void)rig.visit("huge.log");
    TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Text);
    TEST_CHECK(rig.content<TextTileView>() != nullptr && rig.content<TextTileView>()->residentTiles() > 0);
    TEST_CHECK(rig.slot().factsLabel().text().contains("KiB")); // the facts line is there
    TEST_CHECK(!rig.slot().factsLabel().text().contains("line"));
    (void)rig.visit("long.csv");
    const auto* table = rig.content<luciole::TableView>();
    TEST_CHECK(table != nullptr && table->table() && table->table()->rowsCut && table->table()->rowCount > 0);
    TEST_CHECK(rig.slot().factsLabel().text().contains("KiB"));
    TEST_CHECK(!rig.slot().factsLabel().text().contains("rows"));
  }

  long residentKiB() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
      if (line.starts_with("VmRSS:")) {
        return std::stol(line.substr(6));
      }
    }
    return -1;
  }

  // 200 files of mixed kinds in a row: resident memory levels off instead of growing with every file.
  void memoryAfterManyFiles(Rig& rig) {
    constexpr std::array kNames{"style.css", "data.csv",  "cube.stl", "pack.zip",  "spin.gif",
                                "notes.md",  "book.xlsx", "blob.bin", "still.png", "data.json"};
    std::vector<long> resident;
    for (std::size_t visit = 0; visit < 200; ++visit) {
      (void)rig.visit(kNames[visit % kNames.size()]);
      if ((visit + 1) % 50 == 0) {
        resident.push_back(residentKiB());
        std::println("resident after {} files: {} KiB", visit + 1, resident.back());
      }
    }
    TEST_CHECK(resident.size() == 4 && resident.front() > 0);
    // The second hundred files add no more than a small bound: nothing is kept per file.
    TEST_CHECK(resident.back() - resident[1] < 32 * 1024);
  }

  // Files only these checks use: an SVG, a JPEG no decoder reads and a folder of two files.
  void addFiles(Rig& rig) {
    namespace fixture = luciole::test::preview;
    const auto folder = rig.files.folder();
    fixture::writeFile(
        folder / "shape.svg",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"80\">"
        "<rect width=\"120\" height=\"80\" fill=\"#3584e4\"/><circle cx=\"60\" cy=\"40\" r=\"30\" "
        "fill=\"#e01b24\"/></svg>\n"
    );
    fixture::writeFile(
        folder / "octa.ply",
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n"
    );
    fixture::writeFile(folder / "bad.ply", "ply\nformat\n");
    fixture::writeFile(folder / "broken.jpg", std::string("\xFF\xD8\xFF\xE0", 4) + std::string(200, '\x13'));
    fs::create_directories(folder / "rows");
    fixture::writeFile(folder / "rows" / "inner-a.txt", "a\n");
    fixture::writeFile(folder / "rows" / "inner-b.txt", "b\n");
    rig.until([&] {
      const auto& model = rig.browser.model();
      return std::ranges::all_of(
          std::array{"shape.svg", "octa.ply", "bad.ply", "broken.jpg", "rows"}, [&](const char* name) {
            return luciole::rowOf(model, luciole::Location::fromPath((folder / name).native())).has_value();
          }
      );
    });
  }

  // The kind line names what the preview shows: a model the reader accepts is a 3D model, whatever the listing says of
  // its name; one it refuses keeps the listing's words, next to the sentence that says why.
  void modelsAreNamedAsModels(Rig& rig) {
    for (const auto* name : {"cube.stl", "octa.ply"}) {
      (void)rig.visit(name);
      TEST_CHECK(rig.preview->kind() == ColumnPreviewKind::Mesh && rig.content<MeshView>() != nullptr);
      TEST_CHECK(rig.slot().kindLabel().text() == "3D model");
    }
    (void)rig.visit("bad.ply");
    TEST_CHECK(rig.content<MeshView>() == nullptr && rig.status() == ColumnPreviewStatus::Failed);
    TEST_CHECK(rig.slot().kindLabel().text() != "3D model");
    // The next file starts from its own listing again.
    (void)rig.visit("notes.md");
    TEST_CHECK(rig.slot().kindLabel().text() != "3D model");
  }

  // Frames until `done`, after a new preview size.
  void resized(Rig& rig, int percent, const std::function<bool()>& done) {
    rig.preview->setPreviewSize(percent);
    TEST_CHECK(rig.preview->previewSize() == percent && rig.slot().previewSize() == percent);
    (void)rig.frames(done);
  }

  // The distance between the centers of the row numbers 1 and 2, the table's row height; 0 while they are not shown.
  float rowPitch(const Node& root) {
    std::vector<float> centers;
    const std::function<void(const Node&)> walk = [&](const Node& node) {
      if (const auto* label = dynamic_cast<const Label*>(&node);
          label != nullptr && label->visible() && (label->text() == "1" || label->text() == "2")) {
        float x = 0;
        float y = 0;
        Node::absolutePosition(label, x, y);
        centers.push_back(y + (label->height() / 2));
      }
      for (const auto& child : node.children()) {
        walk(*child);
      }
    };
    walk(root);
    if (centers.size() < 2) {
      return 0;
    }
    std::ranges::sort(centers);
    return centers.back() - centers.front();
  }

  // Each kind the slot draws follows the preview size: text and tables at the header's size times the step, pictures
  // that many times as large, models that much nearer. The header keeps its size, and a new size lays the content
  // out again without loading the file again.
  void sizesEachKind(Rig& rig) {
    const float header = luciole::quietTextSize(Style::fontSizeBody, 0);
    constexpr std::array kSizes{70, 100, 150, 300};
    for (const auto* name : {"style.css", "notes.md", "blob.bin"}) {
      (void)rig.visit(name);
      const auto loads = rig.preview->loadsStarted();
      for (const int percent : kSizes) {
        const float expected = header * static_cast<float>(percent) / 100;
        resized(rig, percent, [&] {
          const auto* tiles = rig.content<TextTileView>();
          return tiles != nullptr && std::abs(tiles->textSize() - expected) < 1e-4F && tiles->settled();
        });
        TEST_NEAR(rig.preview->contentTextSize(), expected, 1e-5F);
        TEST_NEAR(rig.slot().textSize(), header, 0.0F);
        std::println(
            "{} at {}%: text {:.2f}, layout {} px tall", name, percent, expected,
            rig.content<TextTileView>()->geometry().height
        );
      }
      TEST_CHECK(rig.preview->loadsStarted() == loads);
    }

    for (const auto* name : {"data.csv", "pack.zip"}) {
      (void)rig.visit(name);
      rig.preview->setPreviewSize(100);
      const auto table = [&] { return rig.content<luciole::TableView>(); };
      (void)rig.frames([&] { return std::abs(table()->textSize() - header) < 1e-4F && rowPitch(*table()) > 0; });
      const float pitch = rowPitch(*table());
      for (const int percent : kSizes) {
        const float expected = header * static_cast<float>(percent) / 100;
        resized(rig, percent, [&] {
          return std::abs(table()->textSize() - expected) < 1e-4F
              && std::abs(rowPitch(*table()) - (pitch * static_cast<float>(percent) / 100)) < 1.5F;
        });
        const float rows = rowPitch(*table());
        std::println("{} at {}%: text {:.2f}, rows {:.1f} px apart", name, percent, expected, rows);
        TEST_NEAR(rows, pitch * static_cast<float>(percent) / 100, 1.5F);
      }
    }

    for (const auto* name : {"still.png", "spin.gif"}) {
      (void)rig.visit(name);
      const auto loads = rig.preview->loadsStarted();
      for (const int percent : kSizes) {
        const float factor = static_cast<float>(percent) / 100;
        resized(rig, percent, [&] { return rig.content<AnimatedImageView>()->previewScale() == factor; });
        rig.frame();
        const auto* picture = rig.content<AnimatedImageView>();
        // 96 by 64 fits the slot, so its own size times the step.
        TEST_NEAR(picture->image().width(), 96 * factor, 0.01F);
        TEST_NEAR(picture->image().height(), 64 * factor, 0.01F);
        // The picture is seen in the slot less 12 pixels of room for its shadow on each side.
        constexpr float kRoom = 24;
        TEST_NEAR(picture->maxScrollX(), std::max(0.0F, (96 * factor) - (picture->width() - kRoom)), 0.01F);
        TEST_NEAR(picture->maxScrollY(), std::max(0.0F, (64 * factor) - (picture->height() - kRoom)), 0.01F);
        std::println(
            "{} at {}%: {:.0f} by {:.0f} in {:.0f} by {:.0f}", name, percent, picture->image().width(),
            picture->image().height(), picture->width(), picture->height()
        );
      }
      TEST_CHECK(rig.preview->loadsStarted() == loads);
    }

    (void)rig.visit("cube.stl");
    rig.preview->setPreviewSize(100);
    rig.frame();
    const auto distance = [&] {
      // The camera's distance from the bounds' center, which the model view maps onto the axis.
      const auto* mesh = rig.content<MeshView>();
      const auto& box = mesh->geometry()->bounds;
      const auto& m = mesh->camera(1).modelView;
      const float x = (box.min.x + box.max.x) / 2;
      const float y = (box.min.y + box.max.y) / 2;
      const float z = (box.min.z + box.max.z) / 2;
      return -((m[2] * x) + (m[6] * y) + (m[10] * z) + m[14]);
    };
    const float fitted = distance();
    for (const int percent : kSizes) {
      const float factor = static_cast<float>(percent) / 100;
      resized(rig, percent, [&] { return rig.content<MeshView>()->previewScale() == factor; });
      std::println("cube.stl at {}%: camera {:.3f} from the center", percent, distance());
      TEST_NEAR(distance(), fitted / factor, fitted * 1e-4F);
    }
    rig.preview->setPreviewSize(100);
    TEST_CHECK(rig.sizes.empty()); // nobody chose these: nothing is saved
  }

  // Ctrl with the wheel over the content steps the size the way the controls do, and the owner saves it.
  void wheelSizes(Rig& rig) {
    (void)rig.visit("style.css");
    const auto* content = rig.slot().content();
    float x = 0;
    float y = 0;
    Node::absolutePosition(content, x, y);
    x += content->width() / 2;
    y += content->height() / 2;
    constexpr auto kVertical = WL_POINTER_AXIS_VERTICAL_SCROLL;
    // A turn sideways is left alone; a notch up, by its high-resolution value, is one size larger.
    TEST_CHECK(!rig.preview->wheelSize(x, y, WL_POINTER_AXIS_HORIZONTAL_SCROLL, -120, -1, -15));
    TEST_CHECK(rig.preview->wheelSize(x, y, kVertical, -120, -1, -15));
    TEST_CHECK(rig.preview->previewSize() == 110 && (rig.sizes == std::vector{110}));
    const float header = luciole::quietTextSize(Style::fontSizeBody, 0);
    (void)rig.frames([&] { return std::abs(rig.content<TextTileView>()->textSize() - (header * 1.1F)) < 1e-4F; });
    // Two notches down by their discrete count, then a wheel that sends only a distance: 15 per notch.
    TEST_CHECK(rig.preview->wheelSize(x, y, kVertical, 0, 2, 30));
    TEST_CHECK(rig.preview->previewSize() == 90 && (rig.sizes == std::vector{110, 90}));
    TEST_CHECK(rig.preview->wheelSize(x, y, kVertical, 0, 0, 15));
    TEST_CHECK(rig.preview->previewSize() == 80 && (rig.sizes == std::vector{110, 90, 80}));
    // Over the header nothing is taken.
    float slotX = 0;
    float slotY = 0;
    Node::absolutePosition(&rig.slot(), slotX, slotY);
    TEST_CHECK(!rig.preview->wheelSize(x, slotY + (rig.slot().headerHeight() / 2), kVertical, 120, 1, 15));
    // The step itself returns to 100%.
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&rig.parent);
    float bx = 0;
    float by = 0;
    Node::absolutePosition(&rig.slot().sizeButton(), bx, by);
    bx += rig.slot().sizeButton().width() / 2;
    by += rig.slot().sizeButton().height() / 2;
    dispatcher.pointerEnter(bx, by, 1);
    dispatcher.pointerButton(bx, by, BTN_LEFT, true, 2, 0, false);
    dispatcher.pointerButton(bx, by, BTN_LEFT, false, 3, 0, false);
    TEST_CHECK(rig.preview->previewSize() == 100 && rig.sizes.back() == 100);
    rig.sizes.clear();
  }

  // The rows of the columns ahead keep their size, below the list's, whatever the preview size.
  void columnsAheadKeepTheirSize(Rig& rig) {
    const auto rowLabels = [&] {
      std::vector<const Label*> found;
      const std::function<void(const Node&)> walk = [&](const Node& node) {
        if (&node == &rig.slot()) {
          return;
        }
        if (const auto* label = dynamic_cast<const Label*>(&node); label != nullptr
            && label->visible()
            && (label->text() == "inner-a.txt" || label->text() == "inner-b.txt")) {
          found.push_back(label);
        }
        for (const auto& child : node.children()) {
          walk(*child);
        }
      };
      walk(rig.session->view());
      return found;
    };
    rig.select("rows");
    (void)rig.frames([&] { return rowLabels().size() == 2; });
    for (const int percent : {70, 100, 300}) {
      rig.preview->setPreviewSize(percent);
      rig.frame();
      rig.frame();
      const auto labels = rowLabels();
      TEST_CHECK(labels.size() == 2);
      for (const auto* label : labels) {
        TEST_NEAR(label->fontSize(), luciole::quietTextSize(Style::fontSizeBody, 0), 0.0F);
        TEST_CHECK(label->fontSize() < Style::fontSizeBody);
      }
    }
    rig.preview->setPreviewSize(100);
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--mesh-preview-worker") {
    return luciole::runMeshPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(std::span(args).subspan(1));
  }
  if (!args.empty()) {
    return 2; // never run the checks again from a helper call this test does not serve
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  // Static, so a failed check, which exits, still removes the folder.
  static const luciole::test::preview::PreviewFiles files;
  std::map<std::string, double> times;
  {
    Rig rig(files, *capture);
    addFiles(rig);
    eachKindReachesItsView(rig, times);
    modelsAreNamedAsModels(rig);
    sizesEachKind(rig);
    wheelSizes(rig);
    columnsAheadKeepTheirSize(rig);
    dwellAndCancel(rig);
    changedFileReloads(rig);
    hiddenSlotLetsGo(rig);
    hiddenPageLetsGo(rig);
    memoryAfterManyFiles(rig);
    cutFilesLeaveTheirCountsOut(rig);
    refusedStillGoesToThePage(rig);
    // Leaving the mode drops the preview: the slot keeps nothing.
    rig.preview.reset();
    TEST_CHECK(rig.session->previewSlot().content() == nullptr);
    TEST_CHECK(rig.session->previewSlot().nameLabel().text().empty());
  }
  for (const auto& [name, ms] : times) {
    std::println("first content after the dwell: {} {:.1f} ms", name, ms);
  }
  std::println("workbench-preview: all checks passed");
  return 0;
}
