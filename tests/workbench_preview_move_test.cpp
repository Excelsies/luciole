#include "media/media_probe.h"
#include "workbench_place_rig.h"

#include <chrono>
#include <glib.h>
#include <print>
#include <string>
#include <utility>

// The preview changes home while the page lives: a resize, a drag of a divider, a hovered Jump ahead row. It is drawn
// in one place at every frame and keeps its load, its player and its place in the user's hands.
namespace {
  using namespace luciole::test::place;

  // Where the preview is drawn is a place it may be in: the Jump ahead pane while Jump ahead gives way to it, or a
  // column the last layout drew. Never both, and never a column the layout dropped.
  void drawnInAValidPlace(Rig& rig) {
    if (!rig.session->shows(rig.slot())) {
      return;
    }
    auto& center = rig.view().center();
    const bool inJump = inside(rig.slot(), *rig.view().shell().content(luciole::PaneRegion::Jump));
    bool inColumn = false;
    for (std::size_t column = 0; column < center.plan().columns.size(); ++column) {
      inColumn = inColumn || center.column(column) == &rig.slot();
    }
    TEST_CHECK(inJump != inColumn);
    TEST_CHECK(!inJump || !rig.shownJump());
  }

  // A resize puts the preview where the new layout says in the same frame, and every frame draws it in one place, a
  // column the layout drew or the Jump ahead pane.
  void aResizeNeverDrawsThePreviewInAStalePlace(Rig& rig) {
    rig.choice = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    rig.select("notes.md");
    for (const auto size : {kSmall, kRoomy, kSmall, kWide, kSmall}) {
      rig.size = size;
      const bool narrow = size.width == kSmall.width;
      for (int frame = 0; frame < 4; ++frame) {
        rig.frame();
        drawnInAValidPlace(rig);
        TEST_CHECK(rig.view().previewHome() == (narrow ? PreviewHome::JumpAhead : PreviewHome::Column));
        TEST_CHECK(rig.jumpTitle() == (narrow ? "Preview" : "Jump ahead")); // the header follows in the same frame
      }
      rig.showsFile("notes.md");
      drawnInAValidPlace(rig);
    }
  }

  void skewPast(Rig& rig) { rig.skew += luciole::WorkbenchPreview::kDwell; }

  MediaView* media(Rig& rig) { return dynamic_cast<MediaView*>(rig.slot().content()); }

  // The preview changes home across a resize, in each direction, without dropping its load: the same load and the
  // same player, a sound that plays keeps playing and its position never goes back, and no frame draws the slot
  // hidden.
  void aResizeKeepsTheLoadAndAPlayingSound(Rig& rig) {
    rig.choice = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    rig.size = kSmall;
    rig.select("song.wav");
    rig.settle();
    skewPast(rig);
    rig.frames([&] { return media(rig) != nullptr && media(rig)->player() == nullptr && media(rig)->facts().audio; });
    rig.click(media(rig)->playButton());
    rig.frames([&] { return media(rig)->player() != nullptr && media(rig)->player()->state() == MediaState::Playing; });
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    const auto* load = rig.preview->load();
    const auto started = rig.preview->loadsStarted();
    const auto* view = media(rig);
    const auto* player = view->player();
    double position = player->position();
    for (const auto size : {kRoomy, kSmall, kWide, kSmall}) {
      rig.size = size;
      for (int frame = 0; frame < 6; ++frame) {
        rig.frame();
        TEST_CHECK(rig.session->shows(rig.slot()));
        TEST_CHECK(rig.preview->load() == load && rig.preview->loadsStarted() == started);
        TEST_CHECK(media(rig) == view && view->player() == player);
        TEST_CHECK(player->state() == MediaState::Playing);
        TEST_CHECK(player->position() >= position);
        position = player->position();
      }
      TEST_CHECK(
          rig.view().previewHome() == (size.width == kSmall.width ? PreviewHome::JumpAhead : PreviewHome::Column)
      );
    }
  }

  // A pointer resting on a Jump ahead row links that entry in the other panes; when the preview takes the pane the
  // row is hidden and gets no leave event, so its link must go with it.
  // The very first frame of a session already titles the pane for the preview it holds.
  void theFirstFrameTitlesThePane(const luciole::test::preview::PreviewFiles& files, ::test::SceneCapture& capture) {
    Rig rig(files, capture);
    rig.size = kSmall;
    rig.select("notes.md");
    rig.frame();
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    TEST_CHECK(rig.jumpTitle() == "Preview");
  }

