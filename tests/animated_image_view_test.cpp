#include "animation_files.h"
#include "core/timer_manager.h"
#include "event_loop/glib_poll_source.h"
#include "png_writer.h"
#include "render/core/image_decoder.h"
#include "render/scene/image_node.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/animated_image_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <poll.h>
#include <print>
#include <string>
#include <sys/resource.h>
#include <utility>
#include <vector>
#include <wayland-client-protocol.h>

namespace {
  using luciole::AnimationState;
  using test::FlatFrame;
  using Clock = AnimatedImageView::Clock;
  using namespace std::chrono_literals;

  constexpr std::array<std::uint8_t, 4> kRed{255, 0, 0, 255};
  constexpr std::array<std::uint8_t, 4> kGreen{0, 255, 0, 255};
  constexpr std::array<std::uint8_t, 4> kBlue{0, 0, 255, 255};
  constexpr std::array<std::uint8_t, 4> kYellow{255, 220, 0, 255};
  const std::vector<FlatFrame> kThree{{kRed, 20}, {kGreen, 30}, {kBlue, 50}};
  // The first frame is red over yellow, so a capture shows which way up it is.
  const std::vector<FlatFrame> kSplit{{.rgba = kRed, .delayMs = 20, .lower = kYellow}, {kGreen, 30}, {kBlue, 50}};

  // Runs the main context and the toolkit's timers for up to `limit` milliseconds, sleeping in poll while idle.
  void runFor(GlibPollSource& glib, int limit) {
    std::vector<pollfd> fds;
    const auto start = glib.addPollFds(fds);
    int timeout = limit;
    for (const int due : {glib.pollTimeoutMs(), TimerManager::instance().pollTimeoutMs()}) {
      timeout = due >= 0 ? std::min(timeout, due) : timeout;
    }
    TEST_CHECK(::poll(fds.data(), fds.size(), timeout) >= 0);
    glib.dispatch(fds, start);
    TimerManager::instance().tick();
  }

  struct Rig {
    std::unique_ptr<test::SceneCapture> capture;
    test::TemporaryDirectory dir;
    GlibPollSource glib;
    Node root;
    Node* parent = nullptr;
    AnimatedImageView* view = nullptr;
    InputDispatcher dispatcher;
    Clock::time_point now = Clock::time_point{} + 1h;
    std::vector<AnimationState> reported;

    explicit Rig(std::unique_ptr<test::SceneCapture> rig, luciole::AnimationLimits limits = {})
        : capture(std::move(rig)) {
      parent = root.addChild(std::make_unique<Node>());
      view = static_cast<AnimatedImageView*>(
          parent->addChild(std::make_unique<AnimatedImageView>([this] { return now; }, limits))
      );
      view->onStateChanged = [this](AnimationState state) { reported.push_back(state); };
      dispatcher.setSceneRoot(&root);
    }

    ~Rig() { parent->removeChild(view); }

    void play(const char* name, const std::vector<std::uint8_t>& bytes) {
      const auto path = dir.path() / name;
      TEST_CHECK(test::writeBytes(path, bytes));
      view->setFile(luciole::Location::fromPath(path.string()));
    }

    // Lays the view out at `width` by `height` logical pixels and draws it.
    test::GlPixels draw(int width, int height, float scale) {
      parent->setSize(static_cast<float>(width), static_cast<float>(height));
      view->setSize(static_cast<float>(width), static_cast<float>(height));
      return capture->capture(root, width, height, scale);
    }

    void waitUntil(const std::function<bool()>& done) {
      const auto deadline = Clock::now() + 10s;
      while (!done()) {
        TEST_CHECK(Clock::now() < deadline);
        runFor(glib, 5);
      }
    }

    void click() {
      dispatcher.pointerMotion(20, 20, 1);
      dispatcher.pointerButton(20, 20, BTN_LEFT, true, 2, 0, false);
      dispatcher.pointerButton(20, 20, BTN_LEFT, false, 3, 0, false);
    }
  };

