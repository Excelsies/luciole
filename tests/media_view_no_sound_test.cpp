#include "media_clips.h"
#include "media_view_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/media_view.h"

#include <chrono>
#include <print>
#include <string>
#include <string_view>

// With no sound device: a video plays silent with a note beside the time, and a sound file says why it cannot play.
// The system's sound output is made to fail for real in a second run of this program, which ranks every audio sink
// out of automatic choice.
namespace {
  using namespace test::media;

  // With no sound output a video plays silent with a note beside the time. A sound file that cannot play says why
  // on the stage, in place of its glyph, and the bar is back to its not-playing state with nothing running.
  void saysWhenNoSoundDeviceOpens(::test::SceneCapture& capture, const fs::path& clip, const fs::path& tone) {
    ViewRig rig(capture, AudioOutput::System);
    rig.view->setSource(clip.string());
    rig.click(rig.view->playButton());
    rig.playing();
    TEST_CHECK(rig.until([&] { return rig.view->messageLabel().text() == "No sound device was found."; }));
    TEST_CHECK(rig.view->messageLabel().visible() && !rig.view->muteButton().visible());
    TEST_CHECK(rig.view->elapsedLabel().visible() && rig.view->positionBar().visible());
    TEST_CHECK(rig.until([&] { return rig.position() > 0.5; }));
    rig.view->setSource(tone.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().audio; }));
    TEST_CHECK(rig.view->messageLabel().text().empty());
    TEST_CHECK(rig.view->showsSoundGlyph() && !rig.view->stageNotice().visible());
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Error; }));
    const std::string reason = "This file cannot be played: no sound device was found.";
    TEST_CHECK(rig.until([&] {
      return rig.view->stageNotice().visible();
    }) && rig.view->stageNotice().text() == reason);
    TEST_CHECK(!rig.view->showsSoundGlyph() && !rig.view->audioVisual().active());
    // The bar is as before Play: Play offered, zero time, the bar disabled, no second copy of the sentence.
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Play" && rig.view->elapsedLabel().text() == "0:00");
    TEST_CHECK(rig.view->elapsedLabel().visible() && rig.view->positionBar().visible());
    TEST_CHECK(!rig.view->positionBar().enabled());
    TEST_CHECK(!rig.view->messageLabel().visible() && rig.view->messageLabel().text().empty());
    // Nothing runs and nothing tries again.
    TEST_CHECK(rig.until([] { return TimerManager::instance().pollTimeoutMs() < 0; }, std::chrono::milliseconds(2000)));
    const auto* failed = rig.view->player();
    rig.until([] { return false; }, std::chrono::milliseconds(300));
    TEST_CHECK(rig.view->player() == failed && rig.state() == MediaState::Error);
    // Another file clears the sentence.
    rig.view->setSource(clip.string());
    TEST_CHECK(!rig.view->stageNotice().visible());
  }

} // namespace

int main(int argc, char** argv) {
  const bool noSoundDevice = argc > 1 && std::string_view(argv[1]) == "--no-sound-device";
  Scratch scratch; // its private plugin registry serves this run and the one it starts
  if (!noSoundDevice) {
    const int status = runWithoutSoundDevices("--no-sound-device");
    TEST_CHECK(status == 0 || status == ::test::kSkipExitCode);
    return status == ::test::kSkipExitCode ? ::test::kSkipExitCode : 0;
  }
  if (const auto missing = missingElements(
          {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "wavparse", "playbin",
           "appsink", "videoconvertscale", "fakesink"}
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
  const auto tone = scratch.path("tone.wav");
  TEST_CHECK(writeClip(clip, {.width = 64, .height = 48, .seconds = 12}));
  writeTone(tone, 2);
  saysWhenNoSoundDeviceOpens(*capture, clip, tone);
  std::println("media-view-no-sound: all checks passed");
  return 0;
}
