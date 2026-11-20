#include "workbench_place_rig.h"

#include <print>
#include <string>

// A preview that has no column to show in takes the place of Jump ahead, or is not shown, as the setting says, and the
// "always" choice leaves the list where it was.
namespace {
  using namespace luciole::test::place;

  struct ListShape {
    float x = 0;
    float width = 0;
    std::size_t columns = 0;
    bool operator==(const ListShape&) const = default;
  };

  ListShape listShape(Rig& rig) {
    const auto* list = rig.view().center().list();
    TEST_CHECK(list != nullptr);
    float x = 0;
    float y = 0;
    Node::absolutePosition(list, x, y);
    return {.x = x, .width = list->width(), .columns = rig.view().center().plan().columns.size()};
  }

  // The preview in Jump ahead leaves the slot a column would have used blank: no preview, header or divider of it
  // is left in the center.
  void theFilesSlotIsBlank(Rig& rig) {
    auto& center = rig.view().center();
    const auto* quiet = dynamic_cast<const luciole::QuietColumnView*>(center.column(0));
    TEST_CHECK(quiet != nullptr && quiet->blank());
    TEST_CHECK(!inside(rig.slot(), center) && rig.slot().parent() != nullptr);
    TEST_CHECK(!center.divider(0).visible());
  }

  // Under "always" the plan depends on the room alone: a file, a folder and a file again leave the list and the
  // planned columns where they were.
  void alwaysKeepsTheListStill(Rig& rig) {
    rig.choice = WorkbenchPreviewPlace::JumpAheadAlways;
    for (const auto size : {kRoomy, kWide}) {
      rig.size = size;
      rig.select("notes.md");
      rig.settle();
      rig.showsFile("notes.md");
      const auto first = listShape(rig);
      theFilesSlotIsBlank(rig);
      rig.select("folder");
      rig.settle();
      TEST_CHECK(rig.view().previewHome() == PreviewHome::None);
      TEST_CHECK(listShape(rig) == first);
      rig.select("notes.md");
      rig.settle();
      rig.showsFile("notes.md");
      TEST_CHECK(listShape(rig) == first);
      theFilesSlotIsBlank(rig);
    }
  }

  void jumpAheadHostsThePreview(Rig& rig) {
    rig.select("notes.md");
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    TEST_CHECK(rig.view().center().plan().columns.empty());
    TEST_CHECK(rig.session->shows(rig.slot()));
    TEST_CHECK(inside(rig.slot(), *rig.view().shell().content(luciole::PaneRegion::Jump)));
    TEST_CHECK(!rig.shownJump());
    TEST_CHECK(rig.jumpTitle() == "Preview");
    // the pane is the left column's: the slot ends at the divider and fills the pane below its header
    TEST_CHECK(rightEdge(rig.slot()) <= 341.0F);
    TEST_CHECK(rig.slot().width() > 200 && rig.slot().height() > 40);
    // the preview got the file, from the list's selection, as a column would
    TEST_CHECK(rig.preview->kind() == luciole::ColumnPreviewKind::Text);
    TEST_CHECK(rig.slot().factsLabel().text().contains("200 bytes"));
  }

  void switchingTheSettingMovesThePreview(Rig& rig) {
    rig.choice = WorkbenchPreviewPlace::HiddenWhenNarrow;
    rig.settle();
    TEST_CHECK(rig.view().previewHome() == PreviewHome::None);
    TEST_CHECK(!rig.session->shows(rig.slot()));
    TEST_CHECK(rig.preview->load() == nullptr); // a hidden slot holds nothing
    TEST_CHECK(rig.shownJump());
    TEST_CHECK(rig.jumpTitle() == "Jump ahead");
    TEST_CHECK(rig.slot().parent() == nullptr);

    rig.choice = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    TEST_CHECK(!rig.shownJump() && rig.jumpTitle() == "Preview");
  }

  void jumpAheadStaysWithoutATarget(Rig& rig) {
    rig.select("folder");
    rig.settle();
    TEST_CHECK(!rig.session->lookahead().preview());
    TEST_CHECK(rig.view().previewHome() == PreviewHome::None);
    TEST_CHECK(rig.shownJump() && rig.jumpTitle() == "Jump ahead");
    TEST_CHECK(!rig.session->shows(rig.slot()));
    rig.select("notes.md");
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
  }

  void aClosedJumpAheadShowsNoPreview(Rig& rig) {
    auto& shell = rig.view().shell();
    shell.setState(wb::togglePane(shell.state(), wb::Pane::Jump));
    rig.settle();
    TEST_CHECK(rig.view().previewHome() == PreviewHome::None);
    TEST_CHECK(!rig.session->shows(rig.slot()));
    TEST_CHECK(rig.preview->load() == nullptr);
    shell.setState(wb::togglePane(shell.state(), wb::Pane::Jump));
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
  }

