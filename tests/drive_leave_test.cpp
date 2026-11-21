#include "app/drive_leaving.h"
#include "app/page/preview_edit_question.h"
#include "preview_edit_rig.h"
#include "render/scene/input_dispatcher.h"
#include "ui/dialogs/dialog_chrome.h"

#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

// Removing a drive asks about unsaved edits of its files first, while a Save can still reach it: real editors in
// the column preview over a real folder that stands for the drive, and the gate that asks them in turn.
namespace {
  using namespace luciole::test::edit;
  using luciole::DriveEditor;
  using luciole::Location;

  struct Drive {
    Files files;
    Location root = Location::fromPath(files.folder.native());
    Drive() {
      files.write("a.txt", "alpha\n");
      files.write("b.txt", "beta\n");
    }
  };

  // The close button in the question's header.
  const Node* closeButtonOf(const Node& node) {
    if (const auto* area = dynamic_cast<const InputArea*>(&node); area != nullptr && luciole::isDialogClose(area)) {
      return area;
    }
    for (const auto& child : node.children()) {
      if (const auto* found = closeButtonOf(*child)) {
        return found;
      }
    }
    return nullptr;
  }

  DriveEditor editorOf(Rig& rig) {
    return {
        .unsavedIn = [&rig](const Location& root) { return rig.editor->unsavedIn(root); },
        .ask = [&rig](std::function<void()> resume) { return rig.editor->leave(std::move(resume), true); }
    };
  }

  // What the eject does once the questions are answered: it reads the drive's file as the unmount would find it.
  struct Eject {
    explicit Eject(const Drive& mount) : drive(mount) {}
    const Drive& drive;
    luciole::CloseGate gate;
    int leaves = 0;
    int abandoned = 0;
    std::vector<luciole::CloseGate::Abandon> reasons;
    std::string aOnDisk;

    void ask(std::vector<DriveEditor> pages) {
      luciole::leaveDriveThen(
          gate, drive.root, std::move(pages),
          [this] {
            ++leaves;
            aOnDisk = drive.files.read("a.txt");
          },
          [this](luciole::CloseGate::Abandon reason) {
            ++abandoned;
            reasons.push_back(reason);
          }
      );
    }
  };

