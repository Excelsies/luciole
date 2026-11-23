#pragma once

#include "app/page/preview_edit_controller.h"
#include "app/page/workbench_preview.h"
#include "browser/miller_columns.h"
#include "browser/workbench_services.h"
#include "code_editor_rig.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "imaging/text_tile_service.h"
#include "media/media_player.h"
#include "png_writer.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "temp_folder.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/controls/button.h"
#include "ui/controls/code_editor_view.h"
#include "ui/controls/label.h"
#include "ui/controls/text_tile_view.h"
#include "ui/workbench/column_preview_view.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <iterator>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// A mounted Workbench session whose column preview edits real text files in a private folder, through the real
// loaders, the editor and the save layer, laid out and drawn offscreen frame by frame as a page does. The question
// stands in for the page's dialog: it records what was asked and is answered through its own callbacks.
namespace luciole::test::edit {
  namespace fs = std::filesystem;
  using Clock = std::chrono::steady_clock;
  using Question = PreviewEditController::Question;

  inline constexpr int kWidth = 1100;
  inline constexpr int kHeight = 700;

  // A private folder with a home of its own, so nothing under the user's home is read or written.
  struct Files {
    TempFolder temp{"luciole-preview-edit-test"};
    fs::path home = temp.path() / "home";
    fs::path folder = temp.path() / "files";

    Files() {
      fs::create_directories(home);
      fs::create_directories(folder);
      for (const char* variable : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
        g_setenv(variable, home.c_str(), TRUE);
      }
    }

    fs::path write(const std::string& name, std::string_view bytes, mode_t mode = 0644) const {
      const auto path = folder / name;
      fs::create_directories(path.parent_path());
      TEST_CHECK((std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes).good());
      TEST_CHECK(::chmod(path.c_str(), mode) == 0);
      return path;
    }

    std::string read(const std::string& name) const {
      std::ifstream input(folder / name, std::ios::binary);
      return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
  };

  // What tells two revisions of a file apart.
  struct Revision {
    ino_t inode = 0;
    timespec modified{};
    timespec changed{};
    off_t size = 0;
    bool operator==(const Revision& other) const {
      return inode == other.inode
          && size == other.size
          && modified.tv_sec == other.modified.tv_sec
          && modified.tv_nsec == other.modified.tv_nsec
          && changed.tv_sec == other.changed.tv_sec
          && changed.tv_nsec == other.changed.tv_nsec;
    }
  };

  inline Revision revisionOf(const fs::path& path) {
    struct stat info{};
    TEST_CHECK(::stat(path.c_str(), &info) == 0);
    return {.inode = info.st_ino, .modified = info.st_mtim, .changed = info.st_ctim, .size = info.st_size};
  }

