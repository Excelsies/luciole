#include "preview_edit_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/code_editor_view.h"
#include "ui/controls/text_tile_view.h"
#include "ui/style.h"

#include <cstdlib>
#include <format>
#include <print>
#include <string>

// The page card a text preview sits on: as tall as its text while it is read, as tall as the pane while it is
// edited, and back again when the editing stops.
namespace {
  using luciole::test::edit::Files;
  using luciole::test::edit::Rig;

  struct Folder {
    Files files;
    Folder() {
      files.write("note.txt", "Hello from the short note.\nSecond line here.\nThird line is the last one.\n");
      files.write("empty.txt", "");
      std::string many;
      for (int line = 0; line < 3000; ++line) {
        many += std::format("line {}\n", line);
      }
      files.write("many.txt", many);
    }
  };

  // The card around the content of the slot, and the room the pane gives it.
  struct Card {
    const Node* card = nullptr;
    float room = 0;
  };

  Card cardOf(const Node* content) {
    TEST_CHECK(content != nullptr && content->parent() != nullptr && content->parent()->parent() != nullptr);
    const Node* card = content->parent();
    return {.card = card, .room = card->parent()->height() - (2 * Style::spaceSm)};
  }

  // The window with the note read, then edited, written for looking at into the folder named by LUCIOLE_CAPTURE_DIR
  // as <name>-<LUCIOLE_SHOT_SET, "after" by default>.png.
  void keepNote(Rig& rig) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const char* set = std::getenv("LUCIOLE_SHOT_SET");
    const std::string suffix = std::format("-{}.png", set != nullptr && set[0] != '\0' ? set : "after");
    rig.visit("note.txt");
    luciole::test::edit::keepForViewing(rig.pixels, "workbench-note-reading" + suffix);
    rig.click(rig.slot().enableEditingButton());
    rig.frames([&] { return rig.codeEditor() != nullptr && rig.codeEditor()->settled(); });
    luciole::test::edit::keepForViewing(rig.pixels, "workbench-note-editing" + suffix);
    rig.click(rig.slot().stopEditingButton());
    rig.frames([&] { return rig.offered(); });
  }

  void shortTextHugsItsLines(Rig& rig) {
    rig.visit("note.txt");
    const auto [card, room] = cardOf(rig.tiles());
    const float line = rig.tiles()->textSize();
    std::println("note: card {} of {} high, text size {}", card->height(), room, line);
    // Three lines, the card's padding above and below, and well under what the pane offers.
    TEST_CHECK(card->height() > 3 * line);
    TEST_CHECK(card->height() < 8 * line);
    TEST_CHECK(card->height() < room * 0.5F);
    TEST_CHECK(card->width() > 0 && card->height() > 0);
  }

  // A larger text size makes the card taller once the text is laid out again, and back to 100% shrinks it.
  void theCardFollowsTheTextSize(Rig& rig) {
    rig.visit("note.txt");
    const float before = cardOf(rig.tiles()).card->height();
    rig.preview->setPreviewSize(200);
    rig.frames([&] {
      return rig.tiles() != nullptr && rig.tiles()->settled() && cardOf(rig.tiles()).card->height() > before * 1.5F;
    });
    rig.preview->setPreviewSize(100);
    rig.frames([&] {
      return rig.tiles() != nullptr && rig.tiles()->settled() && cardOf(rig.tiles()).card->height() < before * 1.1F;
    });
    TEST_NEAR(cardOf(rig.tiles()).card->height(), before, 0.5F);
  }

  void emptyFileShowsItsWordsOnAtLeastOneLine(Rig& rig) {
    rig.visit("empty.txt");
    const auto [card, room] = cardOf(rig.tiles());
    TEST_CHECK(rig.tiles()->message() == "Empty file");
    TEST_CHECK(card->height() >= rig.tiles()->textSize() + (2 * Style::spaceSm));
    TEST_CHECK(card->height() < room * 0.5F);
  }

  void longTextFillsThePane(Rig& rig) {
    rig.visit("many.txt");
    const auto [card, room] = cardOf(rig.tiles());
    TEST_CHECK(card->height() > room - 1.0F && card->height() < room + 0.5F);
  }

  // Editing takes the whole pane; stopping brings back the card of the text, the same height as before.
  void editingFillsThePaneAndStoppingHugsAgain(Rig& rig) {
    rig.visit("note.txt");
    const float before = cardOf(rig.tiles()).card->height();
    rig.click(rig.slot().enableEditingButton());
    rig.frames([&] { return rig.codeEditor() != nullptr && rig.codeEditor()->settled(); });
    const auto [editing, room] = cardOf(rig.codeEditor());
    TEST_CHECK(editing->height() > room - 1.0F && editing->height() < room + 0.5F);
    rig.click(rig.slot().stopEditingButton());
    rig.frames([&] { return rig.offered(); });
    const auto [after, afterRoom] = cardOf(rig.tiles());
    TEST_CHECK(after->height() < afterRoom * 0.5F);
    TEST_NEAR(after->height(), before, 0.5F);
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  const Folder folder;
  Rig rig(folder.files, *capture, folder.files.folder);
  keepNote(rig);
  shortTextHugsItsLines(rig);
  theCardFollowsTheTextSize(rig);
  emptyFileShowsItsWordsOnAtLeastOneLine(rig);
  longTextFillsThePane(rig);
  editingFillsThePaneAndStoppingHugsAgain(rig);
  std::println("preview-edit-card: all checks passed");
  return 0;
}
