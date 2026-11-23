#include "preview_edit_rig.h"

#include <string>
#include <unistd.h>

// Editing a text file in the column preview: Enable Editing, typing, Save and the bytes on disk, the reasons a file
// cannot be edited, and saves the save layer refuses.
namespace {
  using namespace luciole::test::edit;

  // A file whose text is replaced: its own line endings, byte order mark and final newline are kept.
  void testSavedBytes(::test::SceneCapture& capture) {
    const Files files;
    struct Case {
      std::string name;
      std::string bytes;
      std::string after; // after "X" is typed at the start
    };
    const std::vector<Case> cases{
        {"lf.txt", "one\ntwo\n", "Xone\ntwo\n"},
        {"crlf.txt", "one\r\ntwo\r\n", "Xone\r\ntwo\r\n"},
        {"mark.txt", "\xEF\xBB\xBFone\ntwo\n", "\xEF\xBB\xBFXone\ntwo\n"},
        {"open.txt", "one\ntwo", "Xone\ntwo"},
    };
    for (const auto& item : cases) {
      files.write(item.name, item.bytes);
    }
    Rig rig(files, capture, files.folder);
    for (const auto& item : cases) {
      rig.edit(item.name);
      rig.type("X");
      TEST_CHECK(rig.bar().unsaved && rig.bar().saveAvailable && rig.editor->unsaved());
      TEST_CHECK(files.read(item.name) == item.bytes); // only Save writes
      rig.saveKeys();
      TEST_CHECK(files.read(item.name) == item.after);
      TEST_CHECK(!rig.bar().unsaved && !rig.bar().saveAvailable && !rig.editor->unsaved());
      TEST_CHECK(rig.bar().editing && rig.codeEditor() != nullptr); // saving leaves the editor open
      // A second save of the new revision works: the identity of our own save was kept.
      (void)rig.key(XKB_KEY_End, KeyMod::Ctrl);
      rig.type("Y");
      rig.saveKeys();
      TEST_CHECK(files.read(item.name) == item.after + "Y");
      TEST_CHECK(!rig.editor->unsaved());
      TEST_CHECK(rig.bar().notice.empty());
      rig.click(rig.slot().stopEditingButton());
      rig.frames([&] { return rig.offered(); });
      TEST_CHECK(!rig.editor->gate().editing() && rig.tiles() != nullptr);
    }
    TEST_CHECK(files.read("lf.txt") == "Xone\ntwo\nY");
    TEST_CHECK(files.read("crlf.txt") == "Xone\r\ntwo\r\nY");
    TEST_CHECK(files.read("mark.txt") == "\xEF\xBB\xBFXone\ntwo\nY");
    TEST_CHECK(files.read("open.txt") == "Xone\ntwoY");
    TEST_CHECK(rig.asks == 0);
  }

  // The bar's Save button writes too, and the file's facts arriving from the listing after our own save do not
  // restart the preview or say the file changed.
  void testSaveButtonAndOwnSave(::test::SceneCapture& capture) {
    const Files files;
    files.write("notes.md", "# Notes\n\nSome text.\n");
    Rig rig(files, capture, files.folder);
    rig.edit("notes.md");
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-editing.png");
    auto* view = rig.codeEditor();
    rig.type("Hi ");
    rig.idle(); // the buttons' state changes fade in
    keepForViewing(rig.pixels, "preview-edit-unsaved.png");
    rig.click(rig.slot().saveButton());
    TEST_CHECK(rig.editor->textFocused()); // the keyboard goes back to the editor
    rig.frames([&] { return !rig.editor->saving(); });
    TEST_CHECK(files.read("notes.md") == "Hi # Notes\n\nSome text.\n");
    const auto rows = rig.browser.model().snapshot().generation;
    rig.frames([&] { return rig.browser.model().snapshot().generation != rows; }); // the listing sees the save
    for (int i = 0; i < 10; ++i) {
      rig.frame();
    }
    TEST_CHECK(rig.codeEditor() == view);
    TEST_CHECK(rig.bar().notice.empty());
    TEST_CHECK(rig.preview->loadsStarted() == 1);
    TEST_CHECK(rig.slot().factsLabel().text().starts_with("23 bytes")); // the facts follow the listing
    // The save replaced the file; the list's model moved the cursor to the new entry itself, so nothing was put back.
    const auto cursor = luciole::workbenchFeed(rig.browser).cursor;
    TEST_CHECK(rig.reveals == 0 && cursor && cursor->location.basename() == "notes.md");
    TEST_CHECK(rig.browser.model().selection().selected.size() == 1);
  }

