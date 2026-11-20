#include "media_clips.h"
#include "png_writer.h"
#include "render/animation/animation_manager.h"
#include "render/core/image_decoder.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_clear.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/media_view.h"
#include "ui/palette.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <string_view>

// The media view drawn with real text: its poster before Play, then a decoded frame of color bars.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaState;
  using namespace test::media;
  using ::test::GlPixels;

  constexpr int kWidth = 320;
  constexpr int kHeight = 240;

  Palette darkPalette() {
    Palette out{};
    out.surface = rgba(0.07F, 0.08F, 0.10F, 1);
    out.onSurface = rgba(0.90F, 0.90F, 0.92F, 1);
    out.onSurfaceVariant = rgba(0.70F, 0.72F, 0.76F, 1);
    out.surfaceContainerLow = rgba(0.10F, 0.11F, 0.13F, 1);
    out.surfaceContainer = rgba(0.13F, 0.14F, 0.17F, 1);
    out.surfaceContainerHigh = rgba(0.17F, 0.18F, 0.21F, 1);
    out.surfaceContainerHighest = rgba(0.21F, 0.22F, 0.26F, 1);
    out.outlineVariant = rgba(0.30F, 0.32F, 0.36F, 1);
    out.primary = rgba(0.55F, 0.70F, 1.00F, 1);
    out.onPrimary = rgba(0.05F, 0.10F, 0.25F, 1);
    out.error = rgba(1.00F, 0.55F, 0.50F, 1);
    out.hover = rgba(0.17F, 0.18F, 0.21F, 1);
    out.onHover = out.onSurface;
    return out;
  }

  // A 16:9 poster: orange on the left, teal on the right.
  std::shared_ptr<const LoadedImageFile> poster() {
    auto image = std::make_shared<LoadedImageFile>();
    image->width = 160;
    image->height = 90;
    image->rgba.resize(std::size_t{160} * 90 * 4);
    for (int y = 0; y < 90; ++y) {
      for (int x = 0; x < 160; ++x) {
        auto* p = &image->rgba[(static_cast<std::size_t>(y) * 160 + static_cast<std::size_t>(x)) * 4];
        const bool left = x < 80;
        p[0] = left ? 240 : 30;
        p[1] = left ? 140 : 170;
        p[2] = left ? 40 : 160;
        p[3] = 255;
      }
    }
    return image;
  }

  struct Scene {
    AnimationManager animations;
    Flex root;
    MediaView* view = nullptr;
    Pump pump;

    explicit Scene(AudioOutput audio = AudioOutput::Silent) {
      root.setDirection(FlexDirection::Vertical);
      root.setAlign(FlexAlign::Stretch);
      root.setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
      root.setAnimationManager(&animations);
      auto media = std::make_unique<MediaView>(audio);
      media->setFlexGrow(1);
      view = static_cast<MediaView*>(root.addChild(std::move(media)));
    }
    GlPixels frame(::test::SceneCapture& capture, float scale, int width = kWidth, int height = kHeight) {
      pump.once();
      animations.tick(16);
      return capture.capture(root, width, height, scale);
    }
    // Draws frames until `done` holds or the wait ends; whether it held.
    bool
    until(::test::SceneCapture& capture, const std::function<bool()>& done, int width = kWidth, int height = kHeight) {
      const auto deadline = std::chrono::steady_clock::now() + kWait;
      for (std::int64_t i = 0; i < kWait.count() * 100 && std::chrono::steady_clock::now() < deadline; ++i) {
        if (done()) {
          return true;
        }
        (void)frame(capture, 1, width, height);
      }
      return done();
    }
  };

  // How many pixels of the box of shares (x0, y0)-(x1, y1) of the stage are near the color (r, g, b).
  int countNear(
      const GlPixels& pixels, const MediaView& view, float scale, std::array<float, 4> box, std::array<int, 3> color
  ) {
    const float stage = view.stageArea().height();
    int count = 0;
    for (int y = static_cast<int>(box[1] * stage * scale); y < static_cast<int>(box[3] * stage * scale); ++y) {
      for (int x = static_cast<int>(box[0] * view.width() * scale); x < static_cast<int>(box[2] * view.width() * scale);
           ++x) {
        const auto pixel = pixels.at(x, y);
        if (std::abs(pixel[0] - color[0]) <= 24
            && std::abs(pixel[1] - color[1]) <= 24
            && std::abs(pixel[2] - color[2]) <= 24) {
          ++count;
        }
      }
    }
    return count;
  }

  void keepForViewing(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  bool near(const std::array<std::uint8_t, 4>& pixel, int r, int g, int b, int tolerance) {
    return std::abs(pixel[0] - r) <= tolerance
        && std::abs(pixel[1] - g) <= tolerance
        && std::abs(pixel[2] - b) <= tolerance;
  }

  // Device pixel of a point given as shares of the stage, the view's picture area above the controls.
  std::array<std::uint8_t, 4> stageAt(const GlPixels& pixels, const MediaView& view, float x, float y, float scale) {
    const float stage = view.stageArea().height();
    return pixels.at(
        static_cast<int>(std::lround(x * view.width() * scale)), static_cast<int>(std::lround(y * stage * scale))
    );
  }

  // The stage share where bar `index` of seven sits on a 4:3 frame fitted inside the stage.
  float barShare(const MediaView& view, int index) {
    const float stageW = view.stageArea().width();
    const float shownW = view.stageArea().height() * 4.0F / 3.0F;
    const float left = (stageW - shownW) / 2;
    return (left + shownW * (static_cast<float>(index) + 0.5F) / 7.0F) / stageW;
  }

  // A sound file that cannot play, with the system's sound output failing for real: its reason is drawn in the middle
  // of the stage, where the glyph was, and nothing else of the stage is drawn.
  void drawsTheReasonInPlaceOfTheGlyph(::test::SceneCapture& capture, const std::filesystem::path& tone) {
    Scene scene(AudioOutput::System);
    scene.view->setSource(tone.string());
    TEST_CHECK(scene.until(capture, [&] { return scene.view->facts().audio; }));
    for (int i = 0; i < 4; ++i) {
      (void)scene.frame(capture, 1);
    }
    const auto before = scene.frame(capture, 1);
    const int glyphBefore = countNear(before, *scene.view, 1, {0.3F, 0.2F, 0.7F, 0.8F}, {179, 184, 194});
    TEST_CHECK(glyphBefore > 200);
    InputDispatcher input;
    input.setSceneRoot(&scene.root);
    float x = 0;
    float y = 0;
    Node::absolutePosition(&scene.view->playButton(), x, y);
    x += scene.view->playButton().width() / 2;
    y += scene.view->playButton().height() / 2;
    std::uint32_t serial = 0;
    input.pointerEnter(x, y, ++serial);
    input.pointerButton(x, y, BTN_LEFT, true, ++serial, 1000, false);
    input.pointerButton(x, y, BTN_LEFT, false, ++serial, 1100, false);
    input.pointerLeave();
    TEST_CHECK(scene.until(capture, [&] { return scene.view->stageNotice().visible(); }));
    GlPixels pixels;
    for (int i = 0; i < 3; ++i) {
      pixels = scene.frame(capture, 1);
    }
    keepForViewing(pixels, "media-view-no-sound-device-1x.png");
    // The sentence is drawn in the middle band, in a color that is not the glyph's, and no glyph is left.
    const int glyph = countNear(pixels, *scene.view, 1, {0.0F, 0.0F, 1.0F, 1.0F}, {179, 184, 194});
    TEST_CHECK(glyph == 0);
    const auto background = stageAt(pixels, *scene.view, 0.02F, 0.02F, 1);
    int differing = 0;
    const float stage = scene.view->stageArea().height();
    for (int py = static_cast<int>(0.4F * stage); py < static_cast<int>(0.6F * stage); ++py) {
      for (int px = static_cast<int>(0.1F * scene.view->width()); px < static_cast<int>(0.9F * scene.view->width());
           ++px) {
        const auto pixel = pixels.at(px, py);
        differing += !near(pixel, background[0], background[1], background[2], 40) ? 1 : 0;
      }
    }
    std::println("no sound device: {} pixels of the sentence in the middle band", differing);
    TEST_CHECK(differing > 150);
    const auto& notice = scene.view->stageNotice();
    TEST_CHECK(notice.x() >= 0 && notice.x() + notice.width() <= scene.view->width());
    TEST_NEAR(notice.x() + notice.width() / 2, scene.view->width() / 2, 1.5F);
  }
} // namespace

