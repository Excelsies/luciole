#include "media_clips.h"
#include "media_view_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/media_view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

// The edges of a parked video: keys and bars while it reopens, the end of the file, the notice and the picture shown
// meanwhile, and every way a parked view can be left or its file can change.
namespace {
  using luciole::MediaState;
  using namespace test::media;
  using namespace std::chrono_literals;

  constexpr auto kIdle = 300ms;

  void playsPausesAndParks(ViewRig& rig, const fs::path& clip, double from = 1.0) {
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(clip.string());
    rig.click(rig.view->stageArea());
    rig.playing();
    TEST_CHECK(rig.until([&] { return rig.position() >= from; }));
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.state() == MediaState::Paused);
    TEST_CHECK(rig.until([&] { return rig.view->parked() && rig.view->player() == nullptr; }, 5000ms));
  }

  bool settledAndOpen(ViewRig& rig) { return rig.view->player() != nullptr && !rig.view->parked(); }

  void repeatedKeysWhileTheFileReopensAccumulate(ViewRig& rig, const fs::path& clip) {
    playsPausesAndParks(rig, clip);
    const double parkedAt = rig.view->positionBar().fraction() * 30.0;
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right);
    rig.view->setIdleRelease(60s); // it stays open while the test reads the place
    TEST_CHECK(rig.until([&] { return settledAndOpen(rig) && rig.state() == MediaState::Paused; }));
    TEST_NEAR(rig.position(), parkedAt + 3 * MediaView::kSeekStep, 0.6);
  }

  void anEndedVideoNeverShowsATimePastItsEnd(ViewRig& rig, const fs::path& shortClip) {
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(shortClip.string());
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Ended; }, 10000ms));
    TEST_CHECK(rig.until([&] { return rig.view->parked(); }, 5000ms));
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right);
    for (int i = 0; i < 300; ++i) {
      (void)rig.frame();
      TEST_CHECK(rig.view->elapsedLabel().text() != "0:08" && rig.view->elapsedLabel().text() != "0:13");
      TEST_CHECK(rig.view->positionBar().fraction() <= 1.0);
    }
    // At the end still, and parked again after the wait.
    TEST_NEAR(rig.view->positionBar().fraction(), 1.0, 0.05);
    TEST_CHECK(rig.view->elapsedLabel().text() == "0:03");
  }

  void leftWhileParkedSeeksBack(ViewRig& rig, const fs::path& clip) {
    playsPausesAndParks(rig, clip, 1.0);
    rig.key(XKB_KEY_Left);
    rig.view->setIdleRelease(60s); // it stays open while the test reads the place
    TEST_CHECK(rig.until([&] { return settledAndOpen(rig) && rig.state() == MediaState::Paused; }));
    TEST_NEAR(rig.position(), 0.0, 0.3);
  }

  // The picture on the stage while a parked file opens again is the one that was kept, then a frame at the target:
  // never the first frame of the file.
  void neverShowsTheFirstFrameWhileResuming(::test::SceneCapture& capture, const fs::path& clip) {
    ::test::GlPixels first;
    {
      ViewRig rig(capture);
      rig.view->setSource(clip.string());
      rig.click(rig.view->stageArea());
      rig.playing();
      rig.click(rig.view->stageArea());
      rig.click(rig.view->positionBar(), 0.0F);
      TEST_NEAR(rig.position(), 0.0, 0.3);
      first = drawn(rig);
    }
    ViewRig rig(capture);
    playsPausesAndParks(rig, clip, 2.0);
    const auto kept = drawn(rig);
    TEST_CHECK(differing(kept, first, rig.view->stageArea()) > 40);
    rig.click(rig.view->stageArea());
    for (int i = 0; i < 400; ++i) {
      const auto shown = rig.frame();
      TEST_CHECK(differing(shown, first, rig.view->stageArea()) > 40);
      if (settledAndOpen(rig) && rig.state() == MediaState::Playing && rig.position() > 2.5) {
        break;
      }
    }
    TEST_CHECK(rig.state() == MediaState::Playing);
  }

  // The picture of a paused video after a seek: parked first, then asked to seek by `ask`; once it has settled the
  // picture must be the frame at the target, the one a seek on a live player shows, and not the kept frame.
  void
  aSeekWhileParkedAndPausedShowsTheFrameAtTheTarget(::test::SceneCapture& capture, const fs::path& ball, bool byBar) {
    ViewRig rig(capture);
    playsPausesAndParks(rig, ball, 2.0);
    rig.view->setIdleRelease(60s); // it stays open while the test looks at it
    const auto kept = drawn(rig);
    if (byBar) {
      rig.click(rig.view->positionBar(), 0.75F);
    } else {
      rig.key(XKB_KEY_Right);
    }
    TEST_CHECK(rig.until([&] { return settledAndOpen(rig) && rig.state() == MediaState::Paused; }));
    rig.pump.run(300ms);
    const auto shown = drawn(rig);
    // The same target again on the live player: a seek away and back.
    const double target = rig.position();
    rig.click(rig.view->positionBar(), 0.1F);
    TEST_CHECK(rig.until([&] { return !rig.view->player()->seeking() && rig.position() < 3.0; }));
    rig.click(rig.view->positionBar(), static_cast<float>((target + 0.01) / 12.0));
    TEST_CHECK(rig.until([&] { return !rig.view->player()->seeking() && std::abs(rig.position() - target) < 0.2; }));
    rig.pump.run(300ms);
    const auto expected = drawn(rig);
    TEST_CHECK(differing(kept, expected, rig.view->stageArea()) > 40);
    TEST_CHECK(differing(shown, expected, rig.view->stageArea()) < 40);
  }

  // What a seek on a live player to `target` shows: away and back by the bar.
  ::test::GlPixels liveFrameAt(ViewRig& rig, double target) {
    rig.click(rig.view->positionBar(), target < 6.0 ? 0.9F : 0.1F);
    TEST_CHECK(rig.until([&] { return !rig.view->player()->seeking() && std::abs(rig.position() - target) > 2.0; }));
    // A hair past the target, so the frame that starts at it is the one asked for and not the one before.
    rig.click(rig.view->positionBar(), static_cast<float>((target + 0.01) / 12.0));
    TEST_CHECK(rig.until([&] { return !rig.view->player()->seeking() && std::abs(rig.position() - target) < 0.3; }));
    rig.pump.run(300ms);
    return drawn(rig);
  }

  enum class Backward { LeftThrice, BarBefore, DragBack, LeftThenRight, LeftRightLeft };

  // Seeks to a place before the one a video was parked at, or Left and Right in quick turns during the reopen: the
  // picture that stands once the player has settled is the frame at the final target, and not the kept frame.
  void
  aBackwardSeekWhileParkedShowsTheFrameAtTheTarget(::test::SceneCapture& capture, const fs::path& ball, Backward how) {
    ViewRig rig(capture);
    // The bar cases aim before the parked place by more than the slack a frame may be older than it (0.5 s) and a
    // second more, so they tell a frame from the wrong place from a right one.
    const bool byBar = how == Backward::BarBefore || how == Backward::DragBack;
    playsPausesAndParks(rig, ball, byBar ? 3.0 : 2.0);
    rig.view->setIdleRelease(60s);
    const auto kept = drawn(rig);
    const auto& bar = rig.view->positionBar();
    float x = 0;
    float y = 0;
    Node::absolutePosition(&bar, x, y);
    switch (how) {
    case Backward::LeftThrice:
      rig.key(XKB_KEY_Left);
      rig.key(XKB_KEY_Left);
      rig.key(XKB_KEY_Left);
      break;
    case Backward::BarBefore:
      rig.click(bar, 0.1F);
      break;
    case Backward::DragBack:
      rig.press(x + bar.width() * 0.6F, y + bar.height() / 2);
      rig.dispatcher.pointerMotion(x + bar.width() * 0.3F, y + bar.height() / 2, ++rig.serial);
      rig.frame();
      rig.release(x + bar.width() * 0.1F, y + bar.height() / 2);
      break;
    case Backward::LeftThenRight:
      rig.key(XKB_KEY_Left);
      rig.key(XKB_KEY_Right);
      break;
    case Backward::LeftRightLeft: // the last place is below the one a reopen was first asked for
      rig.key(XKB_KEY_Left);
      rig.key(XKB_KEY_Right);
      rig.key(XKB_KEY_Left);
      break;
    }
    TEST_CHECK(rig.until([&] { return settledAndOpen(rig) && rig.state() == MediaState::Paused; }));
    rig.pump.run(300ms);
    const auto shown = drawn(rig);
    const double target = rig.position();
    const auto expected = liveFrameAt(rig, target);
    TEST_CHECK(differing(kept, expected, rig.view->stageArea()) > 40);
    TEST_CHECK(differing(shown, expected, rig.view->stageArea()) < 60);
  }

  // A drag on the bar right after a reopen shows frames along the way, before the button is released.
  void aDragRightAfterAReopenShowsFramesBeforeTheRelease(::test::SceneCapture& capture, const fs::path& ball) {
    ViewRig rig(capture);
    playsPausesAndParks(rig, ball, 2.0);
    rig.view->setIdleRelease(60s);
    const auto& bar = rig.view->positionBar();
    float x = 0;
    float y = 0;
    Node::absolutePosition(&bar, x, y);
    rig.press(x + bar.width() * 0.5F, y + bar.height() / 2);
    TEST_CHECK(rig.until([&] { return rig.view->player() != nullptr && rig.state() != MediaState::Loading; }));
    rig.pump.run(300ms);
    const auto first = drawn(rig);
    rig.dispatcher.pointerMotion(x + bar.width() * 0.9F, y + bar.height() / 2, ++rig.serial);
    rig.pump.run(600ms);
    const auto later = drawn(rig);
    TEST_CHECK(differing(first, later, rig.view->stageArea()) > 20);
    rig.release(x + bar.width() * 0.9F, y + bar.height() / 2);
  }

  // An ended file reopened for a seek that yields no new frame keeps the last picture; the first frame of the file
  // never shows, also when a catch-up comes later, as after a resize.
  void anEndedFileReopenedForASeekNeverShowsTheFirstFrame(::test::SceneCapture& capture, const fs::path& shortBall) {
    ::test::GlPixels first;
    {
      ViewRig rig(capture);
      rig.view->setSource(shortBall.string());
      rig.click(rig.view->stageArea());
      rig.playing();
      rig.click(rig.view->stageArea());
      rig.click(rig.view->positionBar(), 0.0F);
      first = drawn(rig);
    }
    ViewRig rig(capture);
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(shortBall.string());
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Ended; }, 10000ms));
    TEST_CHECK(rig.until([&] { return rig.view->parked(); }, 5000ms));
    rig.view->setIdleRelease(60s);
    const auto kept = drawn(rig);
    TEST_CHECK(differing(kept, first, rig.view->stageArea()) > 40);
    rig.click(rig.view->positionBar(), 0.99F);
    TEST_CHECK(rig.until([&] { return settledAndOpen(rig); }));
    rig.width += 40; // a new size asks for a catch-up
    for (int i = 0; i < 60; ++i) {
      TEST_CHECK(differing(rig.frame(), first, rig.view->stageArea()) > 40);
    }
  }

  // A reopen shows the notice a seek shows. The notice waits for a seek to take long; here it shows at the first look,
  // and a 4K clip takes a few looks to reach its place again.
  void aReopenShowsTheSeekingNotice(ViewRig& rig, const fs::path& slow) {
    rig.view->setSeekNoticeDelay(0ms);
    playsPausesAndParks(rig, slow, 2.0);
    rig.key(XKB_KEY_Right);
    rig.view->setIdleRelease(60s); // it stays open once it has reached its place
    bool seen = false;
    for (int i = 0; i < 2000 && !(settledAndOpen(rig) && rig.state() == MediaState::Paused); ++i) {
      (void)rig.frame();
      seen = seen || rig.view->messageLabel().text().starts_with("Seeking");
    }
    TEST_CHECK(seen);
  }

  void aNewSourceWhileParkedLeavesNothingBehind(ViewRig& rig, const fs::path& clip, const fs::path& other) {
    playsPausesAndParks(rig, clip);
    rig.view->setSource(other.string());
    TEST_CHECK(!rig.view->parked() && rig.view->player() == nullptr && !rig.view->showsFrame());
    rig.click(rig.view->stageArea());
    rig.playing();
    TEST_CHECK(rig.position() < 3.0 && !rig.view->parked());
  }

  void hidingAParkedViewClearsIt(ViewRig& rig, const fs::path& clip) {
    playsPausesAndParks(rig, clip);
    rig.view->setShown(false);
    TEST_CHECK(!rig.view->parked() && !rig.view->showsFrame());
    rig.view->setShown(true);
    rig.pump.run(2 * kIdle);
    TEST_CHECK(rig.view->player() == nullptr && !rig.view->parked());
  }

  void aFileDeletedWhileParkedEndsInAnErrorWithNoStaleFrame(ViewRig& rig, const fs::path& copy) {
    playsPausesAndParks(rig, copy);
    TEST_CHECK(fs::remove(copy));
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.until([&] { return !rig.view->messageLabel().text().empty(); }));
    TEST_CHECK(!rig.view->showsFrame());
    TEST_CHECK(!rig.view->parked() || rig.view->player() != nullptr);
  }

  void loopAndMuteChosenWhileParkedApplyOnResume(ViewRig& rig, const fs::path& clip) {
    playsPausesAndParks(rig, clip);
    rig.view->setLoop(true);
    rig.click(rig.view->muteButton());
    TEST_CHECK(rig.view->player() == nullptr);
    rig.click(rig.view->stageArea());
    rig.playing();
    TEST_CHECK(rig.view->player()->loop() && rig.view->player()->muted());
  }

  // The cover picture of an MP3 is a tag, not a video stream: GStreamer plays it as sound, so the player has no
  // video and the view never parks it.
  void aSoundFileWithACoverTagNeverParks(ViewRig& rig, const fs::path& cover) {
    rig.view->setIdleRelease(kIdle);
    rig.view->setSource(cover.string());
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }));
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.state() == MediaState::Paused);
    TEST_CHECK(!rig.view->player()->hasVideo());
    rig.pump.run(4 * kIdle);
    TEST_CHECK(rig.view->player() != nullptr && !rig.view->parked());
  }

  bool writeH264(const fs::path& path, int width, int height, int seconds, int keyInterval) {
    return writeLaunched(
        std::format(
            "videotestsrc num-buffers={} pattern=ball ! video/x-raw,format=I420,width={},height={},framerate=30/1 ! "
            "x264enc speed-preset=ultrafast key-int-max={} ! h264parse ! mp4mux ! filesink location=\"{}\"",
            30 * seconds, width, height, keyInterval, path.string()
        )
    );
  }

  // An MP3 with a cover picture, made with ffmpeg when it is there.
  bool writeCoverFile(const fs::path& path) {
    const auto cover = path.parent_path() / "cover.png";
    const auto make = std::format(
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i color=c=red:s=64x64 -frames:v 1 \"{}\" && "
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=d=3 -i \"{}\" -map 0:a -map 1:v -c:a libmp3lame "
        "-c:v mjpeg -id3v2_version 3 -disposition:v attached_pic \"{}\"",
        cover.string(), cover.string(), path.string()
    );
    return std::system(make.c_str()) == 0 && fs::exists(path);
  }
} // namespace

