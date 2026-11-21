#include "app/page/workbench_open.h"
#include "preview_edit_rig.h"

#include <string>
#include <unistd.h>
#include <vector>

// Opening a file of the Workbench: one ZIP shows the archive browser, as Open does in the list, and is not launched;
// every other file is launched as before; the visit is recorded either way. Through the real session, with the
// page's listing and a column ahead as the places a file comes from.
namespace {
  using namespace luciole::test::edit;
  using luciole::ArchiveFacts;
  using luciole::Location;

  constexpr std::string_view kEmptyZip{"PK\x05\x06\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 22};

  struct Folder {
    Files files;
    Folder() {
      files.write("pack.zip", kEmptyZip);
      files.write("note.txt", "hello\n");
      files.write("part.zip.002", "split");
      files.write("sub/inner.zip", kEmptyZip);
      files.write("late.zip", kEmptyZip);
    }
    Location at(const std::string& relative) const { return Location::fromPath((files.folder / relative).native()); }
  };

  struct Opened {
    std::vector<std::string> visited;
    std::vector<std::string> launched;
    std::vector<std::string> browsed;

    luciole::WorkbenchOpenHooks hooks() {
      return {
          .visited = [this](const Location& file) { visited.push_back(*file.basename()); },
          .launch = [this](const Location& file) { launched.push_back(*file.basename()); },
          .browseArchive = [this](const ArchiveFacts& archive) { browsed.push_back(*archive.location.basename()); }
      };
    }
  };

  std::vector<const luciole::DirectoryModel*> modelsOf(Rig& rig) {
    std::vector<const luciole::DirectoryModel*> models{&rig.browser.model()};
    for (std::size_t column = 0; column < rig.session->lookahead().count(); ++column) {
      models.push_back(&rig.session->lookahead().model(column));
    }
    return models;
  }

  void open(Rig& rig, const Folder& folder, const std::string& name, bool chooser, Opened& opened) {
    luciole::openWorkbenchFile(folder.at(name), modelsOf(rig), chooser, opened.hooks());
  }

  void aZipShowsTheArchiveBrowser(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    Opened opened;
    open(rig, folder, "pack.zip", false, opened);
    TEST_CHECK(opened.browsed == std::vector<std::string>{"pack.zip"} && opened.launched.empty());
    TEST_CHECK(opened.visited == std::vector<std::string>{"pack.zip"});
  }

  void otherFilesAreLaunched(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    Opened opened;
    open(rig, folder, "note.txt", false, opened);
    open(rig, folder, "part.zip.002", false, opened); // a later part of a split archive is no archive here
    open(rig, folder, "pack.zip", true, opened);      // a file chooser launches nothing but also browses nothing
    TEST_CHECK(opened.launched == (std::vector<std::string>{"note.txt", "part.zip.002", "pack.zip"}));
    TEST_CHECK(opened.browsed.empty());
    TEST_CHECK(opened.visited == opened.launched);
  }

  void aColumnAheadAndAShapeOpenTheSameWay(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.session->lookahead().follow({.target = luciole::WorkbenchEntry{.location = folder.at("sub"), .folder = true}});
    rig.frames([&] {
      return rig.session->lookahead().count() == 1
          && rig.session->lookahead().model(0).snapshot().status == luciole::DirectoryStatus::Ready;
    });
    Opened opened;
    open(rig, folder, "sub/inner.zip", false, opened); // a row of the column ahead
    open(rig, folder, "late.zip", false, opened);      // a file no listing holds, as a piece of the instrument is
    TEST_CHECK((opened.browsed == std::vector<std::string>{"inner.zip", "late.zip"}) && opened.launched.empty());
    TEST_CHECK(opened.visited.size() == 2);
  }

  void aMissingFileIsLaunched(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    Opened opened;
    open(rig, folder, "gone.zip", false, opened);
    TEST_CHECK(opened.launched == std::vector<std::string>{"gone.zip"} && opened.browsed.empty());
  }

  // The refusal is the one Browse Archive gives for a row.
  void runningJobsRefuseTheBrowser() {
    const Folder folder;
    const auto facts = luciole::archiveFactsFor(folder.at("pack.zip"), {});
    TEST_CHECK(luciole::ActionRegistry::browsableFile(facts));
    const auto busy = luciole::ActionRegistry::browseArchiveState(true, facts);
    TEST_CHECK(!busy.enabled && busy.reason == "Another file operation is still finishing.");
    TEST_CHECK(luciole::ActionRegistry::browseArchiveState(false, facts).enabled);
    const auto text = luciole::archiveFactsFor(folder.at("note.txt"), {});
    TEST_CHECK(luciole::ActionRegistry::browseArchiveState(false, text).reason == "Select one ZIP or TAR archive.");
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
  aZipShowsTheArchiveBrowser(*capture);
  otherFilesAreLaunched(*capture);
  aColumnAheadAndAShapeOpenTheSameWay(*capture);
  aMissingFileIsLaunched(*capture);
  runningJobsRefuseTheBrowser();
  std::println("workbench-open: all checks passed");
  return 0;
}