  bool near(const std::array<std::uint8_t, 4>& pixel, const std::array<std::uint8_t, 4>& rgba) {
    return std::ranges::all_of(std::array{0, 1, 2, 3}, [&](int i) { return std::abs(pixel[i] - rgba[i]) <= 3; });
  }

  void keep(const test::GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // A 160 by 90 image in a 200 by 200 box keeps its natural size, centered: columns 20 to 180 and rows 55 to 145. Its
  // corners are rounded, so the colors are read 8 pixels inside them; its shadow reaches 14 pixels past it, so the
  // bare surface is read 18 pixels out, and its own width is the run of opaque pixels.
  void checkLetterbox(
      const test::GlPixels& pixels, float scale, const std::array<std::uint8_t, 4>& top,
      const std::array<std::uint8_t, 4>& bottom
  ) {
    const auto at = [&](float x, float y) {
      return pixels.at(static_cast<int>(x * scale), static_cast<int>(y * scale));
    };
    TEST_CHECK(near(at(28, 63), top) && near(at(100, 95), top) && near(at(172, 63), top));
    TEST_CHECK(near(at(28, 137), bottom) && near(at(100, 105), bottom) && near(at(172, 137), bottom));
    TEST_CHECK(at(100, 37)[3] == 0 && at(100, 163)[3] == 0 && at(2, 100)[3] == 0 && at(198, 100)[3] == 0);
    int drawn = 0;
    const int row = static_cast<int>(70 * scale);
    for (int x = 0; x < pixels.width; ++x) {
      drawn += pixels.at(x, row)[3] > 200 ? 1 : 0;
    }
    TEST_NEAR(drawn, static_cast<int>(160 * scale), 2);
  }

  void showsTheFrameTheClockSays(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    rig.play("three.png", test::animatedPng(160, 90, kSplit, 0));
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
    const auto start = rig.now;
    TEST_CHECK(rig.view->state() == AnimationState::Animated);
    TEST_CHECK((rig.reported == std::vector{AnimationState::Animated}));
    TEST_CHECK(rig.view->playing() && (rig.view->sourceSize() == std::pair(160, 90)));
    const auto first = rig.draw(200, 200, 1);
    checkLetterbox(first, 1, kRed, kYellow);
    keep(first, "animated-image-0ms.png");
    rig.now = start + 60ms;
    rig.waitUntil([&] { return rig.view->frameIndex() == 2U; });
    const auto third = rig.draw(200, 200, 1);
    checkLetterbox(third, 1, kBlue, kBlue);
    keep(third, "animated-image-60ms.png");
    // 130 ms is 30 ms into the second play.
    rig.now = start + 130ms;
    rig.waitUntil([&] { return rig.view->frameIndex() == 1U; });
    const auto scaled = rig.draw(200, 200, 1.5F);
    checkLetterbox(scaled, 1.5F, kGreen, kGreen);
    keep(scaled, "animated-image-130ms-1.5x.png");
  }

  void clickPausesAndResumes(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    rig.play("three.gif", test::animatedGif(160, 90, kThree, 0));
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
    const auto start = rig.now;
    rig.now = start + 10ms;
    rig.click();
    TEST_CHECK(rig.view->paused() && !rig.view->playing());
    rig.now = start + 500ms;
    for (int i = 0; i < 10; ++i) {
      runFor(rig.glib, 5);
    }
    TEST_CHECK(rig.view->frameIndex() == 0U);
    // Resumed, the paused frame keeps its remaining 10 ms.
    rig.click();
    TEST_CHECK(!rig.view->paused() && rig.view->playing());
    rig.now = start + 505ms;
    for (int i = 0; i < 10; ++i) {
      runFor(rig.glib, 5);
    }
    TEST_CHECK(rig.view->frameIndex() == 0U);
    rig.now = start + 512ms;
    rig.waitUntil([&] { return rig.view->frameIndex() == 1U; });
  }

  bool textureAlive(TextureId id) { return id.valid() && glIsTexture(id.value()) == GL_TRUE; }

  void hidingReleasesEverything(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    rig.play("three.gif", test::animatedGif(160, 90, kThree, 0));
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
    const auto texture = rig.view->image().textureId();
    TEST_CHECK(textureAlive(texture));
    rig.view->setShown(false);
    TEST_CHECK(!rig.view->playing() && !rig.view->decoding() && !rig.view->frameIndex());
    TEST_CHECK(!rig.view->image().textureId().valid() && !textureAlive(texture));
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() < 0);
    // Shown again, it starts over.
    rig.now += 1s;
    rig.view->setShown(true);
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
    // An invisible ancestor stops it at the next frame.
    const auto again = rig.view->image().textureId();
    rig.parent->setVisible(false);
    rig.now += 25ms;
    rig.waitUntil([&] { return !rig.view->playing(); });
    TEST_CHECK(!rig.view->frameIndex() && !textureAlive(again) && !rig.view->decoding());
    rig.parent->setVisible(true);
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
  }