  // A file that was opened and not changed, or changed back, is never written.
  void testUnchangedNeverWritten(::test::SceneCapture& capture) {
    const Files files;
    const auto path = files.write("keep.txt", "same\n");
    const auto before = revisionOf(path);
    Rig rig(files, capture, files.folder);
    rig.edit("keep.txt");
    (void)rig.key('s', KeyMod::Ctrl);
    rig.click(rig.slot().saveButton()); // disabled: nothing to save
    rig.click(*rig.codeEditor());       // back into the text
    rig.type("x");
    TEST_CHECK(rig.bar().unsaved && rig.bar().saveAvailable);
    TEST_CHECK(rig.key('z', KeyMod::Ctrl));
    TEST_CHECK(!rig.bar().unsaved && !rig.bar().saveAvailable);
    rig.type("y");
    TEST_CHECK(rig.key('z', KeyMod::Ctrl)); // undone back to the text as read
    TEST_CHECK(!rig.editor->unsaved());
    (void)rig.key('s', KeyMod::Ctrl);
    rig.key(XKB_KEY_Escape);
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.asks == 0);
    TEST_CHECK(revisionOf(path) == before);
  }

  // Mixed line endings: a notice says which one a save writes, and the save writes it throughout.
  void testMixedEndings(::test::SceneCapture& capture) {
    const Files files;
    files.write("mixed.txt", "a\r\nb\r\nc\nd\r\n");
    Rig rig(files, capture, files.folder);
    rig.edit("mixed.txt");
    TEST_CHECK(rig.bar().notice.contains("CRLF"));
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-mixed.png");
    rig.type("X");
    rig.saveKeys();
    TEST_CHECK(files.read("mixed.txt") == "Xa\r\nb\r\nc\r\nd\r\n");
    TEST_CHECK(rig.bar().notice.empty());
  }

  // Each reason a file cannot be edited, as the disabled button's tooltip.
  void testDisabledReasons(::test::SceneCapture& capture) {
    const Files files;
    std::string big(luciole::editing::kEditByteLimit + 10, 'a');
    for (std::size_t at = 80; at < big.size(); at += 80) {
      big[at] = '\n';
    }
    files.write("big.txt", big);
    files.write("latin.txt", "caf\xE9\n");
    files.write("long.txt", "short\n" + std::string((16 * 1024) + 1, 'x') + "\n");
    files.write("secret.txt", "-----BEGIN PRIVATE KEY-----\nMIIB\n-----END PRIVATE KEY-----\n");
    files.write("locked.txt", "text\n", 0444);
    const auto first = files.write("linked.txt", "text\n");
    fs::create_hard_link(first, files.folder / "other-name.txt");
    files.write("fine.txt", "text\n");
    Rig rig(files, capture, files.folder);
    const auto reason = [&](std::string_view name, std::string_view words) {
      rig.visit(name);
      TEST_CHECK(!rig.bar().enableAvailable && !rig.slot().enableEditingButton().enabled());
      TEST_CHECK(rig.bar().reason.contains(words));
      if (!rig.bar().reason.contains(words)) {
        std::println("{}: {}", name, rig.bar().reason);
      }
      rig.click(rig.slot().enableEditingButton());
      TEST_CHECK(!rig.editor->gate().editing() && rig.codeEditor() == nullptr);
    };
    reason("big.txt", "larger than 1 MiB");
    reason("latin.txt", "not UTF-8");
    reason("long.txt", "longer than 16 KiB");
    reason("secret.txt", "private key");
    reason("locked.txt", "read-only");
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-disabled.png");
    reason("linked.txt", "hard links");
    rig.visit("fine.txt");
    TEST_CHECK(rig.bar().enableAvailable && rig.bar().reason.empty());
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-offered.png");
  }

  void testReadOnlyFolderReason(::test::SceneCapture& capture) {
    const Files files;
    files.write("shut/inside.txt", "text\n");
    TEST_CHECK(::chmod((files.folder / "shut").c_str(), 0555) == 0);
    {
      Rig rig(files, capture, files.folder / "shut");
      rig.visit("inside.txt");
      TEST_CHECK(!rig.bar().enableAvailable && rig.bar().reason.contains("folder"));
    }
    TEST_CHECK(::chmod((files.folder / "shut").c_str(), 0755) == 0);
  }

  // Saves the save layer refuses: the sentence shows in the bar and the text is kept for another try.
  void testRefusedSaves(::test::SceneCapture& capture) {
    const Files files;
    files.write("box/a.txt", "alpha\n");
    files.write("box/b.txt", "beta\n");
    files.write("box/c.txt", "gamma\n");
    const auto box = files.folder / "box";
    Rig rig(files, capture, box);

    rig.edit("a.txt");
    rig.type("1");
    TEST_CHECK(::chmod(box.c_str(), 0555) == 0);
    rig.saveKeys();
    TEST_CHECK(::chmod(box.c_str(), 0755) == 0);
    TEST_CHECK(rig.bar().notice.contains("read-only") && rig.bar().unsaved && rig.editor->unsaved());
    TEST_CHECK(files.read("box/a.txt") == "alpha\n");
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-refused.png");
    rig.saveKeys(); // the folder is writable again: the kept text saves
    TEST_CHECK(files.read("box/a.txt") == "1alpha\n" && !rig.editor->unsaved());

    rig.type("2");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    files.write("box/a.txt", "written elsewhere\n");
    rig.frames([&] { return rig.bar().notice.contains("changed on disk"); }); // seen by the listing
    rig.saveKeys();
    TEST_CHECK(rig.bar().notice.contains("changed on disk") && rig.editor->unsaved());
    TEST_CHECK(files.read("box/a.txt") == "written elsewhere\n");
    rig.key(XKB_KEY_Escape);
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerNo();
    rig.frames([&] { return rig.offered(); });

    rig.edit("b.txt");
    rig.type("3");
    fs::create_hard_link(box / "b.txt", box / "b-link.txt");
    rig.saveKeys();
    TEST_CHECK(rig.bar().notice.contains("hard links") && rig.editor->unsaved());
    TEST_CHECK(files.read("box/b.txt") == "beta\n");
  }

  // The editor's text follows the size control: a larger step makes every line taller, a smaller one shorter.
  void testTextSizeFollowsSizeControl(::test::SceneCapture& capture) {
    const Files files;
    std::string lines;
    for (int i = 0; i < 300; ++i) {
      lines += "line " + std::to_string(i) + "\n";
    }
    files.write("long.txt", lines);
    Rig rig(files, capture, files.folder);
    rig.edit("long.txt");
    const float base = rig.codeEditor()->maxScrollY();
    TEST_CHECK(base > 0);
    rig.preview->setPreviewSize(200);
    rig.frames([&] { return rig.codeEditor()->maxScrollY() > base * 1.5F && rig.codeEditor()->settled(); });
    const float large = rig.codeEditor()->maxScrollY();
    rig.preview->setPreviewSize(70);
    rig.frames([&] { return rig.codeEditor()->maxScrollY() < base * 0.9F && rig.codeEditor()->settled(); });
    TEST_CHECK(rig.codeEditor()->maxScrollY() < base && base < large);
    TEST_CHECK(!rig.editor->unsaved() && rig.asks == 0); // the size is no edit
  }
} // namespace

int main() {
  if (::geteuid() == 0) {
    return 77; // root writes read-only files and folders, so those refusals cannot be shown
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  testSavedBytes(*capture);
  testSaveButtonAndOwnSave(*capture);
  testUnchangedNeverWritten(*capture);
  testMixedEndings(*capture);
  testDisabledReasons(*capture);
  testReadOnlyFolderReason(*capture);
  testRefusedSaves(*capture);
  testTextSizeFollowsSizeControl(*capture);
  std::println("preview-edit: all checks passed");
  return 0;
}
