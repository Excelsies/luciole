#include "preview_edit_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/code_editor_view.h"
#include "ui/controls/text_tile_view.h"
#include "ui/style.h"

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
  const Folder folder;
  Rig rig(folder.files, *capture, folder.files.folder);
  shortTextHugsItsLines(rig);
  emptyFileShowsItsWordsOnAtLeastOneLine(rig);
  longTextFillsThePane(rig);
  editingFillsThePaneAndStoppingHugsAgain(rig);
  std::println("preview-edit-card: all checks passed");
  return 0;
}
