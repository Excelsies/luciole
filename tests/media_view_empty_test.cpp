#include "media_clips.h"
#include "media_view_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/media_view.h"
#include "ui/style.h"

#include <memory>
#include <print>
#include <string>

// A media view with no picture yet draws no frame around an empty box: the stage looks the same with the soft shadow
// on as with it off.
namespace {
  using namespace test::media;

  // The stage's pixels, drawn with the popup shadows on and off, differ nowhere.
  void shadowsChangeNothing(ViewRig& rig) {
    for (int i = 0; i < 4; ++i) {
      (void)rig.frame();
    }
    Style::setPopupShadowsEnabled(true);
    const auto with = rig.frame();
    Style::setPopupShadowsEnabled(false);
    const auto without = rig.frame();
    Style::setPopupShadowsEnabled(true);
    TEST_CHECK(differing(with, without, rig.view->stageArea()) == 0);
  }

  // A sound file with no cover, the visual not running: the glyph on a bare stage.
  void soundWithoutACover(::test::SceneCapture& capture, const fs::path& tone) {
    ViewRig rig(capture);
    rig.view->setSource(tone.string());
    TEST_CHECK(rig.until([&] { return rig.view->showsSoundGlyph(); }));
    shadowsChangeNothing(rig);
  }

  // A video before its poster arrives, and one whose poster cannot be shown.
  void videoWithoutAPoster(::test::SceneCapture& capture, const fs::path& clip, bool poster) {
    ViewRig rig(capture);
    rig.view->setSource(clip.string());
    if (poster) {
      rig.view->setPoster(std::make_shared<LoadedImageFile>()); // no pixels
    }
    TEST_CHECK(rig.until([&] { return rig.view->facts().video; }));
    TEST_CHECK(!rig.view->showsPoster() && !rig.view->showsFrame());
    shadowsChangeNothing(rig);
  }
} // namespace

int main() {
  test::media::Scratch scratch;
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
  const auto tone = scratch.path("tone.wav");
  writeTone(tone, 2);
  const auto clip = scratch.path("bars.avi");
  TEST_CHECK(writeClip(clip, {.width = 160, .height = 120, .seconds = 8}));
  soundWithoutACover(*capture, tone);
  videoWithoutAPoster(*capture, clip, false);
  videoWithoutAPoster(*capture, clip, true);
  std::println("media-view-empty: all checks passed");
  return 0;
}