  void aHiddenJumpRowLeavesNoLink(Rig& rig) {
    rig.choice = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    rig.size = kSmall;
    rig.select("folder");
    rig.settle();
    rig.session->state().hover(
        luciole::WorkbenchPane::JumpAhead, {.location = luciole::Location::fromPath("/tmp"), .folder = true}
    );
    TEST_CHECK(
        rig.session->state().hovered() && rig.session->state().hovered()->pane == luciole::WorkbenchPane::JumpAhead
    );
    rig.select("notes.md");
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    const auto& hovered = rig.session->state().hovered();
    TEST_CHECK(!hovered || hovered->pane != luciole::WorkbenchPane::JumpAhead);
  }

  std::pair<float, float> centerOf(const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return {x + (node.width() / 2), y + (node.height() / 2)};
  }

  // Dragging the left column's divider changes the room of the center under the pointer: the preview changes home
  // while the divider keeps the drag, which goes on to its release both ways.
  void aDragThatChangesTheHomeKeepsItsGrip(Rig& rig) {
    rig.choice = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    rig.size = kRoomy;
    rig.select("notes.md");
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::Column);
    auto& shell = rig.view().shell();
    const auto [x, y] = centerOf(shell.divider(wb::DividerId::Left));
    rig.press(x, y);
    TEST_CHECK(shell.resizing());
    for (const float to : {x + 100, x + 200, x + 300}) { // the center gives up its column on the way
      rig.drag(to, y);
      TEST_CHECK(shell.resizing());
      TEST_CHECK(rig.session->shows(rig.slot()));
    }
    TEST_CHECK(rig.view().previewHome() == PreviewHome::JumpAhead);
    TEST_CHECK(rig.preview->load() != nullptr);
    for (const float to : {x + 200, x + 100, x}) { // and takes it back
      rig.drag(to, y);
      TEST_CHECK(shell.resizing() && rig.session->shows(rig.slot()));
    }
    TEST_CHECK(rig.view().previewHome() == PreviewHome::Column);
    rig.release(x, y);
    TEST_CHECK(!shell.resizing());
    rig.settle();
    rig.showsFile("notes.md");
    TEST_CHECK(rig.view().previewHome() == PreviewHome::Column);
  }

  // A column divider's drag holds the number of columns it started with, so the preview does not change home in it.
  void aColumnDragKeepsTheHome(Rig& rig) {
    rig.size = kWide;
    rig.settle();
    rig.showsFile("notes.md");
    auto& center = rig.view().center();
    TEST_CHECK(rig.view().previewHome() == PreviewHome::Column);
    const auto columns = center.plan().columns.size();
    const auto [x, y] = centerOf(center.divider(0));
    rig.press(x, y);
    TEST_CHECK(center.resizing());
    for (const float to : {x - 60, x + 200, x + 500, x - 100}) {
      rig.drag(to, y);
      TEST_CHECK(center.resizing());
      TEST_CHECK(center.plan().columns.size() == columns);
      TEST_CHECK(rig.view().previewHome() == PreviewHome::Column && rig.session->shows(rig.slot()));
    }
    rig.release(x - 100, y);
    TEST_CHECK(!center.resizing());
  }

  // The first read of a media file starts GStreamer and loads its plugins, which a loaded machine stretches past the
  // probe's own time limit; the view's probe would then report no facts and the sound would never show its play
  // button. Reading the file once, with room, leaves the plugins loaded for the reads the checks wait on.
  void warmTheMediaProbe(const std::filesystem::path& clip) {
    if (!luciole::MediaProbe::available()) {
      return;
    }
    luciole::MediaProbe probe;
    bool done = false;
    probe.start(clip.string(), [&done](auto) { done = true; }, std::chrono::seconds(60));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(65);
    while (!done && std::chrono::steady_clock::now() < deadline) {
      g_main_context_iteration(nullptr, TRUE);
    }
    TEST_CHECK(done);
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
  warmTheMediaProbe(files.folder() / "song.wav");
  {
    Rig rig(files, *capture);
    rig.choice = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    aResizeNeverDrawsThePreviewInAStalePlace(rig);
    theFirstFrameTitlesThePane(files, *capture);
    if (luciole::MediaPlayer::available()) { // a build without playback has no sound to keep
      aResizeKeepsTheLoadAndAPlayingSound(rig);
    }
    aHiddenJumpRowLeavesNoLink(rig);
    aDragThatChangesTheHomeKeepsItsGrip(rig);
    aColumnDragKeepsTheHome(rig);
  }
  std::println("workbench-preview-move: all checks passed");
  return 0;
}
