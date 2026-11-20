#include "app/page/held_view_change.h"
#include "preview_edit_rig.h"
#include "ui/controls/flex.h"
#include "ui/controls/segmented.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// A view change with unsaved text in the column preview's editor: the question comes while the Workbench view with
// the editor is still shown, the view changes once it is answered, and the file is selected in the new view.
namespace {
  using namespace luciole::test::edit;
  using luciole::HeldViewChange;
  using luciole::ViewMode;

  struct Folder {
    Files files;
    explicit Folder(const std::string& name = "files") {
      files.folder = files.temp.path() / name;
      files.write("a.txt", "alpha\n");
      files.write("b.txt", "beta\n");
    }
  };

  constexpr std::array kModes{ViewMode::Grid, ViewMode::List, ViewMode::Miller, ViewMode::Workbench};

  std::size_t segmentOf(ViewMode mode) {
    return static_cast<std::size_t>(std::ranges::find(kModes, mode) - kModes.begin());
  }

  void collectButtons(Node& node, std::vector<Button*>& out) {
    if (auto* button = dynamic_cast<Button*>(&node)) {
      out.push_back(button);
    }
    for (const auto& child : node.children()) {
      collectButtons(*child, out);
    }
  }

  // The page's toolbar view switch wired as the page wires it: a real segment control whose change reports go to the
  // held view change, which keeps the segment on the current view. Unmounting stands for the new view taking the
  // Workbench's place.
  struct Toolbar {
    Rig& rig;
    int& changes; // outside the toolbar, so it can be read after the toolbar is gone
    int leaves = 0;
    ViewMode mode = ViewMode::Workbench;
    Flex root;
    Segmented* segment = nullptr;
    InputDispatcher dispatcher;
    HeldViewChange change;

    Toolbar(Rig& owner, int& applied)
        : rig(owner), changes(applied), change(
                                            {.current = [this] { return mode; },
                                             .segmentOf = segmentOf,
                                             .leave =
                                                 [this](std::function<void()> resume) {
                                                   ++leaves;
                                                   return rig.editor->leave(std::move(resume));
                                                 },
                                             .unsaved = [this] { return rig.editor->unsaved(); },
                                             .apply =
                                                 [this](ViewMode wanted) {
                                                   rig.unmount();
                                                   mode = wanted;
                                                   ++changes;
                                                 }}
                                        ) {
      root.setAnimationManager(&rig.animations);
      auto made = std::make_unique<Segmented>();
      for (const char* label : {"Grid", "List", "Columns", "Workbench"}) {
        made->addOption(label);
      }
      made->setSelectedIndex(segmentOf(mode));
      made->setOnChange([this](std::size_t index) {
        change.changed(index, [this](std::size_t picked) { change.request(kModes.at(picked)); });
      });
      segment = static_cast<Segmented*>(root.addChild(std::move(made)));
      change.setSegment(segment);
      rig.capture.capture(root, 480, 48, 1);
      dispatcher.setSceneRoot(&root);
    }

    // A pointer click on a segment's button through the toolkit's dispatcher.
    void click(ViewMode target) {
      std::vector<Button*> buttons;
      collectButtons(*segment, buttons);
      TEST_CHECK(buttons.size() == kModes.size());
      if (buttons.size() != kModes.size()) {
        return;
      }
      const Button& button = *buttons.at(segmentOf(target));
      float x = 0;
      float y = 0;
      Node::absolutePosition(&button, x, y);
      x += button.width() / 2;
      y += button.height() / 2;
      dispatcher.pointerMotion(x, y, 1);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, 2, 0, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, 3, 0, false);
    }

    bool shows(ViewMode wanted) const { return segment->selectedIndex() == segmentOf(wanted); }

