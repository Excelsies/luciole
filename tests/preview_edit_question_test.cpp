#include "app/page/preview_edit_question.h"
#include "png_writer.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/dialogs/dialog_chrome.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>

// The "Save Changes to <name>?" question, laid out and drawn offscreen, answered through a real dispatcher.
namespace {
  constexpr int kWidth = 408;
  constexpr int kHeight = 200;

  struct Rig {
    ::test::SceneCapture& capture;
    int saves = 0;
    int noes = 0;
    int closes = 0;
    luciole::SaveQuestion question =
        luciole::saveQuestion("notes.txt", [this] { ++saves; }, [this] { ++noes; }, [this] { ++closes; });
    InputDispatcher dispatcher;
    std::uint32_t serial = 0;

    explicit Rig(::test::SceneCapture& rig) : capture(rig) {
      dispatcher.setSceneRoot(question.root.get());
      draw();
    }

    ::test::GlPixels draw() { return capture.capture(*question.root, kWidth, kHeight, 1); }

    void click(const Node& node) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&node, x, y);
      x += node.width() / 2;
      y += node.height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 0, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 0, false);
      draw();
    }
  };

  void keepForViewing(const ::test::GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // The close button in the dialog's header.
  const Node* closeButton(const Node& node) {
    if (const auto* area = dynamic_cast<const InputArea*>(&node); area != nullptr && luciole::isDialogClose(area)) {
      return area;
    }
    for (const auto& child : node.children()) {
      if (const auto* found = closeButton(*child)) {
        return found;
      }
    }
    return nullptr;
  }

  bool hasText(const Node& node, const std::string& text) {
    if (const auto* label = dynamic_cast<const Label*>(&node); label != nullptr && label->text() == text) {
      return true;
    }
    return std::ranges::any_of(node.children(), [&](const auto& child) { return hasText(*child, text); });
  }

  void testAsksWithSaveAndNo(::test::SceneCapture& capture) {
    Rig rig(capture);
    TEST_CHECK(hasText(*rig.question.root, "Save Changes to notes.txt?"));
    TEST_CHECK(rig.question.save->label()->text() == "Save" && rig.question.no->label()->text() == "No");
    TEST_CHECK(!rig.question.sentence->visible());
    keepForViewing(rig.draw(), "preview-edit-question.png");
    rig.click(*rig.question.save);
    TEST_CHECK(rig.saves == 1 && rig.noes == 0);
    rig.click(*rig.question.no);
    TEST_CHECK(rig.saves == 1 && rig.noes == 1);
    const auto* close = closeButton(*rig.question.root);
    TEST_CHECK(close != nullptr);
    if (close != nullptr) {
      rig.click(*close);
    }
    TEST_CHECK(rig.saves == 1 && rig.noes == 1 && rig.closes == 1); // the close button is no choice of No
  }

  void testFailedSaveSentence(::test::SceneCapture& capture) {
    Rig rig(capture);
    const auto top = [&] {
      float x = 0;
      float y = 0;
      Node::absolutePosition(rig.question.save, x, y);
      return y;
    };
    const float before = top();
    luciole::showQuestionSentence(rig.question, "The folder is read-only, so the file was not saved.");
    rig.draw();
    TEST_CHECK(rig.question.sentence->visible() && rig.question.sentence->height() > 0);
    TEST_CHECK(rig.question.sentence->text() == "The folder is read-only, so the file was not saved.");
    TEST_CHECK(top() > before); // the sentence lies between the question and the buttons
    keepForViewing(rig.draw(), "preview-edit-question-failed.png");
    luciole::showQuestionSentence(rig.question, "");
    rig.draw();
    TEST_CHECK(!rig.question.sentence->visible());
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
  testAsksWithSaveAndNo(*capture);
  testFailedSaveSentence(*capture);
  std::println("preview-edit-question: all checks passed");
  return 0;
}
