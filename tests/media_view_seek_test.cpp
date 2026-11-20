#include "media/media_player.h"
#include "media_clips.h"
#include "media_loop_rig.h"
#include "media_view_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/media_view.h"

#include <chrono>
#include <print>
#include <string>

// The seek note of the control bar: a seek that settles at once shows nothing; one in flight for longer than
// MediaView::kSeekNoticeDelay shows "Seeking..." beside the time, which goes when it settles, fails, is replaced by a
// newer seek, or the view stops. The seeks are held in flight where they reach the frame sink's counting filter of
// media_loop_rig.h.
namespace {
  using luciole::MediaState;
  using namespace test::media;
  using namespace std::chrono_literals;

  constexpr auto kNote = "Seeking...";

  bool noted(const ViewRig& rig) {
    const auto& label = rig.view->messageLabel();
    return label.visible() && label.text() == kNote;
  }

  void play(ViewRig& rig, const fs::path& clip) {
    rig.view->setSource(clip.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.click(rig.view->playButton());
    rig.playing();
  }

  // A seek is asked with a click on the position bar.
  void seek(ViewRig& rig, float share) { rig.click(rig.view->positionBar(), share); }

  void aQuickSeekShowsNothing(ViewRig& rig) {
    output().seekDelayMs = 0;
    seek(rig, 0.3F);
    TEST_CHECK(!noted(rig));
    // Over several watch intervals and past the delay: nothing came.
    TEST_CHECK(!rig.until([&] { return noted(rig); }, 700ms));
  }

  void aSlowSeekShowsTheNoteUntilItSettles(ViewRig& rig) {
    output().seekDelayMs = 1200;
    seek(rig, 0.6F);
    TEST_CHECK(!noted(rig));
    // Not before the delay.
    TEST_CHECK(!rig.until([&] { return noted(rig); }, 150ms));
    TEST_CHECK(rig.until([&] { return noted(rig); }, 2000ms));
    output().seekDelayMs = 0;
    TEST_CHECK(rig.until([&] { return !noted(rig); }, 8000ms));
    TEST_CHECK(!rig.view->player()->seeking());
  }

  void aRefusedSeekClearsTheNote(ViewRig& rig) {
    output().seekDelayMs = 800;
    output().refuseVideoNext = 4;
    seek(rig, 0.2F);
    TEST_CHECK(rig.until([&] { return noted(rig); }, 2000ms));
    TEST_CHECK(rig.until([&] { return !noted(rig); }, 4000ms));
    output().refuseVideoNext = 0;
    output().seekDelayMs = 0;
  }

  void aNewerSeekStartsTheWaitAgain(ViewRig& rig) {
    output().seekDelayMs = 1500;
    seek(rig, 0.4F);
    TEST_CHECK(rig.until([&] { return noted(rig); }, 2000ms));
    seek(rig, 0.7F);
    TEST_CHECK(!noted(rig));
    TEST_CHECK(rig.until([&] { return noted(rig); }, 2000ms));
    output().seekDelayMs = 0;
    TEST_CHECK(rig.until([&] { return !noted(rig); }, 10000ms));
  }

  void stoppingClearsTheNote(ViewRig& rig) {
    output().seekDelayMs = 1500;
    seek(rig, 0.5F);
    TEST_CHECK(rig.until([&] { return noted(rig); }, 2000ms));
    rig.view->setShown(false);
    TEST_CHECK(rig.view->player() == nullptr);
    TEST_CHECK(!noted(rig));
    rig.view->setShown(true);
    TEST_CHECK(rig.until([&] { return !noted(rig); }, 500ms) && !noted(rig));
    output().seekDelayMs = 0;
  }
} // namespace

int main() {
  Scratch scratch;
  if (const auto missing = missingElements(
          {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "playbin", "appsink",
           "videoconvertscale", "fakesink"}
      );
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  if (!luciole::MediaPlayer::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  const auto clip = scratch.path("clip.avi");
  TEST_CHECK(writeClip(clip, {.width = 64, .height = 48, .seconds = 20}));
  countVideoSeeks();
  ViewRig rig(*capture);
  play(rig, clip);
  aQuickSeekShowsNothing(rig);
  aSlowSeekShowsTheNoteUntilItSettles(rig);
  aRefusedSeekClearsTheNote(rig);
  aNewerSeekStartsTheWaitAgain(rig);
  stoppingClearsTheNote(rig);
  std::println("media-view-seek: all checks passed");
  return 0;
}