int main() {
  Scratch scratch;
  if (const auto missing = missingElements(
          {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "playbin", "appsink",
           "videoconvertscale", "fakesink", "x264enc", "h264parse", "mp4mux", "qtdemux", "avdec_h264"}
      );
      !missing.empty() || !luciole::MediaPlayer::available()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  const auto clip30 = scratch.path("clip30.avi");
  const auto ball = scratch.path("ball.avi");
  const auto other = scratch.path("other.avi");
  const auto shortClip = scratch.path("short3.avi");
  const auto shortBall = scratch.path("shortball.avi");
  const auto copy = scratch.path("copy.avi");
  const auto slow = scratch.path("slow.mp4");
  TEST_CHECK(writeClip(clip30, {.width = 64, .height = 48, .seconds = 30}));
  TEST_CHECK(writeClip(ball, {.width = 64, .height = 48, .seconds = 12, .pattern = "ball"}));
  TEST_CHECK(writeClip(other, {.width = 64, .height = 48, .seconds = 12}));
  TEST_CHECK(writeClip(shortBall, {.width = 64, .height = 48, .seconds = 3, .pattern = "ball"}));
  TEST_CHECK(writeClip(shortClip, {.width = 64, .height = 48, .seconds = 3}));
  TEST_CHECK(writeClip(copy, {.width = 64, .height = 48, .seconds = 12}));
  TEST_CHECK(writeH264(slow, 3840, 2160, 8, 250));
  // H.264 frames after a seek come with their time, as the frames of a real file do; the AVI clips' do not, and a
  // frame with no time is shown whatever place it is for.
  const auto h264ball = scratch.path("h264ball.mp4");
  TEST_CHECK(writeH264(h264ball, 320, 240, 12, 30));
  {
    ViewRig rig(*capture);
    repeatedKeysWhileTheFileReopensAccumulate(rig, clip30);
  }
  {
    ViewRig rig(*capture);
    anEndedVideoNeverShowsATimePastItsEnd(rig, shortClip);
  }
  {
    ViewRig rig(*capture);
    leftWhileParkedSeeksBack(rig, clip30);
  }
  neverShowsTheFirstFrameWhileResuming(*capture, ball);
  aSeekWhileParkedAndPausedShowsTheFrameAtTheTarget(*capture, ball, false);
  aSeekWhileParkedAndPausedShowsTheFrameAtTheTarget(*capture, ball, true);
  for (const auto how :
       {Backward::LeftThrice, Backward::BarBefore, Backward::DragBack, Backward::LeftThenRight,
        Backward::LeftRightLeft}) {
    aBackwardSeekWhileParkedShowsTheFrameAtTheTarget(*capture, h264ball, how);
  }
  aDragRightAfterAReopenShowsFramesBeforeTheRelease(*capture, ball);
  anEndedFileReopenedForASeekNeverShowsTheFirstFrame(*capture, shortBall);
  {
    ViewRig rig(*capture);
    aReopenShowsTheSeekingNotice(rig, slow);
  }
  {
    ViewRig rig(*capture);
    aNewSourceWhileParkedLeavesNothingBehind(rig, clip30, other);
  }
  {
    ViewRig rig(*capture);
    hidingAParkedViewClearsIt(rig, clip30);
  }
  {
    ViewRig rig(*capture);
    aFileDeletedWhileParkedEndsInAnErrorWithNoStaleFrame(rig, copy);
  }
  {
    ViewRig rig(*capture);
    loopAndMuteChosenWhileParkedApplyOnResume(rig, clip30);
  }
  {
    // Destroyed while parked: the file is closed and nothing waits.
    {
      ViewRig rig(*capture);
      playsPausesAndParks(rig, clip30);
    }
    Pump pump;
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip30); }));
  }
  if (const auto cover = scratch.path("cover.mp3"); writeCoverFile(cover)) {
    ViewRig rig(*capture);
    aSoundFileWithACoverTagNeverParks(rig, cover);
  } else {
    std::println("a cover picture is not checked: ffmpeg made no file");
  }
  std::println("media-view-park-edges: all checks passed");
  return 0;
}