  void saveAnswersBeforeTheDriveIsReleased(::test::SceneCapture& capture) {
    const Drive drive;
    Rig rig(drive.files, capture, drive.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    Eject eject(drive);
    eject.ask({editorOf(rig)});
    TEST_CHECK(eject.leaves == 0);
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(rig.asks == 1 && rig.question->name == "a.txt" && eject.leaves == 0);
    rig.answerSave();
    TEST_CHECK(eject.leaves == 1 && eject.abandoned == 0);
    TEST_CHECK(eject.aOnDisk == "1alpha\n"); // the bytes were on disk before the drive was released
  }

  void noDropsTheChangesAndTheEjectGoesOn(::test::SceneCapture& capture) {
    const Drive drive;
    Rig rig(drive.files, capture, drive.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    Eject eject(drive);
    eject.ask({editorOf(rig)});
    rig.frames([&] { return rig.question.has_value(); });
    rig.chooseNo();
    TEST_CHECK(eject.leaves == 1 && eject.abandoned == 0 && eject.aOnDisk == "alpha\n");
    TEST_CHECK(!rig.editor->unsaved());
  }

  void aFailingSaveKeepsTheDriveMounted(::test::SceneCapture& capture) {
    const Drive drive;
    Rig rig(drive.files, capture, drive.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    Eject eject(drive);
    eject.ask({editorOf(rig)});
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(::chmod(drive.files.folder.c_str(), 0555) == 0);
    rig.answerSave();
    TEST_CHECK(::chmod(drive.files.folder.c_str(), 0755) == 0);
    TEST_CHECK(rig.question.has_value() && rig.editor->unsaved()); // still asking, with the reason
    TEST_CHECK(eject.leaves == 0 && eject.abandoned == 0);
    TEST_CHECK(drive.files.read("a.txt") == "alpha\n");
    rig.chooseNo(); // giving up on the changes lets the eject go on
    TEST_CHECK(eject.leaves == 1);
  }

  void anEditOutsideTheDriveIsNotAskedAbout(::test::SceneCapture& capture) {
    const Drive drive;
    const Drive elsewhere;
    Rig outside(elsewhere.files, capture, elsewhere.files.folder);
    outside.edit("a.txt");
    outside.type("1");
    Eject eject(drive);
    eject.ask({editorOf(outside)});
    TEST_CHECK(eject.leaves == 1 && outside.asks == 0 && !outside.question);
    TEST_CHECK(outside.editor->unsaved());
  }

  void twoPagesAreAskedInTurn(::test::SceneCapture& capture) {
    const Drive drive;
    Rig first(drive.files, capture, drive.files.folder);
    Rig second(drive.files, capture, drive.files.folder);
    first.edit("a.txt");
    first.type("1");
    second.edit("b.txt");
    second.type("2");
    Eject eject(drive);
    eject.ask({editorOf(first), editorOf(second)});
    first.frames([&] { return first.question.has_value(); });
    TEST_CHECK(first.asks == 1 && second.asks == 0 && !second.question);
    first.answerSave();
    TEST_CHECK(eject.leaves == 0);
    second.frames([&] { return second.question.has_value(); });
    TEST_CHECK(second.asks == 1);
    second.chooseNo();
    TEST_CHECK(eject.leaves == 1 && eject.aOnDisk == "1alpha\n" && drive.files.read("b.txt") == "beta\n");
  }

  // Escape or the close button chooses nothing: the changes stay in the editor, the drive is not released and a
  // second removal asks again; only No drops them.
  void aDismissedQuestionGivesTheRemovalUp(::test::SceneCapture& capture) {
    const Drive drive;
    Rig rig(drive.files, capture, drive.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    Eject eject(drive);
    eject.ask({editorOf(rig)});
    rig.frames([&] { return rig.question.has_value(); });
    rig.dismiss();
    rig.frames([&] { return eject.abandoned == 1; });
    TEST_CHECK(eject.leaves == 0 && eject.reasons.front() == luciole::CloseGate::Abandon::Kept);
    TEST_CHECK(rig.editor->gate().editing() && rig.editor->unsaved() && !rig.question);
    TEST_CHECK(drive.files.read("a.txt") == "alpha\n");
    eject.ask({editorOf(rig)});
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(rig.asks == 2 && eject.leaves == 0);
    rig.chooseNo();
    TEST_CHECK(eject.leaves == 1 && !rig.editor->unsaved() && drive.files.read("a.txt") == "alpha\n");
  }

  // A removal asked while another question waits says so instead of doing nothing.
  void aRemovalWhileAnotherQuestionWaitsIsTurnedAway(::test::SceneCapture& capture) {
    const Drive drive;
    Rig rig(drive.files, capture, drive.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    Eject first(drive);
    first.ask({editorOf(rig)});
    rig.frames([&] { return rig.question.has_value(); });
    Eject second(drive);
    luciole::leaveDriveThen(
        first.gate, drive.root, {editorOf(rig)}, [&second] { ++second.leaves; },
        [&second](luciole::CloseGate::Abandon reason) {
          ++second.abandoned;
          second.reasons.push_back(reason);
        }
    );
    TEST_CHECK(second.abandoned == 1 && second.reasons.front() == luciole::CloseGate::Abandon::Busy);
    TEST_CHECK(second.leaves == 0 && rig.asks == 1);
    rig.chooseNo();
    TEST_CHECK(first.leaves == 1);
  }

  // A page that goes away while its question is open counts as answered: the removal goes on with the other pages
  // and the gate takes the next request.
  void aPageThatVanishesDuringItsQuestionCountsAsAnswered(::test::SceneCapture& capture) {
    const Drive drive;
    auto gone = std::make_unique<Rig>(drive.files, capture, drive.files.folder);
    Rig other(drive.files, capture, drive.files.folder);
    gone->edit("a.txt");
    gone->type("1");
    other.edit("b.txt");
    other.type("2");
    Rig* pages[2] = {gone.get(), &other};
    const auto present = [&pages](int index) { return pages[index] != nullptr; };
    const auto editorAt = [&](int index) {
      return DriveEditor{
          .unsavedIn =
              [&, index](const Location& root) { return present(index) && pages[index]->editor->unsavedIn(root); },
          .ask = [&, index](
                     std::function<void()> resume
                 ) { return !present(index) || pages[index]->editor->leave(std::move(resume), true); }
      };
    };
    Eject eject(drive);
    eject.ask({editorAt(0), editorAt(1)});
    gone->frames([&] { return gone->question.has_value(); });
    pages[0] = nullptr;
    gone.reset(); // the page closes with its question open
    other.frames([&] { return other.question.has_value(); });
    TEST_CHECK(eject.leaves == 0 && other.asks == 1); // the next page is asked
    other.chooseNo();
    TEST_CHECK(eject.leaves == 1 && eject.abandoned == 0);
    // The gate takes the next request: a second removal asks about a fresh edit instead of being turned away.
    other.edit("b.txt");
    other.type("3");
    Eject again(drive);
    again.ask({editorAt(1)});
    other.frames([&] { return other.question.has_value(); });
    TEST_CHECK(again.abandoned == 0 && other.asks == 2);
    other.chooseNo();
    TEST_CHECK(again.leaves == 1);
  }

  // The mode of one question ends with it: after a removal's question was answered, a question the editor opens
  // itself, by moving to another file, is answered No when it is dismissed.
  void aRemovalsModeDoesNotOutliveItsQuestion(::test::SceneCapture& capture) {
    const Drive drive;
    Rig rig(drive.files, capture, drive.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    Eject eject(drive);
    eject.ask({editorOf(rig)});
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerSave();
    TEST_CHECK(eject.leaves == 1);
    rig.frames([&] { return rig.offered(); });
    rig.edit("a.txt");
    rig.type("2");
    rig.select("b.txt");
    rig.frames([&] { return rig.question.has_value(); });
    rig.dismiss();
    TEST_CHECK(!rig.editor->gate().editing() && !rig.editor->unsaved());
    TEST_CHECK(drive.files.read("a.txt") == "1alpha\n");
  }

  // A dismissal cancels only when every request waiting on the question is a removal's. With the page's own question
  // or a window or tab leave on it, in either order, it means No for all of them and the removal goes on.
  void aDismissalCancelsOnlyWhenEveryWaitingRequestDoes(::test::SceneCapture& capture) {
    {
      const Drive drive;
      Rig rig(drive.files, capture, drive.files.folder);
      rig.edit("a.txt");
      rig.type("1");
      rig.select("b.txt"); // the editor's own question
      rig.frames([&] { return rig.question.has_value(); });
      Eject eject(drive);
      eject.ask({editorOf(rig)});
      rig.dismiss();
      rig.frames([&] { return eject.leaves == 1; });
      TEST_CHECK(!rig.editor->unsaved() && eject.abandoned == 0 && drive.files.read("a.txt") == "alpha\n");
    }
    for (const bool windowFirst : {true, false}) {
      const Drive drive;
      Rig rig(drive.files, capture, drive.files.folder);
      rig.edit("a.txt");
      rig.type("1");
      int closed = 0;
      Eject eject(drive);
      const auto window = [&] { return rig.editor->leave([&closed] { ++closed; }, false); };
      if (windowFirst) {
        TEST_CHECK(!window());
        eject.ask({editorOf(rig)});
      } else {
        eject.ask({editorOf(rig)});
        TEST_CHECK(!window());
      }
      rig.frames([&] { return rig.question.has_value(); });
      rig.dismiss();
      rig.frames([&] { return closed == 1 && eject.leaves == 1; });
      TEST_CHECK(!rig.editor->unsaved() && eject.abandoned == 0 && drive.files.read("a.txt") == "alpha\n");
    }
  }

  // Escape while the Save of the question is writing is no answer: the question comes back, and the removal goes on
  // when the save succeeds or stays given up while it fails.
  void aDismissalDuringTheSaveChangesNothing(::test::SceneCapture& capture) {
    for (const bool succeeds : {true, false}) {
      const Drive drive;
      Rig rig(drive.files, capture, drive.files.folder);
      rig.edit("a.txt");
      rig.type("1");
      Eject eject(drive);
      eject.ask({editorOf(rig)});
      rig.frames([&] { return rig.question.has_value(); });
      if (!succeeds) {
        TEST_CHECK(::chmod(drive.files.folder.c_str(), 0555) == 0);
      }
      auto shown = *rig.question;
      shown.save();
      shown.closed(); // Escape, with the save running
      rig.frames([&] { return !rig.editor->saving(); });
      if (!succeeds) {
        TEST_CHECK(::chmod(drive.files.folder.c_str(), 0755) == 0);
      }
      rig.frames([&] { return succeeds ? eject.leaves == 1 : rig.question.has_value(); });
      TEST_CHECK(eject.abandoned == 0);
      TEST_CHECK(succeeds ? drive.files.read("a.txt") == "1alpha\n" : drive.files.read("a.txt") == "alpha\n");
      TEST_CHECK(succeeds || (eject.leaves == 0 && rig.editor->unsaved()));
      if (!succeeds) {
        rig.chooseNo();
      }
    }
  }

  // The buttons of the page's own question: the close button chooses nothing and No chooses No.
  void thePageQuestionsButtonsMeanWhatTheyDo(::test::SceneCapture& capture) {
    const Drive drive;
    Rig rig(drive.files, capture, drive.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    Eject eject(drive);
    for (const bool closeButton : {true, false}) {
      eject.ask({editorOf(rig)});
      rig.frames([&] { return rig.question.has_value(); });
      auto shown = *rig.question;
      auto parts = luciole::pageSaveQuestion(shown.name, shown.save, shown.no, [&rig] {
        if (auto closed = std::exchange(rig.question, std::nullopt)) {
          closed->closed();
        }
      });
      InputDispatcher dispatcher;
      dispatcher.setSceneRoot(parts.root.get());
      (void)capture.capture(*parts.root, 408, 200, 1);
      std::uint32_t serial = 0;
      const Node* target = closeButton ? closeButtonOf(*parts.root) : parts.no;
      TEST_CHECK(target != nullptr);
      float x = 0;
      float y = 0;
      Node::absolutePosition(target, x, y);
      x += target->width() / 2;
      y += target->height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 0, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 0, false);
      rig.frames([&] { return !closeButton || eject.abandoned == 1; });
      if (closeButton) {
        TEST_CHECK(eject.leaves == 0 && rig.editor->unsaved());
      } else {
        rig.frames([&] { return eject.leaves == 1; });
        TEST_CHECK(!rig.editor->unsaved());
      }
    }
  }

  // A save that runs when the drive is asked to go keeps it busy until the write has ended, and only for the files
  // of that drive.
  void aRunningSaveHoldsTheDrive(::test::SceneCapture& capture) {
    const Drive drive;
    const Drive elsewhere;
    Rig rig(drive.files, capture, drive.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    TEST_CHECK(rig.key('s', KeyMod::Ctrl));
    TEST_CHECK(rig.editor->saving() && rig.editor->savingIn(drive.root));
    TEST_CHECK(!rig.editor->savingIn(elsewhere.root));
    rig.frames([&] { return !rig.editor->saving(); });
    TEST_CHECK(!rig.editor->savingIn(drive.root) && drive.files.read("a.txt") == "1alpha\n");
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
  saveAnswersBeforeTheDriveIsReleased(*capture);
  noDropsTheChangesAndTheEjectGoesOn(*capture);
  aFailingSaveKeepsTheDriveMounted(*capture);
  anEditOutsideTheDriveIsNotAskedAbout(*capture);
  twoPagesAreAskedInTurn(*capture);
  aDismissedQuestionGivesTheRemovalUp(*capture);
  aRemovalWhileAnotherQuestionWaitsIsTurnedAway(*capture);
  aPageThatVanishesDuringItsQuestionCountsAsAnswered(*capture);
  aRemovalsModeDoesNotOutliveItsQuestion(*capture);
  aDismissalCancelsOnlyWhenEveryWaitingRequestDoes(*capture);
  aDismissalDuringTheSaveChangesNothing(*capture);
  thePageQuestionsButtonsMeanWhatTheyDo(*capture);
  aRunningSaveHoldsTheDrive(*capture);
  std::println("drive-leave: all checks passed");
  return 0;
}