  inline void keepForViewing(const ::test::GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = fs::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  struct Rig {
    const Files& files;
    ::test::SceneCapture& capture;
    WorkbenchServices services;
    TextTileService text;
    ThumbnailService thumbnails;
    FileIconCache icons;
    AnimationManager animations;
    BrowserController browser{{}, false};
    MillerColumns* miller = nullptr; // set once the Columns view replaces the list, as the page's is
    editor::MemoryClipboard clipboard;
    InputDispatcher dispatcher;
    Node parent;
    Node* pane = nullptr;
    InputArea* listFocus = nullptr;
    std::optional<Question> question; // the question shown now
    int asks = 0;
    int dismissals = 0;
    int frameRequests = 0;
    int reveals = 0; // cursors the editor had to put back; none while the model keeps the cursor on a replaced file
    int width = kWidth;
    int height = kHeight;
    WorkbenchChoices choices;      // what the view is told each frame
    std::vector<std::string> told; // failed saves' sentences shown in an open question
    bool mayAsk = true;
    std::unique_ptr<PreviewEditController> editor; // outlives the preview, as the page's does
    std::unique_ptr<WorkbenchSession> session;
    std::unique_ptr<WorkbenchPreview> preview;
    Clock::duration skew{};
    std::uint32_t serial = 0;
    ::test::GlPixels pixels;

    Rig(const Files& fixture, ::test::SceneCapture& rig, const fs::path& folder) : files(fixture), capture(rig) {
      parent.setAnimationManager(&animations);
      auto made = std::make_unique<Node>();
      made->setFlexGrow(1);
      listFocus = static_cast<InputArea*>(made->addChild(std::make_unique<InputArea>()));
      pane = parent.addChild(std::move(made));
      browser.open(Location::fromPath(folder.native()));
      until([this] { return browser.model().snapshot().status == DirectoryStatus::Ready; });
      editor = std::make_unique<PreviewEditController>(PreviewEditController::Hooks{
          .frameNeeded = [this] { ++frameRequests; },
          .modifiers = [] { return 0U; },
          .clipboard = &clipboard,
          .focused = [this](const InputArea& area) { return dispatcher.focusedArea() == &area; },
          .focus = [this](InputArea& area) { dispatcher.setFocus(&area); },
          .focusList = [this] { dispatcher.setFocus(listFocus); },
          .reveal =
              [this](const Location& file) {
                if (restoreCursor(listModel(), file)) {
                  ++reveals;
                }
              },
          .mayAsk = [this] { return mayAsk; },
          .ask =
              [this](Question asked) {
                ++asks;
                question = std::move(asked);
                return true;
              },
          .tell =
              [this](const std::string& sentence) {
                told.push_back(sentence);
                if (question) {
                  question->sentence = sentence;
                }
              },
          .dismiss =
              [this] {
                ++dismissals;
                // The popup closes as the page's does: its close is reported after the dismissal.
                if (auto shown = std::exchange(question, std::nullopt)) {
                  shown->closed();
                }
              },
      });
      mount();
    }
    ~Rig() { unmount(); }

    // The model of the list the page shows: the Columns view's focused column, else the one list.
    DirectoryModel& listModel() {
      return miller != nullptr && miller->enabled() ? miller->active().model() : browser.model();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // The Workbench view and its column preview, as the page's mode makes them.
    void mount() {
      session = std::make_unique<WorkbenchSession>(
          *pane, listFocus, services.measureCache(), icons, [] { return 0U; },
          WorkbenchModelOptions{.ancestorLevels = 0}
      );
      preview = std::make_unique<WorkbenchPreview>(
          session->previewSlot(), WorkbenchPreviewWorkers{.text = &text, .thumbnails = &thumbnails},
          MediaPlayer::available(),
          WorkbenchPreview::Hooks{
              .frameNeeded = {},
              .open = {},
              .overlay = {},
              .previewSizeChosen = {},
              .clock = [this] { return Clock::now() + skew; },
          }
      );
      preview->setEditor(*editor);
      dispatcher.setSceneRoot(&session->view());
    }

    // Another view mode: the preview goes before the session, as the page's does.
    void unmount() {
      dispatcher.setSceneRoot(nullptr);
      preview.reset();
      session.reset();
    }

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
      for (auto& call : DeferredCall::takePending()) {
        call();
      }
    }

    // One page frame: work that arrived, the session, the preview and the editor before layout, then layout and
    // drawing, then the slot's placement after layout.
    void frame() {
      pump();
      editor->onFrame();
      if (!session) {
        return;
      }
      animations.tick(16);
      session->update(workbenchFeed(browser), choices);
      preview->update();
      pixels = capture.capture(session->view(), width, height, 1);
      auto& slot = session->previewSlot();
      std::optional<LayoutRect> bounds;
      if (session->shows(slot) && slot.width() > 0 && slot.height() > 0) {
        LayoutRect at{.x = 0, .y = 0, .width = slot.width(), .height = slot.height()};
        Node::absolutePosition(&slot, at.x, at.y);
        bounds = session->previewInJump() ? jumpPreviewBounds(at) : previewBounds(at);
      }
      preview->place(*session, browser.model(), bounds, 1);
      thumbnails.endFrame();
    }

    // Half a second of frames, so the controls' fades end.
    void idle() {
      for (int i = 0; i < 30; ++i) {
        frame();
      }
    }

    // Frames until `done`, for at most ten seconds.
    void frames(const std::function<bool()>& done) {
      const auto start = Clock::now();
      while (!done() && Clock::now() - start < std::chrono::seconds(10)) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!done()) {
        report();
      }
      TEST_CHECK(done());
    }

    // What the slot shows, for a wait that failed.
    void report() const {
      if (!session) {
        std::println("no view; editing {}, asks {}", editor->gate().editing(), asks);
        return;
      }
      const auto& shown = bar();
      std::println(
          "slot '{}': tiles {}, editor {}, message '{}'; bar shown {} editing {} enable {} reason '{}' notice '{}'; "
          "editing {} unsaved {} saving {} asking {}; asks {}",
          slot().nameLabel().text(), tiles() != nullptr, codeEditor() != nullptr, slot().message(), shown.shown,
          shown.editing, shown.enableAvailable, shown.reason, shown.notice, editor->gate().editing(), editor->unsaved(),
          editor->saving(), editor->gate().asking(), asks
      );
      std::println(
          "columns aim at a file {}, placed {}, slot shown {} {}x{}, kind {}, load {}",
          session->lookahead().preview().has_value(), session->columnPreview().placement().has_value(),
          session->shows(slot()), slot().width(), slot().height(), preview->kind().has_value(),
          preview->load() != nullptr
      );
      std::println(
          "cursor {}, target {}, rows {}, generation {}", workbenchFeed(browser).cursor.has_value(),
          session->state().target().has_value(), browser.model().rows().size(), browser.model().snapshot().generation
      );
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

    ColumnPreviewView& slot() const { return session->previewSlot(); }
    const ColumnPreviewEditBar& bar() const { return slot().editBar(); }
    CodeEditorView* codeEditor() const { return dynamic_cast<CodeEditorView*>(slot().content()); }
    TextTileView* tiles() const { return dynamic_cast<TextTileView*>(slot().content()); }

    // The file is shown read-only, as text, and its edit bar says whether it can be edited.
    bool offered() const {
      const auto* shown = tiles();
      return shown != nullptr
          && shown->settled()
          && bar().shown
          && !bar().editing
          && (bar().enableAvailable || !bar().reason.empty());
    }

    // Selects `name`, lets the dwell pass and frames until its text and its edit bar show.
    void visit(std::string_view name) {
      select(name);
      frame();
      skew += WorkbenchPreview::kDwell;
      frames([this] { return offered(); });
      TEST_CHECK(slot().nameLabel().text() == name);
    }

    void click(const Node& node) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&node, x, y);
      x += node.width() / 2;
      y += node.height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      (void)dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 0, false);
      (void)dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 0, false);
      frame();
    }

    // Visits `name`, presses Enable editing and frames until the editor has the keyboard.
    void edit(std::string_view name) {
      visit(name);
      TEST_CHECK(bar().enableAvailable);
      click(slot().enableEditingButton());
      frames([this] { return codeEditor() != nullptr && editor->textFocused() && codeEditor()->settled(); });
      TEST_CHECK(bar().editing && !bar().unsaved && !bar().saveAvailable);
    }

    // A key offered as the page offers it: to the editor first while it has the keyboard.
    bool key(std::uint32_t sym, std::uint32_t modifiers = 0, std::uint32_t utf32 = 0) {
      const bool taken = editor->textFocused()
          && editor->key({.sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = true, .preedit = false});
      frame();
      return taken;
    }

    void type(std::string_view text) {
      for (const char c : text) {
        const auto code = static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        TEST_CHECK(key(code, 0, code));
      }
    }

    // Ctrl+S, then frames until the save ends.
    void saveKeys() {
      TEST_CHECK(key('s', KeyMod::Ctrl));
      frames([this] { return !editor->saving(); });
    }

    // The question's Save, then frames until the save ends.
    void answerSave() {
      TEST_CHECK(question.has_value());
      if (question) {
        question->save();
      }
      frames([this] { return !editor->saving(); });
      frame();
    }

    // No pressed, as against the close button or Escape, which choose nothing.
    void chooseNo() {
      TEST_CHECK(question.has_value());
      if (auto shown = std::exchange(question, std::nullopt)) {
        shown->no();
        shown->closed();
      }
      frame();
    }

    // The question closed without a choice: Escape or the close button.
    void dismiss() { answerNo(); }

    // No, the close button and Escape all close the question without Save.
    void answerNo() {
      TEST_CHECK(question.has_value());
      if (auto shown = std::exchange(question, std::nullopt)) {
        shown->closed();
      }
      frame();
    }
  };
} // namespace luciole::test::edit