  void destroyingStopsTimerAndWorker(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    rig.play("three.gif", test::animatedGif(160, 90, kThree, 0));
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
    const auto texture = rig.view->image().textureId();
    rig.parent->removeChild(rig.view);
    rig.view = static_cast<AnimatedImageView*>(rig.parent->addChild(std::make_unique<AnimatedImageView>()));
    TEST_CHECK(!textureAlive(texture));
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() < 0);
    // A view destroyed while its first frames decode publishes nothing.
    rig.play("again.gif", test::animatedGif(160, 90, kThree, 0));
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.view->decoding(); });
    rig.parent->removeChild(rig.view);
    rig.view = static_cast<AnimatedImageView*>(rig.parent->addChild(std::make_unique<AnimatedImageView>()));
    for (int i = 0; i < 40; ++i) {
      runFor(rig.glib, 5);
    }
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() < 0);
  }

  void stillImagesAreLeftToTheOwner(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    rig.play("one.gif", test::animatedGif(160, 90, {{kRed, 100}}, 0));
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return !rig.reported.empty(); });
    TEST_CHECK((rig.reported == std::vector{AnimationState::Still}));
    TEST_CHECK(!rig.view->playing() && !rig.view->image().textureId().valid());
    rig.play("junk.gif", {'n', 'o', 'p', 'e'});
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.reported.size() == 2; });
    TEST_CHECK(rig.reported.back() == AnimationState::Failed);
    TEST_CHECK(!rig.view->playing() && !rig.view->image().textureId().valid());
  }

  // Twelve 4 KiB frames with room for four: the view waits for frames the decoder has not reached yet.
  void streamsAPlayLongerThanItsBudget(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture), {.budgetBytes = 16U * 1024U, .maxFrames = 5000});
    std::vector<FlatFrame> frames;
    frames.reserve(12);
    for (int i = 0; i < 12; ++i) {
      frames.push_back({{static_cast<std::uint8_t>(i * 20), 100, 50, 255}, 20});
    }
    rig.play("long.png", test::animatedPng(32, 32, frames, 0));
    rig.draw(200, 200, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
    const auto start = rig.now;
    for (int step = 1; step <= 30; ++step) {
      rig.now = start + step * 20ms + 1ms;
      rig.waitUntil([&] { return rig.view->frameIndex() == static_cast<std::size_t>(step % 12); });
      TEST_CHECK(rig.view->playing() || rig.view->decoding());
    }
  }

  void largerBoxDecodesLargerFrames(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    // The picture's box is the view less the room for its shadow, 12 pixels on each side.
    rig.play("wide.gif", test::animatedGif(400, 200, kThree, 0));
    rig.draw(124, 124, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
    TEST_CHECK(rig.view->image().textureWidth() == 100 && rig.view->image().textureHeight() == 50);
    rig.draw(324, 324, 1);
    rig.waitUntil([&] { return rig.view->image().textureWidth() == 300; });
    TEST_CHECK(rig.view->image().textureHeight() == 150);
    // A slightly smaller box keeps the frames it has.
    rig.draw(304, 304, 1);
    for (int i = 0; i < 60; ++i) {
      runFor(rig.glib, 5);
    }
    TEST_CHECK(rig.view->image().textureWidth() == 300);
  }

  // Runs the main context and timers for `span` of real time, long enough for the resize delay to pass.
  void runAbout(Rig& rig, std::chrono::milliseconds span) {
    const auto end = Clock::now() + span;
    while (Clock::now() < end) {
      runFor(rig.glib, 5);
    }
  }

  // A 16 KiB budget caps 64 by 64 frames at 32 by 32 whatever the box, so relayouts keep the play going.
  void budgetCappedFramesSurviveRelayout(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture), {.budgetBytes = 16U * 1024U, .maxFrames = 5000});
    rig.play("capped.gif", test::animatedGif(64, 64, kThree, 0));
    rig.draw(64, 64, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 0U; });
    TEST_CHECK(rig.view->image().textureWidth() == 32);
    rig.now += 25ms;
    rig.waitUntil([&] { return rig.view->frameIndex() == 1U; });
    for (const int side : {66, 64, 66, 200}) {
      rig.draw(side, side, 1);
      runAbout(rig, 400ms);
      TEST_CHECK(!rig.view->decoding());
      TEST_CHECK(rig.view->frameIndex() == 1U);
    }
  }

  int liveCallbackCopies = 0;
  bool callbackOutlivedTheView = false;

  // Counts its live copies in liveCallbackCopies.
  struct CopyCounter {
    CopyCounter() { ++liveCallbackCopies; }
    CopyCounter(const CopyCounter& /*other*/) { ++liveCallbackCopies; }
    CopyCounter(CopyCounter&& /*other*/) noexcept { ++liveCallbackCopies; }
    CopyCounter& operator=(const CopyCounter&) = default;
    CopyCounter& operator=(CopyCounter&&) noexcept = default;
    ~CopyCounter() { --liveCallbackCopies; }
  };

  // The owner swaps in its still preview from onStateChanged; the callback runs on to its end.
  void ownerMayDestroyTheViewFromItsCallback(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    rig.view->onStateChanged = [&rig, counter = CopyCounter{}](AnimationState state) {
      if (state != AnimationState::Still) {
        return;
      }
      rig.parent->removeChild(std::exchange(rig.view, nullptr));
      // Past this point the callback reads only globals.
      callbackOutlivedTheView = liveCallbackCopies > 0;
    };
    rig.play("one.gif", test::animatedGif(16, 16, {{kRed, 100}}, 0));
    rig.draw(64, 64, 1);
    rig.waitUntil([&] { return rig.view == nullptr; });
    TEST_CHECK(callbackOutlivedTheView);
    TEST_CHECK(liveCallbackCopies == 0);
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() < 0);
    rig.view = static_cast<AnimatedImageView*>(rig.parent->addChild(std::make_unique<AnimatedImageView>()));
  }

  double processorSeconds() {
    rusage usage{};
    TEST_CHECK(::getrusage(RUSAGE_SELF, &usage) == 0);
    const auto seconds = [](const timeval& t) { return static_cast<double>(t.tv_sec) + t.tv_usec / 1e6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
  }

  // Processor time of this process while a 480 by 270 animation plays at 20 frames a second, and while hidden.
  void playingCostsLittleAndHiddenNothing(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    // This view reads the real clock.
    rig.parent->removeChild(rig.view);
    rig.view = static_cast<AnimatedImageView*>(rig.parent->addChild(std::make_unique<AnimatedImageView>()));
    std::vector<FlatFrame> frames;
    frames.reserve(20);
    for (int i = 0; i < 20; ++i) {
      frames.push_back({{static_cast<std::uint8_t>(i * 12), 80, static_cast<std::uint8_t>(240 - i * 12), 255}, 50});
    }
    rig.play("measure.gif", test::animatedGif(480, 270, frames, 0));
    rig.draw(480, 270, 1);
    rig.waitUntil([&] { return rig.view->frameIndex() == 19U; });
    TEST_CHECK(!rig.view->decoding());
    const auto measure = [&](std::chrono::milliseconds span) {
      const double before = processorSeconds();
      const auto end = Clock::now() + span;
      while (Clock::now() < end) {
        runFor(
            rig.glib,
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count()) + 1
        );
      }
      return processorSeconds() - before;
    };
    const double playing = measure(3s);
    rig.view->setShown(false);
    const double hidden = measure(2s);
    std::println(
        "480x270 at 20 fps: {:.1f} ms of processor time per second playing, {:.2f} ms per second hidden",
        playing * 1000 / 3, hidden * 1000 / 2
    );
    TEST_CHECK(playing / 3 < 0.05);
    TEST_CHECK(hidden / 2 < 0.001);
  }

  // A still the owner decoded, 200 by 120 with a red left half and a blue right half.
  std::shared_ptr<LoadedImageFile> splitStill(int width, int height) {
    auto still = std::make_shared<LoadedImageFile>();
    still->width = width;
    still->height = height;
    still->sourceWidth = width;
    still->sourceHeight = height;
    still->rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto& color = x < width / 2 ? kRed : kBlue;
        std::ranges::copy(color, still->rgba.begin() + ((static_cast<std::ptrdiff_t>(y) * width) + x) * 4);
      }
    }
    return still;
  }

  // The box the picture is seen in is the view less 12 pixels of room for its shadow on each side: the 324 by 224
  // view below has a 300 by 200 box. At scale 1 a still fits the box and is never enlarged; other scales draw it that many times as large, centered
  // while it fits and scrolled once it does not, with the wheel and a drag moving it and a new scale keeping the
  // point at the center.
  void scalesScrollsAndPans(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    rig.view->setStill(splitStill(200, 120));
    TEST_CHECK((rig.view->sourceSize() == std::pair(200, 120)));
    struct Case {
      float scale;
      float width;
      float height;
      float maxX;
      float maxY;
    };
    for (const auto& item :
         {Case{0.7F, 140, 84, 0, 0}, Case{1, 200, 120, 0, 0}, Case{1.5F, 300, 180, 0, 0},
          Case{3, 600, 360, 300, 160}}) {
      rig.view->setPreviewScale(item.scale);
      const auto pixels = rig.draw(324, 224, 1);
      keep(pixels, std::format("picture-{}.png", static_cast<int>(std::lround(item.scale * 100))));
      TEST_NEAR(rig.view->image().width(), item.width, 0.01F);
      TEST_NEAR(rig.view->image().height(), item.height, 0.01F);
      TEST_NEAR(rig.view->maxScrollX(), item.maxX, 0.01F);
      TEST_NEAR(rig.view->maxScrollY(), item.maxY, 0.01F);
      TEST_CHECK(rig.view->pictureShown());
      // The center of the box shows the picture's own center: where its halves meet.
      const int middle = static_cast<int>(item.width / 2);
      TEST_CHECK(near(pixels.at(162 - std::min(middle, 150) + 3, 112), kRed));
      TEST_CHECK(near(pixels.at(162 + std::min(middle, 149) - 3, 112), kBlue));
      TEST_CHECK(!near(pixels.at(164, 112), pixels.at(159, 112))); // the halves meet at the center
    }
    // Scale 3: the picture was centered, so the scroll sits at the middle of its range.
    TEST_NEAR(rig.view->scrollX(), 150, 0.01F);
    TEST_NEAR(rig.view->scrollY(), 80, 0.01F);
    // A new scale keeps the picture's point at the box's center: 350 by 200 of 600 by 360 is 7/12 and 5/9 across.
    rig.view->scrollTo(200, 100);
    rig.view->setPreviewScale(2.5F);
    (void)rig.draw(324, 224, 1);
    TEST_NEAR(rig.view->scrollX(), (500.0F * 7 / 12) - 150, 0.01F);
    TEST_NEAR(rig.view->scrollY(), (300.0F * 5 / 9) - 100, 0.01F);
    // The wheel scrolls down and back, held to the range.
    const float before = rig.view->scrollY();
    TEST_CHECK(rig.dispatcher.pointerAxis(162, 112, WL_POINTER_AXIS_VERTICAL_SCROLL, 0, 15, 1, 120, 1, 1));
    (void)rig.draw(324, 224, 1);
    TEST_CHECK(rig.view->scrollY() > before && rig.view->scrollY() <= rig.view->maxScrollY());
    for (int i = 0; i < 20; ++i) {
      (void)rig.dispatcher.pointerAxis(162, 112, WL_POINTER_AXIS_VERTICAL_SCROLL, 0, -15, -1, -120, -1, 1);
    }
    TEST_NEAR(rig.view->scrollY(), 0, 0.01F);
    // A drag pans the other way from the pointer, and its release does not count as a click.
    const float x = rig.view->scrollX();
    rig.dispatcher.pointerMotion(162, 112, 10);
    rig.dispatcher.pointerButton(162, 112, BTN_LEFT, true, 11, 0, false);
    rig.dispatcher.pointerMotion(122, 82, 12);
    rig.dispatcher.pointerButton(122, 82, BTN_LEFT, false, 13, 0, false);
    TEST_NEAR(rig.view->scrollX(), x + 40, 0.01F);
    TEST_NEAR(rig.view->scrollY(), 30, 0.01F);
    TEST_CHECK(!rig.view->paused());
    // Back at scale 1 the picture fits and the wheel is left to the views around it.
    rig.view->setPreviewScale(1);
    (void)rig.draw(324, 224, 1);
    TEST_CHECK(rig.view->scrollX() == 0 && rig.view->scrollY() == 0);
    TEST_CHECK(!rig.dispatcher.pointerAxis(162, 112, WL_POINTER_AXIS_VERTICAL_SCROLL, 0, 15, 1, 120, 1, 1));
    // Scales out of range are clamped, and values that are not finite and positive are ignored.
    for (const float bad : {0.0F, -1.0F, std::numeric_limits<float>::quiet_NaN()}) {
      rig.view->setPreviewScale(bad);
      TEST_CHECK(rig.view->previewScale() == 1);
    }
    rig.view->setPreviewScale(100);
    TEST_CHECK(rig.view->previewScale() == AnimatedImageView::kMaxScale);
    // A larger picture fits the box at scale 1 and grows from there.
    rig.view->setStill(splitStill(1000, 500));
    rig.view->setPreviewScale(1);
    (void)rig.draw(324, 224, 1);
    TEST_NEAR(rig.view->image().width(), 300, 0.01F);
    TEST_NEAR(rig.view->image().height(), 150, 0.01F);
    rig.view->setPreviewScale(1.5F);
    (void)rig.draw(324, 224, 1);
    TEST_NEAR(rig.view->maxScrollX(), 150, 0.01F);
    TEST_NEAR(rig.view->maxScrollY(), 25, 0.01F);
    // Hidden, the still's texture goes; shown again, it comes back.
    const auto texture = rig.view->image().textureId();
    TEST_CHECK(textureAlive(texture));
    rig.view->setShown(false);
    TEST_CHECK(!rig.view->pictureShown() && !textureAlive(texture));
    rig.view->setShown(true);
    (void)rig.draw(324, 224, 1);
    TEST_CHECK(rig.view->pictureShown());
  }
  // The picture is framed as the side preview frames one: rounded corners, an outline, a soft shadow that shows only
  // while shadows are enabled, and room inside the view for it so the view's edge never cuts it.
  void framesThePicture(std::unique_ptr<test::SceneCapture> capture) {
    Rig rig(std::move(capture));
    Palette colors{};
    colors.outlineVariant = Color{0.55F, 0.55F, 0.6F, 1.0F};
    colors.surfaceContainerLow = Color{0.1F, 0.12F, 0.16F, 1.0F};
    setPalette(colors);
    const auto surface = resolveColorSpec(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    // A 100 by 60 picture is shown at its own size, centered in 200 by 200: columns 50 to 150, rows 70 to 130.
    rig.view->setStill(splitStill(100, 60));
    for (const bool shadows : {true, false}) {
      Style::setPopupShadowsEnabled(shadows);
      const auto pixels = rig.draw(200, 200, 1);
      keep(pixels, std::format("framed-small-shadow-{}.png", shadows));
      TEST_CHECK(pixels.at(50, 70)[3] < 128 && pixels.at(149, 129)[3] < 128); // rounded corners
      TEST_CHECK(near(pixels.at(56, 100), kRed) && near(pixels.at(144, 100), kBlue));
      const auto edge = pixels.at(75, 70); // the outline over the picture's top row
      TEST_CHECK(!near(edge, kRed) && edge[3] > 200);
      const auto below = pixels.at(100, 134)[3];
      TEST_CHECK(shadows ? below > 0 : below == 0);
    }
    Style::setPopupShadowsEnabled(true);
    // A picture that fills the box keeps its shadow whole: it reaches into the room reserved inside the view.
    rig.view->setStill(splitStill(1000, 500));
    const auto wide = rig.draw(200, 200, 1);
    keep(wide, "framed-wide.png");
    TEST_NEAR(rig.view->image().width(), 176, 0.01F);
    TEST_CHECK(wide.at(6, 100)[3] > 0 && wide.at(6, 100)[3] < wide.at(11, 100)[3] + 1);
    TEST_CHECK(wide.at(0, 100)[3] <= wide.at(6, 100)[3]);
    rig.view->setStill(splitStill(500, 1000));
    const auto tall = rig.draw(200, 200, 1);
    keep(tall, "framed-tall.png");
    TEST_NEAR(rig.view->image().height(), 176, 0.01F);
    TEST_CHECK(tall.at(100, 6)[3] > 0 && tall.at(100, 0)[3] <= tall.at(100, 6)[3]);
    rig.view->setStill(splitStill(1000, 500));
    // Zoomed past the box, the picture is clipped at the box and its visible corners are rounded, never the picture's.
    for (const float scrolled : {0.0F, 1.0F}) {
      rig.view->setPreviewScale(3);
      (void)rig.draw(200, 200, 1);
      rig.view->scrollTo(scrolled * rig.view->maxScrollX(), scrolled * rig.view->maxScrollY());
      const auto cut = rig.draw(200, 200, 1);
      keep(cut, std::format("framed-zoomed-{}.png", static_cast<int>(scrolled)));
      const auto bare = [&](int x, int y) {
        return std::abs(cut.at(x, y)[0] - static_cast<int>(std::lround(surface.r * 255))) <= 3
            && cut.at(x, y)[3] == 255;
      };
      TEST_CHECK(bare(12, 12) && bare(187, 12) && bare(12, 187) && bare(187, 187));
      TEST_CHECK(cut.at(100, 100)[3] == 255 && !bare(100, 100));
      // Nothing of the picture shows in the room around the box.
      TEST_CHECK(cut.at(6, 100)[3] < 128 && cut.at(100, 6)[3] < 128);
      rig.view->setPreviewScale(1);
    }
  }
} // namespace

int main() {
  const std::vector<void (*)(std::unique_ptr<test::SceneCapture>)> suites{
      showsTheFrameTheClockSays,
      clickPausesAndResumes,
      hidingReleasesEverything,
      destroyingStopsTimerAndWorker,
      stillImagesAreLeftToTheOwner,
      largerBoxDecodesLargerFrames,
      streamsAPlayLongerThanItsBudget,
      budgetCappedFramesSurviveRelayout,
      ownerMayDestroyTheViewFromItsCallback,
      playingCostsLittleAndHiddenNothing,
      scalesScrollsAndPans,
      framesThePicture,
  };
  for (const auto suite : suites) {
    std::string reason;
    auto capture = test::SceneCapture::create(reason);
    if (!capture) {
      std::println("skipped: {}", reason);
      return 77;
    }
    suite(std::move(capture));
  }
  return 0;
}