    void stillWorkbench() const {
      TEST_CHECK(
          rig.session != nullptr && rig.codeEditor() != nullptr && rig.bar().editing && shows(ViewMode::Workbench)
      );
    }
  };

  std::optional<luciole::EntryId> idOf(Rig& rig, std::string_view name) {
    return luciole::rowOf(rig.browser.model(), luciole::Location::fromPath((rig.files.folder / name).native()));
  }

  bool selected(Rig& rig, std::string_view name) {
    const auto id = idOf(rig, name);
    return id && rig.browser.model().selection().selected.contains(*id);
  }

  // A click on the Grid segment with unsaved text: the question comes first, the Workbench view and its segment stay.
  void openQuestionByClick(Rig& rig, Toolbar& bar, int& changes) {
    rig.type("1");
    bar.click(ViewMode::Grid);
    rig.frames([&] { return rig.question.has_value(); });
    rig.frame();
    TEST_CHECK(changes == 0 && rig.asks == 1 && bar.mode == ViewMode::Workbench);
    bar.stillWorkbench();
  }

  // Save: the question is asked first, the file is written, the view changes once, and when the listing lists the
  // saved file under a new entry the file is selected again.
  void saveThenChange(::test::SceneCapture& capture, const std::string& folderName) {
    const Folder folder(folderName);
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    TEST_CHECK(selected(rig, "a.txt"));
    const auto before = idOf(rig, "a.txt");
    int changes = 0;
    Toolbar bar(rig, changes);
    openQuestionByClick(rig, bar, changes);
    TEST_CHECK(folder.files.read("a.txt") == "alpha\n"); // nothing is written before the answer
    rig.answerSave();
    rig.frames([&] { return changes == 1; });
    TEST_CHECK(folder.files.read("a.txt") == "1alpha\n" && rig.session == nullptr);
    for (int i = 0; i < 10; ++i) {
      rig.frame();
    }
    TEST_CHECK(changes == 1 && bar.mode == ViewMode::Grid && bar.shows(ViewMode::Grid));
    rig.frames([&] {
      rig.editor->watch(rig.browser.model());
      return idOf(rig, "a.txt") != before && selected(rig, "a.txt");
    });
    TEST_CHECK(rig.browser.model().selection().selected.size() == 1 && !rig.editor->holdsCursor());
  }

  // Save, then the Columns view: its own column controllers list the folder anew, and the file is selected in the
  // focused column once that column lists it.
  void testSaveThenColumns(::test::SceneCapture& capture) {
    Files files;
    files.write("first.txt", "alpha\n");
    Rig rig(files, capture, files.folder);
    rig.edit("first.txt");
    const auto idBefore = idOf(rig, "first.txt");
    TEST_CHECK(idBefore.has_value());
    rig.type("1");
    int changes = 0;
    Toolbar bar(rig, changes);
    bar.click(ViewMode::Miller);
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerSave();
    rig.frames([&] {
      rig.editor->watch(rig.listModel()); // the page's per-frame call
      return changes == 1;
    });
    TEST_CHECK(files.read("first.txt") == "1alpha\n" && rig.session == nullptr);
    luciole::MillerColumns miller;
    miller.start(rig.browser);
    rig.browser.stop();
    rig.miller = &miller;
    const auto file = luciole::Location::fromPath((files.folder / "first.txt").native());
    const auto picked = [&] {
      const auto id = luciole::rowOf(rig.listModel(), file);
      return id && rig.listModel().selection().selected.contains(*id);
    };
    const auto start = Clock::now();
    const auto step = [&] {
      Rig::pump();
      for (std::size_t i = 0; i < miller.columns().size(); ++i) {
        miller.load(i);
      }
      miller.synchronize();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    // The column's own listing gives the file the entry id the other listing gave it before the save, so only the
    // file's identity on disk tells the saved file from the old one.
    while (!luciole::rowOf(rig.listModel(), file) && Clock::now() - start < std::chrono::seconds(10)) {
      step();
    }
    TEST_CHECK(luciole::rowOf(rig.listModel(), file) == idBefore);
    while (!picked() && Clock::now() - start < std::chrono::seconds(10)) {
      step();
      rig.editor->watch(rig.listModel());
    }
    TEST_CHECK(picked() && rig.listModel().selection().selected.size() == 1);
    TEST_CHECK(miller.enabled() && !rig.editor->holdsCursor());
    rig.miller = nullptr;
  }

  void testSaveThenChange(::test::SceneCapture& capture) { saveThenChange(capture, "files"); }

  // The folder's name has a space and a non-ASCII letter: the edited file's folder is still the listed one.
  void testSaveThenChangeInOddFolder(::test::SceneCapture& capture) { saveThenChange(capture, "my files été"); }

  // No: the view changes once after the answer, nothing is written, the file stays selected, the segment shows the
  // new view.
  void testNoThenChange(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    const auto path = folder.files.folder / "a.txt";
    const auto written = revisionOf(path);
    const auto before = idOf(rig, "a.txt");
    int changes = 0;
    Toolbar bar(rig, changes);
    openQuestionByClick(rig, bar, changes);
    rig.answerNo();
    rig.frames([&] { return changes == 1; });
    for (int i = 0; i < 10; ++i) {
      rig.frame();
      rig.editor->watch(rig.browser.model());
    }
    TEST_CHECK(changes == 1 && rig.asks == 1 && rig.reveals == 0);
    TEST_CHECK(bar.mode == ViewMode::Grid && bar.shows(ViewMode::Grid));
    TEST_CHECK(folder.files.read("a.txt") == "alpha\n" && revisionOf(path) == written); // nothing was written
    TEST_CHECK(idOf(rig, "a.txt") == before && selected(rig, "a.txt") && !rig.editor->holdsCursor());
  }

  // Two different clicks while the question is open: one change, to the last, with no view between, and the segment
  // shows the current view in the meantime.
  void testLastClickWins(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    int changes = 0;
    Toolbar bar(rig, changes);
    bar.click(ViewMode::Miller);
    rig.frames([&] { return rig.question.has_value(); });
    bar.stillWorkbench();
    bar.click(ViewMode::List);
    rig.frame();
    TEST_CHECK(changes == 0 && bar.mode == ViewMode::Workbench && rig.asks == 1);
    bar.stillWorkbench();
    rig.answerNo();
    rig.frames([&] { return changes == 1; });
    for (int i = 0; i < 10; ++i) {
      rig.frame();
    }
    TEST_CHECK(changes == 1 && bar.mode == ViewMode::List && bar.shows(ViewMode::List) && rig.asks == 1);
    TEST_CHECK(folder.files.read("a.txt") == "alpha\n" && selected(rig, "a.txt"));
  }

  // A second request while the question is open: the segment shows the current view again at once, the page is asked
  // to leave again for each request (a dropped resume is replaced), and the change still applies once, to the last.
  void testRequestWhileHeldKeepsOneChange(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    int changes = 0;
    Toolbar bar(rig, changes);
    bar.click(ViewMode::Miller);
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(bar.leaves == 1);
    bar.click(ViewMode::List);
    TEST_CHECK(bar.leaves == 2 && bar.shows(ViewMode::Workbench) && changes == 0 && rig.asks == 1);
    rig.answerNo();
    rig.frames([&] { return changes >= 1; });
    for (int i = 0; i < 10; ++i) {
      rig.frame();
    }
    TEST_CHECK(changes == 1 && bar.mode == ViewMode::List && bar.shows(ViewMode::List));
  }

  // A mode is held and the text is back to the saved text while the question is still open: nothing is left to
  // ask, so the next request applies at once, without asking the page to leave again.
  void testRequestWhenUndoneWhileHeld(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    int changes = 0;
    Toolbar bar(rig, changes);
    bar.click(ViewMode::Miller);
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(rig.key('z', KeyMod::Ctrl));
    TEST_CHECK(!rig.editor->unsaved() && changes == 0);
    bar.click(ViewMode::List);
    TEST_CHECK(changes == 1 && bar.mode == ViewMode::List && bar.shows(ViewMode::List) && bar.leaves == 1);
  }

  // A request that does not come from the segment, as a keyboard shortcut makes it, is held and applied the same way.
  void testDirectRequest(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    int changes = 0;
    Toolbar bar(rig, changes);
    bar.change.request(ViewMode::Grid);
    rig.frames([&] { return rig.question.has_value(); });
    bar.stillWorkbench();
    rig.answerNo();
    rig.frames([&] { return changes == 1; });
    TEST_CHECK(changes == 1 && bar.mode == ViewMode::Grid && bar.shows(ViewMode::Grid));
  }

  // The owner is destroyed while its view change is held: the deferred call does nothing.
  void testOwnerGoneBeforeTheChange(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    int changes = 0;
    {
      std::optional<Toolbar> bar(std::in_place, rig, changes);
      bar->change.request(ViewMode::Grid);
      rig.frames([&] { return rig.question.has_value(); });
      if (auto shown = std::exchange(rig.question, std::nullopt)) {
        shown->closed(); // the resume is queued; the owner goes before the main loop runs it
      }
      TEST_CHECK(changes == 0);
      bar.reset();
    }
    rig.frames([&] { return !rig.editor->gate().asking(); });
    rig.frame();
    TEST_CHECK(changes == 0 && rig.session != nullptr); // the view never changed
    TEST_CHECK(folder.files.read("a.txt") == "alpha\n");
  }

  // The answer is on its way (the editor closed, the resume is queued) but a new request comes first: nothing is left
  // to ask, so it applies at once and the queued resume finds nothing held.
  void testRequestWhenNothingIsLeftToAsk(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    int changes = 0;
    Toolbar bar(rig, changes);
    bar.change.request(ViewMode::Grid);
    rig.frames([&] { return rig.question.has_value(); });
    if (auto shown = std::exchange(rig.question, std::nullopt)) {
      shown->closed();
    }
    TEST_CHECK(changes == 0 && !rig.editor->unsaved());
    bar.change.request(ViewMode::List);
    TEST_CHECK(changes == 1 && bar.mode == ViewMode::List && bar.shows(ViewMode::List));
    for (int i = 0; i < 10; ++i) {
      rig.frame();
    }
    TEST_CHECK(changes == 1 && bar.mode == ViewMode::List);
  }

  // A save that fails keeps the question and the Workbench view; the view changes after the next answer.
  void testFailedSaveKeepsTheView(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    int changes = 0;
    Toolbar bar(rig, changes);
    bar.click(ViewMode::Grid);
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(::chmod(folder.files.folder.c_str(), 0555) == 0);
    rig.answerSave();
    TEST_CHECK(::chmod(folder.files.folder.c_str(), 0755) == 0);
    TEST_CHECK(rig.question.has_value() && !rig.told.empty() && changes == 0);
    bar.stillWorkbench();
    TEST_CHECK(folder.files.read("a.txt") == "alpha\n" && rig.editor->unsaved());
    rig.answerNo(); // Escape and the close button answer as No does
    rig.frames([&] { return changes == 1; });
    TEST_CHECK(folder.files.read("a.txt") == "alpha\n" && selected(rig, "a.txt"));
  }

  // Without unsaved text a click changes the view at once and asks nothing.
  void testCleanChange(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    int changes = 0;
    Toolbar bar(rig, changes);
    bar.click(ViewMode::Grid);
    TEST_CHECK(changes == 1 && rig.asks == 0 && selected(rig, "a.txt"));
    TEST_CHECK(bar.mode == ViewMode::Grid && bar.shows(ViewMode::Grid));
  }
} // namespace

int main() {
  if (::geteuid() == 0) {
    return 77; // root writes into read-only folders, so the refused save cannot be shown
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  testSaveThenChange(*capture);
  testSaveThenChangeInOddFolder(*capture);
  testSaveThenColumns(*capture);
  testLastClickWins(*capture);
  testDirectRequest(*capture);
  testRequestWhileHeldKeepsOneChange(*capture);
  testRequestWhenUndoneWhileHeld(*capture);
  testRequestWhenNothingIsLeftToAsk(*capture);
  testOwnerGoneBeforeTheChange(*capture);
  testNoThenChange(*capture);
  testFailedSaveKeepsTheView(*capture);
  testCleanChange(*capture);
  std::println("preview-edit-view-change: all checks passed");
  return 0;
}
