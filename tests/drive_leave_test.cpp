#include "app/drive_leaving.h"
#include "preview_edit_rig.h"

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

  DriveEditor editorOf(Rig& rig) {
    return {
        .unsavedIn = [&rig](const Location& root) { return rig.editor->unsavedIn(root); },
        .ask = [&rig](std::function<void()> resume) { return rig.editor->leave(std::move(resume)); }
    };
  }

  // What the eject does once the questions are answered: it reads the drive's file as the unmount would find it.
  struct Eject {
    explicit Eject(const Drive& mount) : drive(mount) {}
    const Drive& drive;
    luciole::CloseGate gate;
    int leaves = 0;
    int abandoned = 0;
    std::string aOnDisk;

    void ask(std::vector<DriveEditor> pages) {
      luciole::leaveDriveThen(
          gate, drive.root, std::move(pages),
          [this] {
            ++leaves;
            aOnDisk = drive.files.read("a.txt");
          },
          [this] { ++abandoned; }
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
    rig.answerNo();
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
    rig.answerNo(); // giving up on the changes lets the eject go on
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
    second.answerNo();
    TEST_CHECK(eject.leaves == 1 && eject.aOnDisk == "1alpha\n" && drive.files.read("b.txt") == "beta\n");
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
  aRunningSaveHoldsTheDrive(*capture);
  std::println("drive-leave: all checks passed");
  return 0;
}
