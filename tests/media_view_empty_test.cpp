#include "media_clips.h"
#include "media_view_rig.h"
#include "png_writer.h"
#include "render/animation/animation_manager.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/controls/flex.h"
#include "ui/controls/media_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cstdlib>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>

// A media view with no picture yet draws no frame around an empty box: the stage looks the same with the soft shadow
// on as with it off.
namespace {
  using namespace test::media;

  // The stage on the preview's surface, 460 by 420, written for looking at into the folder named by
  // LUCIOLE_CAPTURE_DIR as <name>-<LUCIOLE_SHOT_SET, "after" by default>.png.
  void keepStage(::test::SceneCapture& capture, const fs::path& source, std::string_view name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const char* set = std::getenv("LUCIOLE_SHOT_SET");
    AnimationManager animations;
    Flex root;
    root.setDirection(FlexDirection::Vertical);
    root.setAlign(FlexAlign::Stretch);
    root.setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    root.setAnimationManager(&animations);
    auto media = std::make_unique<MediaView>(ColorRole::SurfaceContainerLow, luciole::AudioOutput::Silent);
    media->setFlexGrow(1);
    auto* view = static_cast<MediaView*>(root.addChild(std::move(media)));
    view->setSource(source.string());
    Pump pump;
    ::test::GlPixels pixels;
    for (int i = 0; i < 40; ++i) {
      pump.once();
      animations.tick(16);
      pixels = capture.capture(root, 460, 420, 1);
    }
    const auto path = fs::path(dir) / std::format("{}-{}.png", name, set != nullptr && set[0] != '\0' ? set : "after");
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

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
  setPalette(luciole::test::tiles::testPalette());
  const auto tone = scratch.path("tone.wav");
  writeTone(tone, 2);
  const auto clip = scratch.path("bars.avi");
  TEST_CHECK(writeClip(clip, {.width = 160, .height = 120, .seconds = 8}));
  keepStage(*capture, tone, "workbench-audio-no-cover");
  keepStage(*capture, clip, "workbench-video-no-poster");
  soundWithoutACover(*capture, tone);
  videoWithoutAPoster(*capture, clip, false);
  videoWithoutAPoster(*capture, clip, true);
  std::println("media-view-empty: all checks passed");
  return 0;
}
