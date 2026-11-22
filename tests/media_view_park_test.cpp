#include "bink2_sample.h"
#include "media_clips.h"
#include "media_view_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/media_view.h"

#include <chrono>
#include <cmath>
#include <format>
#include <print>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

// A video that sits paused or ended for a while gives its pipeline back and keeps its last frame, its time and its
// bar; Play, or a seek, opens it again at the same place.
namespace {
  using luciole::MediaState;
  using namespace test::media;
  using namespace std::chrono_literals;

  constexpr auto kIdle = 400ms;

  // Plays the clip, then pauses it with a click on the picture; the view parks `kIdle` later.
  void playsAndPauses(ViewRig& rig, const fs::path& clip) {
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(clip.string());
    rig.click(rig.view->stageArea());
    rig.playing();
    TEST_CHECK(rig.until([&] { return rig.position() >= 2.0; }));
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.state() == MediaState::Paused);
    TEST_CHECK(rig.view->player()->reopensInPlace());
  }

  void parksAPausedVideoAndKeepsWhatItShowed(ViewRig& rig, const fs::path& clip) {
    playsAndPauses(rig, clip);
    const double at = rig.position();
    const double fraction = rig.view->positionBar().fraction();
    const auto before = rig.view->elapsedLabel().text();
    TEST_CHECK(rig.view->player() != nullptr);
    TEST_CHECK(rig.until([&] { return rig.view->player() == nullptr; }, 3000ms));
    TEST_CHECK(rig.view->parked());
    TEST_CHECK(rig.until([&] { return !holdsOpen(clip); }));
    TEST_CHECK(rig.view->showsFrame());
    TEST_CHECK(rig.view->elapsedLabel().text() == before);
    TEST_NEAR(rig.view->positionBar().fraction(), fraction, 0.01);
    TEST_CHECK(rig.view->totalLabel().text() == "0:12");
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Play");
    std::println("parked at {:.2f} s", at);
  }

  void resumesAtTheSamePlace(ViewRig& rig) {
    const double fraction = rig.view->positionBar().fraction();
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.view->player() != nullptr);
    // While the file opens again the bar stays where it was.
    TEST_NEAR(rig.view->positionBar().fraction(), fraction, 0.01);
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Pause");
    // The view keeps its kept frame and place until the seek to them has settled.
    TEST_CHECK(rig.until([&] { return !rig.view->parked(); }));
    rig.playing();
    const double parkedAt = fraction * 12.0;
    TEST_CHECK(rig.position() >= parkedAt - 0.05);
    TEST_CHECK(rig.position() <= parkedAt + 2.0);
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.state() == MediaState::Paused);
  }

  void neverParksWhilePlayingOrWhileBusy(ViewRig& rig, const fs::path& clip) {
    // A long wait, so a stall of a second on a busy machine cannot park a video the test means to keep busy.
    constexpr auto kLongIdle = 2000ms;
    rig.view->setIdleRelease(kLongIdle);
    rig.view->setSource(clip.string());
    rig.click(rig.view->stageArea());
    rig.playing();
    rig.pump.run(kLongIdle + 500ms);
    TEST_CHECK(rig.state() == MediaState::Playing && !rig.view->parked());
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.state() == MediaState::Paused);
    // A seek is use: each one, and the settling after it, starts the wait again. Four of them take longer than one
    // wait.
    for (int i = 0; i < 4; ++i) {
      rig.pump.run(kLongIdle / 3);
      rig.key(XKB_KEY_Right);
      TEST_CHECK(!rig.view->parked());
    }
    TEST_CHECK(rig.until([&] { return rig.view->parked(); }, 10000ms));
  }

  void anEndedVideoParksAndPlaysFromTheStart(ViewRig& rig, const fs::path& shortClip) {
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(shortClip.string());
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Ended; }, 10000ms));
    TEST_CHECK(rig.until([&] { return rig.view->parked(); }, 3000ms));
    TEST_CHECK(rig.view->showsFrame());
    rig.click(rig.view->stageArea());
    rig.playing();
    TEST_CHECK(rig.position() < 1.0);
  }

  void aSeekWhileParkedOpensThePlayerPausedAtThePlace(ViewRig& rig, const fs::path& clip) {
    playsAndPauses(rig, clip);
    TEST_CHECK(rig.until([&] { return rig.view->parked(); }, 3000ms));
    rig.click(rig.view->positionBar(), 0.75F);
    TEST_CHECK(rig.view->player() != nullptr);
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Paused && !rig.view->parked(); }));
    TEST_NEAR(rig.position(), 9.0, 0.3);
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Play");
  }

  // Reopening a Bink 2 file in place decodes forward from a key frame, which costs about a second at 4K: such a
  // video keeps its player however long it sits paused.
  void aBink2VideoNeverParks(ViewRig& rig, const fs::path& sample) {
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(sample.string());
    rig.click(rig.view->stageArea());
    rig.playing();
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.state() == MediaState::Paused);
    rig.pump.run(4 * kIdle);
    TEST_CHECK(rig.view->player() != nullptr && !rig.view->parked());
    TEST_CHECK(!rig.view->player()->reopensInPlace());
  }

  void aSoundFileNeverParks(ViewRig& rig, const fs::path& tone) {
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(tone.string());
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }));
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.state() == MediaState::Paused);
    rig.pump.run(3 * kIdle);
    TEST_CHECK(rig.view->player() != nullptr && !rig.view->parked());
  }

  bool writeH264(const fs::path& path, int width, int height, int fps, int seconds, int keyInterval) {
    return writeLaunched(
        std::format(
            "videotestsrc num-buffers={} pattern=ball ! video/x-raw,format=I420,width={},height={},framerate={}/1 ! "
            "x264enc speed-preset=ultrafast key-int-max={} ! h264parse ! mp4mux ! filesink location=\"{}\"",
            fps * seconds, width, height, fps, keyInterval, path.string()
        )
    );
  }

  // How long Play takes after a long pause on a real H.264 clip: from the click to the position moving on from where
  // the video was parked. Printed for the record; only the position failing to move on fails.
  void timesTheResume(Scratch& scratch, ::test::SceneCapture& capture, int width, int height, int keyInterval) {
    const auto clip = scratch.path(std::format("h264-{}-{}.mp4", width, keyInterval).c_str());
    TEST_CHECK(writeH264(clip, width, height, 30, 15, keyInterval));
    ViewRig rig(capture);
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(clip.string());
    rig.click(rig.view->stageArea());
    rig.playing();
    TEST_CHECK(rig.until([&] { return rig.position() >= 7.3; }));
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.until([&] { return rig.view->parked(); }, 5000ms));
    const double parkedAt = rig.view->positionBar().fraction() * 15.0;
    const auto began = std::chrono::steady_clock::now();
    rig.click(rig.view->stageArea());
    const auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(); };
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }));
    const double loaded = since();
    // Playing from where it stood: once it has moved a fifth of a second on, which takes that long to play.
    TEST_CHECK(rig.until([&] { return rig.view->player()->position() > parkedAt + 0.2; }));
    const double moving = since();
    // Timing is printed for the record, not asserted: it is software decoding of 4K on whatever machine runs this.
    std::println(
        "resume of {}x{} with a key frame every {} frames, parked at {:.2f} s: playing after {:.3f} s, "
        "0.2 s further on after {:.3f} s",
        width, height, keyInterval, parkedAt, loaded, moving
    );
  }
} // namespace