int main(int argc, char** argv) {
  const bool noSoundDevice = argc > 1 && std::string_view(argv[1]) == "--no-sound-device";
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
  setPalette(darkPalette());
  if (noSoundDevice) {
    const auto tone = scratch.path("tone.wav");
    writeTone(tone, 2);
    drawsTheReasonInPlaceOfTheGlyph(*capture, tone);
    std::println("media-view-pixels without a sound device: all checks passed");
    return 0;
  }
  const auto clip = scratch.path("bars.avi");
  TEST_CHECK(writeClip(clip, {.width = 160, .height = 120, .seconds = 8}));

  Scene scene;
  auto& view = *scene.view;
  view.setSource(clip.string());
  view.setPoster(poster());
  for (const float scale : {1.0F, 1.5F}) {
    GlPixels pixels;
    for (int i = 0; i < 4; ++i) {
      pixels = scene.frame(*capture, scale);
    }
    TEST_CHECK(view.showsPoster());
    keepForViewing(pixels, std::format("media-view-poster-{}x.png", scale));
    // The wide poster fills the stage's width, centered: orange on the left, teal on the right.
    TEST_CHECK(near(stageAt(pixels, view, 0.25F, 0.5F, scale), 240, 140, 40, 6));
    TEST_CHECK(near(stageAt(pixels, view, 0.75F, 0.5F, scale), 30, 170, 160, 6));
    // Above and below it, the background shows.
    TEST_CHECK(!near(stageAt(pixels, view, 0.25F, 0.02F, scale), 240, 140, 40, 60));
    TEST_CHECK(!near(stageAt(pixels, view, 0.25F, 0.98F, scale), 240, 140, 40, 60));
  }

  // The control bar's times and buttons at the text scale of a 16 pt system font, 21.33 px over the 14 px body size.
  capture->setTextScale(16.0F * 96.0F / 72.0F / 14.0F);
  for (int i = 0; i < 4; ++i) {
    (void)scene.frame(*capture, 1.0F);
  }
  TEST_CHECK(::test::expectTextClear(scene.root, capture->textScale()) > 0);
  capture->setTextScale(1.0F);

  // Play from the button, then pause on a frame with a click on the picture, and draw it.
  scene.frame(*capture, 1);
  InputDispatcher dispatcher;
  dispatcher.setSceneRoot(&scene.root);
  std::uint32_t serial = 0;
  const auto click = [&](const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    x += node.width() / 2;
    y += node.height() / 2;
    dispatcher.pointerEnter(x, y, ++serial);
    dispatcher.pointerMotion(x, y, ++serial);
    dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 1000, false);
    dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 1100, false);
  };
  click(view.playButton());
  const auto reached = [&](const std::function<bool()>& done) { return scene.until(*capture, done); };
  TEST_CHECK(reached([&] { return view.showsFrame() && view.player()->position() >= 3.2; }));
  click(view.stageArea());
  TEST_CHECK(view.player()->state() == MediaState::Paused);
  // Paused, the view stops ticking once the button's hover has faded: nothing runs while it rests.
  TEST_CHECK(reached([&] { return !scene.animations.hasActive(); }));
  for (const float scale : {1.0F, 1.5F}) {
    GlPixels pixels;
    for (int i = 0; i < 4; ++i) {
      pixels = scene.frame(*capture, scale);
    }
    TEST_CHECK(view.showsFrame());
    keepForViewing(pixels, std::format("media-view-frame-{}x.png", scale));
    // The color bars: white, yellow, cyan, green, magenta, red, blue, left to right across the top.
    const std::array<std::array<int, 3>, 7> bars{{
        {255, 255, 255},
        {255, 255, 0},
        {0, 255, 255},
        {0, 255, 0},
        {255, 0, 255},
        {255, 0, 0},
        {0, 0, 255},
    }};
    for (int i = 0; i < 7; ++i) {
      const auto pixel = stageAt(pixels, view, barShare(view, i), 0.3F, scale);
      std::println("{}x bar {}: {} {} {}", scale, i, pixel[0], pixel[1], pixel[2]);
      TEST_CHECK(near(pixel, bars[i][0], bars[i][1], bars[i][2], 12));
    }
  }
  std::println("elapsed {} of {}", view.elapsedLabel().text(), view.totalLabel().text());

  // Paused and made smaller, the frame is decoded again at the new size rather than the old one scaled down.
  {
    constexpr int kSmallW = 200;
    constexpr int kSmallH = 100;
    TEST_CHECK(scene.until(
        *capture, [&] { return view.framePixels().second == static_cast<int>(std::lround(view.stageArea().height())); },
        kSmallW, kSmallH
    ));
    const auto pixels = scene.frame(*capture, 1, kSmallW, kSmallH);
    std::println(
        "resized frame {}x{} in a {}x{} stage", view.framePixels().first, view.framePixels().second,
        view.stageArea().width(), view.stageArea().height()
    );
    TEST_CHECK(view.framePixels().second < 120);
    keepForViewing(pixels, "media-view-frame-resized-1x.png");
    TEST_CHECK(near(stageAt(pixels, view, barShare(view, 1), 0.3F, 1), 255, 255, 0, 12));
    TEST_CHECK(near(stageAt(pixels, view, barShare(view, 6), 0.3F, 1), 0, 0, 255, 12));
  }

  // A sound file with no cover: the sound glyph in the middle of the stage.
  {
    const auto tone = scratch.path("tone.wav");
    writeTone(tone, 2);
    Scene sound;
    sound.view->setSource(tone.string());
    TEST_CHECK(sound.until(*capture, [&] { return sound.view->showsSoundGlyph(); }));
    for (const float scale : {1.0F, 1.5F}) {
      GlPixels pixels;
      for (int i = 0; i < 4; ++i) {
        pixels = sound.frame(*capture, scale);
      }
      keepForViewing(pixels, std::format("media-view-sound-{}x.png", scale));
      // The glyph is drawn in the role's color in the middle, and nothing is drawn near the stage's edges.
      const int middle = countNear(pixels, *sound.view, scale, {0.3F, 0.2F, 0.7F, 0.8F}, {179, 184, 194});
      const int edges = countNear(pixels, *sound.view, scale, {0.0F, 0.0F, 0.15F, 0.9F}, {179, 184, 194});
      std::println("{}x sound glyph pixels: {} in the middle, {} at the left edge", scale, middle, edges);
      TEST_CHECK(middle > 200);
      TEST_CHECK(edges == 0);
    }
  }

  // No sound device: the video plays and the note sits beside the time.
  {
    breakSystemSoundOutput();
    Scene silent(AudioOutput::System);
    silent.view->setSource(clip.string());
    (void)silent.frame(*capture, 1);
    InputDispatcher input;
    input.setSceneRoot(&silent.root);
    float x = 0;
    float y = 0;
    Node::absolutePosition(&silent.view->playButton(), x, y);
    x += silent.view->playButton().width() / 2;
    y += silent.view->playButton().height() / 2;
    input.pointerEnter(x, y, ++serial);
    input.pointerButton(x, y, BTN_LEFT, true, ++serial, 1000, false);
    input.pointerButton(x, y, BTN_LEFT, false, ++serial, 1100, false);
    input.pointerLeave();
    TEST_CHECK(silent.until(*capture, [&] {
      return silent.view->showsFrame()
          && silent.view->messageLabel().visible()
          && silent.view->player()->position() >= 1.0;
    }));
    const auto pixels = silent.frame(*capture, 1);
    keepForViewing(pixels, "media-view-no-sound-1x.png");
    TEST_CHECK(silent.view->messageLabel().text() == "No sound device was found.");
  }
  // The system's sound output is made to fail for real in a second run of this program.
  TEST_CHECK(runWithoutSoundDevices("--no-sound-device") == 0);
  std::println("media-view-pixels: all checks passed");
  return 0;
}
