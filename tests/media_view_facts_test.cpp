#include "media/media_probe.h"
#include "media_clips.h"
#include "media_view_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/media_view.h"

#include <chrono>
#include <fstream>
#include <print>
#include <string>

// The container facts a media view shows when the probe runs out of time: a timeout says nothing of the file, so the
// view reads once more with a longer limit and says it is still reading; when that fails too it says so, and
// choosing the file again reads again. A file that is not media ends at once, with no second read and no message.
namespace {
  using namespace test::media;
  using std::chrono::milliseconds;

  constexpr std::string_view kReading = "Still reading the file...";
  constexpr std::string_view kUnread = "Could not read the details of this file.";

  // A real timeout: the first read gets no time at all, so it ends by its limit whatever the machine does.
  void aTimeoutIsTriedOnceMoreAndTheFactsArrive(::test::SceneCapture& capture, const fs::path& tone) {
    ViewRig rig(capture);
    rig.view->setProbeLimits(milliseconds(0), milliseconds(20'000));
    rig.view->setSource(tone.string());
    bool sawReading = false;
    TEST_CHECK(rig.until([&] {
      sawReading = sawReading || (rig.view->messageLabel().visible() && rig.view->messageLabel().text() == kReading);
      return rig.view->facts().audio;
    }));
    TEST_CHECK(sawReading && !rig.view->detailsUnread());
    TEST_CHECK(rig.until([&] { return rig.view->messageLabel().text().empty(); }));
    TEST_CHECK(rig.view->showsSoundGlyph());
  }

  // Both reads time out: the message says so within the two reads, nothing keeps running, and choosing the file
  // again reads it.
  void aSourceThatNeverAnswersEndsInAMessage(::test::SceneCapture& capture, const fs::path& tone) {
    ViewRig rig(capture);
    rig.view->setProbeLimits(milliseconds(0), milliseconds(0));
    rig.view->setSource(tone.string());
    TEST_CHECK(rig.until([&] { return rig.view->detailsUnread(); }));
    TEST_CHECK(rig.view->messageLabel().visible() && rig.view->messageLabel().text() == kUnread);
    TEST_CHECK(rig.until([] { return !luciole::MediaProbe::readsPending(); }));
    rig.until([] { return false; }, milliseconds(300)); // nothing tries again
    TEST_CHECK(rig.view->detailsUnread() && !luciole::MediaProbe::readsPending() && !rig.view->facts().audio);
    rig.view->setProbeLimits(luciole::MediaProbe::kTimeLimit, luciole::MediaProbe::kRetryLimit);
    rig.view->setSource(tone.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().audio; }));
    TEST_CHECK(!rig.view->detailsUnread() && rig.view->messageLabel().text().empty());
  }

  // Not a media file: the read ends at once, says nothing and is not repeated.
  void aTextFileIsNotMediaAndIsNotRetried(::test::SceneCapture& capture, const fs::path& text) {
    ViewRig rig(capture);
    rig.view->setSource(text.string());
    TEST_CHECK(rig.until([] { return !luciole::MediaProbe::readsPending(); }));
    bool sawMessage = false;
    rig.until(
        [&] {
          sawMessage = sawMessage || rig.view->messageLabel().visible();
          return false;
        },
        milliseconds(400)
    );
    TEST_CHECK(!sawMessage && !rig.view->detailsUnread() && !rig.view->facts().audio && !rig.view->facts().video);
    TEST_CHECK(!luciole::MediaProbe::readsPending());
  }
} // namespace

int main() {
  Scratch scratch;
  if (const auto missing = missingElements({"wavparse", "playbin", "appsink", "fakesink"}); !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  if (!luciole::MediaPlayer::available() || !luciole::MediaProbe::available()) {
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
  const auto text = scratch.path("notes.txt");
  writeTone(tone, 2);
  TEST_CHECK((std::ofstream(text) << "just some words\n").good());
  aTimeoutIsTriedOnceMoreAndTheFactsArrive(*capture, tone);
  aSourceThatNeverAnswersEndsInAMessage(*capture, tone);
  aTextFileIsNotMediaAndIsNotRetried(*capture, text);
  std::println("media-view-facts: all checks passed");
  return 0;
}