int main() {
  Scratch scratch;
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
  const auto shortClip = scratch.path("short.avi");
  const auto tone = scratch.path("tone.wav");
  TEST_CHECK(writeClip(clip, {.width = 64, .height = 48, .seconds = 12}));
  TEST_CHECK(writeClip(shortClip, {.width = 64, .height = 48, .seconds = 1}));
  writeTone(tone, 2);
  {
    ViewRig rig(*capture);
    parksAPausedVideoAndKeepsWhatItShowed(rig, clip);
    resumesAtTheSamePlace(rig);
  }
  {
    ViewRig rig(*capture);
    neverParksWhilePlayingOrWhileBusy(rig, clip);
  }
  {
    ViewRig rig(*capture);
    anEndedVideoParksAndPlaysFromTheStart(rig, shortClip);
  }
  {
    ViewRig rig(*capture);
    aSeekWhileParkedOpensThePlayerPausedAtThePlace(rig, clip);
  }
  {
    ViewRig rig(*capture);
    aSoundFileNeverParks(rig, tone);
  }
  if (missingElements({"x264enc", "h264parse", "mp4mux", "avdec_h264", "qtdemux"}).empty()) {
    timesTheResume(scratch, *capture, 1920, 1080, 30);
    timesTheResume(scratch, *capture, 3840, 2160, 30);
    timesTheResume(scratch, *capture, 3840, 2160, 250);
  }
  if (const auto sample = test::bink2::sampleFrom(test::bink2::kSampleVariable)) {
    ViewRig rig(*capture);
    aBink2VideoNeverParks(rig, *sample);
  } else {
    std::println("a Bink 2 video is not checked: {} names no file", test::bink2::kSampleVariable);
  }
  std::println("media-view-park: all checks passed");
  return 0;
}
