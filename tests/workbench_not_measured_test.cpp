#include "browser/location_presentation.h"
#include "fs/gio_interop.h"
#include "instrument_rig.h"
#include "preview_edit_rig.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/ribbon_view.h"
#include "ui/workbench/workbench_view.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>

// A location that is not a local folder, such as the Trash or a share, is not measured: the Workbench starts no walk
// for it, says so plainly in the instrument, the ribbon and Jump ahead, and names the folder as the location is
// named. Through the real session, over the Trash of a private data folder and a location no network is needed for.
namespace {
  using namespace luciole::test::edit;
  using luciole::JumpNotice;
  using luciole::Location;
  using luciole::WorkbenchFeed;
  namespace fs = std::filesystem;

  constexpr const char* kNote = "Not measured here";

  struct Folder {
    Files files;
    Folder() { files.write("gone.txt", "bye\n"); }
  };

  // Frames with the page's feed until `done` holds, for at most ten seconds.
  void settle(Rig& rig, const WorkbenchFeed& feed, const std::function<bool()>& done) {
    const auto start = std::chrono::steady_clock::now();
    while (!done() && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
      Rig::pump();
      rig.animations.tick(16);
      rig.session->update(feed, rig.choices);
      (void)rig.capture.capture(rig.session->view(), rig.width, rig.height, 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(done());
  }

  bool measuringNothing(Rig& rig) {
    const auto status = rig.services.measureService().status();
    return status.queued + status.running == 0 && !rig.session->model().measuring();
  }

  // Frames until the snapshot of the location is shown, then checks what the panes say.
  void expectNotMeasured(Rig& rig, const WorkbenchFeed& feed, const std::string& name) {
    settle(rig, feed, [&] {
      const auto snapshot = rig.session->model().snapshot();
      return snapshot && snapshot->notMeasured && rig.session->view().jumpList().notice() == JumpNotice::NotMeasured;
    });
    const auto snapshot = rig.session->model().snapshot();
    TEST_CHECK(snapshot->tree.nodes[snapshot->tree.root].name == name);
    TEST_CHECK(snapshot->error && snapshot->error->message == "This location is not a local folder.");
    TEST_CHECK(measuringNothing(rig));
    // No walk reads the file system's root, nor anything else.
    TEST_CHECK(!rig.services.measureService().reading(Location::fromPath("/")));
    for (const auto* shapes :
         {&rig.session->view().instrument().shapeView(), &rig.session->view().ribbon().shapeView()}) {
      settle(rig, feed, [&] { return luciole::test::instrument::notesWritten(*shapes, kNote) > 0; });
      TEST_CHECK(luciole::test::instrument::notesWritten(*shapes, "Loading…") == 0);
    }
  }

  void theTrashIsNotMeasured(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    const auto path = folder.files.folder / "gone.txt";
    TEST_CHECK(g_file_trash(luciole::gioFile(Location::fromPath(path.native())).get(), nullptr, nullptr) != FALSE);
    // The trash holds the file in a local folder of its own, which stands for one volume's trash only.
    TEST_CHECK(fs::exists(folder.files.home / "Trash" / "files" / "gone.txt"));
    // The desktop trash service lists it as the page's browser does; the test feeds the session what it would.
    const WorkbenchFeed feed{
        .folder = Location::trash(),
        .cursor = luciole::WorkbenchEntry{.location = Location::fromUri("trash:///gone.txt"), .folder = false},
        .sort = {},
        .showHidden = false,
        .showBackups = true
    };
    rig.session->update(feed, rig.choices);
    expectNotMeasured(rig, feed, "Trash");
    TEST_CHECK(luciole::locationTitle(Location::trash()) == "Trash");
  }

  void aShareIsNotMeasuredEither(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    const auto share = Location::fromUri("sftp://example.invalid/data");
    WorkbenchFeed feed{.folder = share, .cursor = std::nullopt, .sort = {}, .showHidden = false, .showBackups = true};
    rig.session->update(feed, rig.choices);
    expectNotMeasured(rig, feed, "data");
    // Back in a local folder, it is measured again.
    settle(rig, luciole::workbenchFeed(rig.browser), [&] {
      const auto snapshot = rig.session->model().snapshot();
      return snapshot
          && !snapshot->notMeasured
          && snapshot->location == Location::fromPath(folder.files.folder.native())
          && rig.session->view().jumpList().notice() != JumpNotice::NotMeasured;
    });
  }
} // namespace

int main() {
  if (::geteuid() == 0) {
    return 77;
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  theTrashIsNotMeasured(*capture);
  aShareIsNotMeasuredEither(*capture);
  std::println("workbench-not-measured: all checks passed");
  return 0;
}