  void aRoomyViewKeepsTheColumn(Rig& rig) {
    rig.size = kRoomy;
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(!rig.view().center().plan().columns.empty());
    TEST_CHECK(rig.view().previewHome() == PreviewHome::Column);
    TEST_CHECK(inside(rig.slot(), rig.view().center()));
    TEST_CHECK(rig.shownJump() && rig.jumpTitle() == "Jump ahead");
    for (const auto choice : {WorkbenchPreviewPlace::HiddenWhenNarrow, WorkbenchPreviewPlace::JumpAheadWhenNarrow}) {
      rig.choice = choice;
      rig.settle();
      TEST_CHECK(rig.view().previewHome() == PreviewHome::Column); // the narrow-case choices have no effect
    }
    // and back to the small view: the preview leaves the column for Jump ahead
    rig.size = kSmall;
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
  }

  void alwaysReplacesJumpAheadAtEverySize(Rig& rig) {
    rig.size = kRoomy;
    rig.choice = WorkbenchPreviewPlace::JumpAheadAlways;
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    TEST_CHECK(inside(rig.slot(), *rig.view().shell().content(luciole::PaneRegion::Jump)));
    TEST_CHECK(!inside(rig.slot(), rig.view().center()));
    TEST_CHECK(!rig.shownJump() && rig.jumpTitle() == "Preview");
    TEST_CHECK(rig.slot().width() > 200 && rig.slot().height() > 40);

    // the same at a small size and a wide one
    for (const auto size : {kSmall, kWide}) {
      rig.size = size;
      rig.settle();
      rig.showsFile("notes.md");
      TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    }
    // the other choices at the wide size keep the column, which runs to the edge
    for (const auto choice : {WorkbenchPreviewPlace::JumpAheadWhenNarrow, WorkbenchPreviewPlace::HiddenWhenNarrow}) {
      rig.choice = choice;
      rig.settle();
      rig.showsFile("notes.md");
      TEST_CHECK(rig.view().previewHome() == PreviewHome::Column);
      TEST_CHECK(rig.view().center().plan().columns.size() == 3);
      TEST_CHECK(inside(rig.slot(), rig.view().center()));
      TEST_CHECK(rightEdge(rig.slot()) >= rightEdge(rig.view().center()) - 1.5F);
    }
    rig.choice = WorkbenchPreviewPlace::JumpAheadAlways;
    rig.settle();
    rig.showsFile("notes.md");

    // no target: Jump ahead stays
    rig.select("folder");
    rig.settle();
    TEST_CHECK(rig.view().previewHome() == PreviewHome::None);
    TEST_CHECK(rig.shownJump() && rig.jumpTitle() == "Jump ahead");
    rig.select("notes.md");
    rig.settle();
    rig.showsFile("notes.md");

    // switching live at the roomy size moves the preview into its column and back, or hides it
    rig.size = kRoomy;
    rig.settle();
    rig.choice = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::Column);
    TEST_CHECK(inside(rig.slot(), rig.view().center()));
    TEST_CHECK(rig.shownJump() && rig.jumpTitle() == "Jump ahead");
    rig.choice = WorkbenchPreviewPlace::JumpAheadAlways;
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    rig.choice = WorkbenchPreviewPlace::HiddenWhenNarrow;
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::Column);

    // with Jump ahead collapsed the "always" choice shows no preview, though a column has room
    rig.choice = WorkbenchPreviewPlace::JumpAheadAlways;
    auto& shell = rig.view().shell();
    shell.setState(wb::togglePane(shell.state(), wb::Pane::Jump));
    rig.settle();
    TEST_CHECK(rig.view().previewHome() == PreviewHome::None);
    TEST_CHECK(!rig.session->shows(rig.slot()));
    TEST_CHECK(rig.preview->load() == nullptr);
    shell.setState(wb::togglePane(shell.state(), wb::Pane::Jump));
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  static const luciole::test::preview::PreviewFiles files;
  test::audio::writeSoundClip(files.folder() / "song.wav", test::audio::SoundClip::ToneAndKicks, 30);
  {
    Rig rig(files, *capture);
    jumpAheadHostsThePreview(rig);
    switchingTheSettingMovesThePreview(rig);
    jumpAheadStaysWithoutATarget(rig);
    aClosedJumpAheadShowsNoPreview(rig);
    aRoomyViewKeepsTheColumn(rig);
    alwaysReplacesJumpAheadAtEverySize(rig);
    alwaysKeepsTheListStill(rig);
  }
  std::println("workbench-preview-place: all checks passed");
  return 0;
}
