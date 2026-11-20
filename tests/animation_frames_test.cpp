#include "animation_files.h"
#include "fs/location.h"
#include "imaging/animation_frames.h"
#include "imaging/animation_head.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <glib.h>
#include <memory>
#include <optional>
#include <print>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using luciole::AnimationFrame;
  using luciole::AnimationFrames;
  using luciole::AnimationLimits;
  using luciole::AnimationState;
  using test::FlatFrame;
  using namespace std::chrono_literals;

  constexpr std::array<std::uint8_t, 4> kRed{255, 0, 0, 255};
  constexpr std::array<std::uint8_t, 4> kGreen{0, 255, 0, 255};
  constexpr std::array<std::uint8_t, 4> kBlue{0, 0, 255, 255};

  const std::vector<FlatFrame> kThree{{kRed, 20}, {kGreen, 30}, {kBlue, 40}};

  // Runs the main context until `done` holds, failing after ten seconds.
  void waitUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      if (g_main_context_iteration(nullptr, FALSE) == FALSE) {
        std::this_thread::sleep_for(1ms);
      }
    }
  }

  struct Opened {
    std::unique_ptr<AnimationFrames> frames;
    int changes = 0;
  };

  std::unique_ptr<Opened> open(const std::filesystem::path& path, int width, int height, AnimationLimits limits = {}) {
    auto opened = std::make_unique<Opened>();
    auto* raw = opened.get();
    opened->frames = std::make_unique<AnimationFrames>(
        luciole::Location::fromPath(path.string()), width, height, [raw] { ++raw->changes; }, limits
    );
    waitUntil([raw] { return raw->frames->state() != AnimationState::Opening; });
    return opened;
  }

  std::filesystem::path
  write(const test::TemporaryDirectory& dir, const char* name, const std::vector<std::uint8_t>& bytes) {
    auto path = dir.path() / name;
    TEST_CHECK(test::writeBytes(path, bytes));
    return path;
  }

  // The next frame, waiting for the decoder; empty once the plays are over.
  std::shared_ptr<const AnimationFrame> take(AnimationFrames& frames) {
    std::shared_ptr<const AnimationFrame> frame;
    waitUntil([&] {
      frame = frames.next();
      return frame != nullptr || frames.finished();
    });
    return frame;
  }

  void sameColor(const AnimationFrame& frame, const std::array<std::uint8_t, 4>& rgba) {
    TEST_CHECK(frame.rgba.size() == static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4);
    const std::size_t center = (static_cast<std::size_t>(frame.height / 2) * frame.width + frame.width / 2) * 4;
    for (const std::size_t at : {std::size_t{0}, center, frame.rgba.size() - 4}) {
      for (std::size_t channel = 0; channel < 4; ++channel) {
        TEST_NEAR(static_cast<int>(frame.rgba[at + channel]), static_cast<int>(rgba[channel]), 3);
      }
    }
  }

  // Takes one play and checks its colors and delays against `expected`.
  void checkPlay(AnimationFrames& frames, const std::vector<FlatFrame>& expected) {
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto frame = take(frames);
      TEST_CHECK(frame != nullptr);
      TEST_CHECK(frame->index == i);
      TEST_CHECK(frame->delay == std::chrono::milliseconds(expected[i].delayMs));
      sameColor(*frame, expected[i].rgba);
    }
  }

  void gifFramesDelaysAndPlays() {
    const test::TemporaryDirectory dir;
    // The loop extension counts repeats after the first play.
    const auto opened = open(write(dir, "three.gif", test::animatedGif(48, 27, kThree, 2)), 1000, 1000);
    auto& frames = *opened->frames;
    TEST_CHECK(frames.state() == AnimationState::Animated);
    TEST_CHECK(frames.sourceWidth() == 48 && frames.sourceHeight() == 27);
    TEST_CHECK(frames.plays() == 3);
    for (int play = 0; play < 3; ++play) {
      checkPlay(frames, kThree);
    }
    TEST_CHECK(frames.frameCount() == 3U);
    TEST_CHECK(frames.resident());
    TEST_CHECK(!frames.truncated());
    TEST_CHECK(take(frames) == nullptr);
    TEST_CHECK(frames.finished());
    TEST_CHECK(frames.next() == nullptr);
  }

  void gifWithoutLoopExtensionPlaysOnce() {
    const test::TemporaryDirectory dir;
    const auto opened = open(write(dir, "once.gif", test::animatedGif(8, 8, kThree, std::nullopt)), 64, 64);
    auto& frames = *opened->frames;
    TEST_CHECK(frames.plays() == 1);
    checkPlay(frames, kThree);
    TEST_CHECK(take(frames) == nullptr);
    TEST_CHECK(frames.finished());
  }

  void apngKeepsAlphaAndPlays() {
    const test::TemporaryDirectory dir;
    const std::vector<FlatFrame> translucent{{kRed, 50}, {{0, 255, 0, 128}, 60}, {{0, 0, 255, 0}, 70}};
    const auto opened = open(write(dir, "three.png", test::animatedPng(20, 10, translucent, 2)), 64, 64);
    auto& frames = *opened->frames;
    TEST_CHECK(frames.state() == AnimationState::Animated);
    TEST_CHECK(frames.plays() == 2);
    checkPlay(frames, {translucent[0], translucent[1]});
    const auto clear = take(frames);
    TEST_CHECK(clear != nullptr && clear->rgba[3] == 0);
    checkPlay(frames, {translucent[0], translucent[1]});
    TEST_CHECK(take(frames) != nullptr);
    TEST_CHECK(take(frames) == nullptr);
  }

  void webpLoopsForever() {
    const test::TemporaryDirectory dir;
    const std::vector<FlatFrame> twoColors{{kRed, 30}, {{10, 200, 30, 255}, 40}};
    const auto opened = open(write(dir, "two.webp", test::animatedWebp(16, 12, twoColors, 0)), 64, 64);
    auto& frames = *opened->frames;
    TEST_CHECK(frames.state() == AnimationState::Animated);
    TEST_CHECK(frames.plays() == 0);
    for (int play = 0; play < 10; ++play) {
      checkPlay(frames, twoColors);
    }
    TEST_CHECK(!frames.finished());
  }

  void shortDelaysShowFor100ms() {
    const test::TemporaryDirectory dir;
    const std::vector<FlatFrame> quick{{kRed, 0}, {kGreen, 10}, {kBlue, 20}};
    for (const auto& path :
         {write(dir, "quick.gif", test::animatedGif(4, 4, quick, 0)),
          write(dir, "quick.png", test::animatedPng(4, 4, {{kRed, 5}, {kGreen, 11}}, 0))}) {
      const auto opened = open(path, 64, 64);
      const auto first = take(*opened->frames);
      const auto second = take(*opened->frames);
      TEST_CHECK(first->delay == 100ms);
      TEST_CHECK(second->delay == (path.extension() == ".gif" ? 100ms : 11ms));
    }
  }

  void stillImageAnswersNo() {
    const test::TemporaryDirectory dir;
    for (const auto& path :
         {write(dir, "one.gif", test::animatedGif(8, 8, {{kRed, 100}}, 0)),
          write(dir, "one.png", test::animatedPng(8, 8, {{kRed, 100}}, 0))}) {
      const auto opened = open(path, 64, 64);
      TEST_CHECK(opened->frames->state() == AnimationState::Still);
      TEST_CHECK(opened->frames->next() == nullptr);
      TEST_CHECK(opened->frames->heldBytes() == 0);
      TEST_CHECK(!opened->frames->decoding());
      // One frame was decoded to tell.
      TEST_CHECK(opened->frames->sourceWidth() == 8);
    }
  }

  std::optional<std::uint32_t> headPlays(const std::vector<std::uint8_t>& bytes) {
    return luciole::animationPlaysFromHead({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
  }

  void headGivesPlaysOrStill() {
    TEST_CHECK(headPlays(test::animatedGif(8, 8, kThree, 2)) == 3U);
    TEST_CHECK(headPlays(test::animatedGif(8, 8, kThree, 0)) == 0U);
    TEST_CHECK(headPlays(test::animatedGif(8, 8, kThree, std::nullopt)) == 1U);
    TEST_CHECK(headPlays(test::animatedPng(8, 8, kThree, 2)) == 2U);
    TEST_CHECK(headPlays(test::animatedWebp(8, 8, {{kRed, 30}, {kGreen, 30}}, 5)) == 5U);
    TEST_CHECK(!headPlays(test::stillPng(8, 8, kRed)));
    TEST_CHECK(!headPlays(test::stillWebp(8, 8, kRed, false)));
    TEST_CHECK(!headPlays(test::stillWebp(8, 8, kRed, true)));
    // Heads cut before they tell, or that are not images, leave the answer to the decoder.
    auto png = test::stillPng(8, 8, kRed);
    png.resize(20);
    auto gif = test::animatedGif(8, 8, kThree, 2);
    gif.resize(12);
    auto webp = test::stillWebp(8, 8, kRed, true);
    webp.resize(12);
    for (const auto& bytes : {png, gif, webp, std::vector<std::uint8_t>{}, std::vector<std::uint8_t>{'R', 'I', 'F'}}) {
      TEST_CHECK(headPlays(bytes).has_value());
    }
    // A chunk length past the end stops the walk.
    auto hostile = test::stillPng(8, 8, kRed);
    std::fill_n(hostile.begin() + 8, 4, 0xFF);
    TEST_CHECK(headPlays(hostile) == 0U);
  }

  // A PNG without acTL before its image data and a WebP without the animation flag answer from their first bytes,
  // with no decoder: the source size stays unknown.
  void stillFormatsAnswerFromTheirHeader() {
    const test::TemporaryDirectory dir;
    for (const auto& path :
         {write(dir, "plain.png", test::stillPng(40, 30, kRed)),
          write(dir, "plain.webp", test::stillWebp(40, 30, kRed, false)),
          write(dir, "extended.webp", test::stillWebp(40, 30, kRed, true))}) {
      int changes = 0;
      const AnimationFrames frames(luciole::Location::fromPath(path.string()), 64, 64, [&changes] { ++changes; });
      waitUntil([&] { return changes > 0; });
      TEST_CHECK(changes == 1);
      TEST_CHECK(frames.state() == AnimationState::Still);
      TEST_CHECK(!frames.decoding() && frames.heldBytes() == 0);
      TEST_CHECK(frames.sourceWidth() == 0 && frames.sourceHeight() == 0);
    }
  }

  void framesFitTheAskedSize() {
    const test::TemporaryDirectory dir;
    const auto path = write(dir, "wide.gif", test::animatedGif(64, 32, kThree, 0));
    const auto smaller = open(path, 16, 16);
    const auto frame = take(*smaller->frames);
    TEST_CHECK(frame->width == 16 && frame->height == 8);
    sameColor(*frame, kRed);
    // Never above the image's own size.
    const auto larger = open(path, 1000, 500);
    const auto natural = take(*larger->frames);
    TEST_CHECK(natural->width == 64 && natural->height == 32);
    // A box with no area still gives a pixel.
    const auto empty = open(path, 0, -4);
    const auto dot = take(*empty->frames);
    TEST_CHECK(dot->width == 1 && dot->height == 1);
  }

  // A quarter of the budget holds one frame: 16 KiB allows 1,024 pixels.
  void frameSizeStaysUnderTheByteCap() {
    constexpr std::size_t kSmall = 16U * 1024U;
    const auto [width, height] = luciole::animationFrameSize(30, 35, 1000, 1000, kSmall);
    TEST_CHECK(width * height <= 1024 && width * height >= 980);
    TEST_CHECK(width >= 29 && width <= 30 && height >= 33 && height <= 35);
    const auto [wideWidth, wideHeight] = luciole::animationFrameSize(3840, 2160, 8192, 8192, 64U * 1024U * 1024U);
    TEST_CHECK(static_cast<std::size_t>(wideWidth) * static_cast<std::size_t>(wideHeight) * 4 <= 16U * 1024U * 1024U);
    TEST_CHECK(wideWidth >= 2729 && wideHeight >= 1534);
    TEST_CHECK((luciole::animationFrameSize(64, 64, 66, 66, kSmall) == std::pair{32, 32}));
    TEST_CHECK((luciole::animationFrameSize(64, 32, 16, 16, kSmall) == std::pair{16, 8}));
    TEST_CHECK((luciole::animationFrameSize(64, 32, 1000, 500, 1U << 30U) == std::pair{64, 32}));
    // A side held at one pixel still keeps the cap.
    const auto [thinWidth, thinHeight] = luciole::animationFrameSize(8000, 1, 8192, 8192, 64);
    TEST_CHECK(thinWidth * thinHeight <= 4 && thinWidth >= 1 && thinHeight == 1);
    for (const auto& size :
         {luciole::animationFrameSize(0, 10, 64, 64, kSmall), luciole::animationFrameSize(-5, -5, 64, 64, kSmall),
          luciole::animationFrameSize(10, 10, 0, -4, kSmall), luciole::animationFrameSize(10, 10, 64, 64, 0)}) {
      TEST_CHECK((size == std::pair{1, 1}));
    }
    const test::TemporaryDirectory dir;
    const auto opened = open(
        write(dir, "capped.gif", test::animatedGif(30, 35, kThree, 0)), 1000, 1000,
        {.budgetBytes = kSmall, .maxFrames = 5000}
    );
    const auto frame = take(*opened->frames);
    TEST_CHECK(frame != nullptr && frame->rgba.size() <= 4096);
  }

  std::array<std::uint8_t, 4> pixel(const AnimationFrame& frame, int x, int y) {
    const auto at =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)) * 4;
    return {frame.rgba[at], frame.rgba[at + 1], frame.rgba[at + 2], frame.rgba[at + 3]};
  }

  void scalingKeepsRowsInPlace() {
    const test::TemporaryDirectory dir;
    const std::vector<FlatFrame> split{{.rgba = kRed, .delayMs = 50, .lower = kBlue}, {kGreen, 50}};
    const auto path = write(dir, "split.png", test::animatedPng(64, 32, split, 0));
    const auto natural = open(path, 64, 64);
    const auto whole = take(*natural->frames);
    TEST_CHECK(whole->width == 64 && whole->height == 32);
    TEST_CHECK(pixel(*whole, 63, 15) == kRed && pixel(*whole, 0, 16) == kBlue);
    const auto smaller = open(path, 16, 16);
    const auto quarter = take(*smaller->frames);
    TEST_CHECK(quarter->width == 16 && quarter->height == 8);
    for (int x = 0; x < 16; ++x) {
      TEST_CHECK(pixel(*quarter, x, 3) == kRed && pixel(*quarter, x, 4) == kBlue);
    }
  }

  std::vector<FlatFrame> rainbow(std::size_t count) {
    std::vector<FlatFrame> frames;
    frames.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      frames.push_back({{static_cast<std::uint8_t>(i * 6), static_cast<std::uint8_t>(250 - i * 6), 90, 255}, 20});
    }
    return frames;
  }

  void longAnimationStreamsWithinTheBudget() {
    const test::TemporaryDirectory dir;
    const auto colors = rainbow(40);
    // 32 by 32 frames take 4 KiB each; the budget holds four.
    const AnimationLimits limits{.budgetBytes = 16U * 1024U, .maxFrames = 5000};
    const auto opened = open(write(dir, "long.png", test::animatedPng(32, 32, colors, 2)), 32, 32, limits);
    auto& frames = *opened->frames;
    TEST_CHECK(frames.state() == AnimationState::Animated);
    std::size_t shown = 0;
    for (int play = 0; play < 2; ++play) {
      for (std::size_t i = 0; i < colors.size(); ++i) {
        const auto frame = take(frames);
        TEST_CHECK(frame != nullptr && frame->index == i);
        sameColor(*frame, colors[i].rgba);
        TEST_CHECK(frames.heldBytes() <= limits.budgetBytes);
        ++shown;
      }
    }
    TEST_CHECK(shown == 80);
    TEST_CHECK(!frames.resident());
    TEST_CHECK(frames.frameCount() == 40U);
    TEST_CHECK(take(frames) == nullptr);
    TEST_CHECK(frames.finished());
    waitUntil([&] { return !frames.decoding(); });
    TEST_CHECK(frames.heldBytes() == 0);
  }

  // A play that fits the budget still arrives a few frames at a time, so playback never waits for the whole play.
  void firstPlayArrivesInSmallSteps() {
    const test::TemporaryDirectory dir;
    const auto colors = rainbow(40);
    const auto path = write(dir, "many.gif", test::animatedGif(64, 48, colors, 0));
    std::size_t taken = 0;
    std::size_t largestStep = 0;
    std::size_t steps = 0;
    std::unique_ptr<AnimationFrames> frames;
    // Takes every frame that has arrived; frames of a later play are not counted.
    const auto drain = [&] {
      std::size_t step = 0;
      for (std::size_t i = 0; i < colors.size() && taken < colors.size(); ++i) {
        const auto frame = frames->next();
        if (frame == nullptr) {
          break;
        }
        TEST_CHECK(frame->index == taken);
        ++taken;
        ++step;
      }
      largestStep = std::max(largestStep, step);
      ++steps;
    };
    frames = std::make_unique<AnimationFrames>(luciole::Location::fromPath(path.string()), 64, 48, drain);
    waitUntil([&] { return taken == colors.size(); });
    TEST_CHECK(frames->state() == AnimationState::Animated);
    TEST_CHECK(largestStep <= luciole::kAnimationFramesAhead);
    TEST_CHECK(steps >= colors.size() / luciole::kAnimationFramesAhead);
    waitUntil([&] { return !frames->decoding(); });
    TEST_CHECK(frames->resident());
    TEST_CHECK(frames->frameCount() == 40U);
  }

  void frameCapEndsThePlay() {
    const test::TemporaryDirectory dir;
    const auto colors = rainbow(12);
    const auto opened = open(
        write(dir, "capped.gif", test::animatedGif(8, 8, colors, 0)), 8, 8, {.budgetBytes = 1U << 20U, .maxFrames = 5}
    );
    auto& frames = *opened->frames;
    for (int play = 0; play < 2; ++play) {
      checkPlay(frames, {colors.begin(), colors.begin() + 5});
    }
    TEST_CHECK(frames.frameCount() == 5U);
    TEST_CHECK(frames.truncated());
  }

  void cutFileEndsAtItsLastWholeFrame() {
    const test::TemporaryDirectory dir;
    const auto colors = rainbow(4);
    const auto one = test::animatedGif(40, 40, {colors.begin(), colors.begin() + 1}, 0).size();
    const auto two = test::animatedGif(40, 40, {colors.begin(), colors.begin() + 2}, 0).size();
    const auto frameBytes = two - one;
    auto bytes = test::animatedGif(40, 40, colors, 0);
    bytes.resize(one - frameBytes + 2 * frameBytes + frameBytes / 2);
    const auto opened = open(write(dir, "cut.gif", bytes), 64, 64);
    auto& frames = *opened->frames;
    TEST_CHECK(frames.state() == AnimationState::Animated);
    for (int play = 0; play < 3; ++play) {
      checkPlay(frames, {colors.begin(), colors.begin() + 2});
    }
    TEST_CHECK(frames.frameCount() == 2U);
    TEST_CHECK(frames.truncated());
  }

  void unreadableFilesFail() {
    const test::TemporaryDirectory dir;
    const std::vector<std::uint8_t> junk{'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
    auto header = test::animatedGif(8, 8, kThree, 0);
    header.resize(20);
    // A canvas wider than any image the interface decodes.
    const auto wide = test::animatedGif(9000, 1, kThree, 0);
    for (const auto& path :
         {write(dir, "junk.gif", junk), write(dir, "empty.gif", {}), write(dir, "header.gif", header),
          write(dir, "wide.gif", wide), dir.path() / "missing.gif"}) {
      const auto opened = open(path, 64, 64);
      TEST_CHECK(opened->frames->state() == AnimationState::Failed);
      const auto& message = opened->frames->message();
      TEST_CHECK(!message.empty() && message.size() <= 240 && !message.contains('\n'));
      TEST_CHECK(opened->frames->next() == nullptr);
      std::println("{}: {}", path.filename().string(), opened->frames->message());
    }
  }

  void destroyedFramesPublishNothing() {
    const test::TemporaryDirectory dir;
    const auto path = write(dir, "three.gif", test::animatedGif(48, 27, kThree, 0));
    int changes = 0;
    {
      const AnimationFrames frames(luciole::Location::fromPath(path.string()), 64, 64, [&changes] { ++changes; });
      TEST_CHECK(frames.decoding());
    }
    const auto until = std::chrono::steady_clock::now() + 500ms;
    waitUntil([&] { return std::chrono::steady_clock::now() >= until; });
    TEST_CHECK(changes == 0);
  }
} // namespace

int main() {
  gifFramesDelaysAndPlays();
  gifWithoutLoopExtensionPlaysOnce();
  apngKeepsAlphaAndPlays();
  webpLoopsForever();
  shortDelaysShowFor100ms();
  stillImageAnswersNo();
  headGivesPlaysOrStill();
  stillFormatsAnswerFromTheirHeader();
  framesFitTheAskedSize();
  frameSizeStaysUnderTheByteCap();
  scalingKeepsRowsInPlace();
  longAnimationStreamsWithinTheBudget();
  firstPlayArrivesInSmallSteps();
  frameCapEndsThePlay();
  cutFileEndsAtItsLastWholeFrame();
  unreadableFilesFail();
  destroyedFramesPublishNothing();
  return 0;
}
