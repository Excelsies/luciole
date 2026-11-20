#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/radar_sweep.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualCapture;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;

  constexpr int kTurnFrames = 240; // a quarter turn a second at 60 frames a second

  // A ring and sector of the scope: radii as shares of its radius, bearings as shares of a turn clockwise from north.
  struct Polar {
    double innerRadius = 0;
    double outerRadius = 1;
    double fromBearing = 0;
    double toBearing = 1;
  };

  struct PolarStats {
    int pixels = 0;
    int bright = 0;
    double meanLuma = 0;
    double brightBearing = 0; // bearing of the bright pixels' mean position
  };

  PolarStats polarStats(const test::GlPixels& image, const Polar& area, int minLuma) {
    const auto scope =
        luciole::workbench::fitRadarScope(static_cast<float>(image.width), static_cast<float>(image.height));
    PolarStats stats;
    double sumX = 0;
    double sumY = 0;
    double lumaSum = 0;
    for (int y = 0; y < image.height; ++y) {
      for (int x = 0; x < image.width; ++x) {
        const double dx = x + 0.5 - scope.centerX;
        const double dy = scope.centerY - (y + 0.5);
        const double r = std::hypot(dx, dy) / scope.radius;
        double bearing = std::atan2(dx, dy) / (2 * std::numbers::pi);
        bearing -= std::floor(bearing);
        if (r < area.innerRadius || r > area.outerRadius || bearing < area.fromBearing || bearing > area.toBearing) {
          continue;
        }
        const int luma = test::audio::luma(image.at(x, y));
        ++stats.pixels;
        lumaSum += luma;
        if (luma >= minLuma) {
          ++stats.bright;
          sumX += dx;
          sumY += dy;
        }
      }
    }
    stats.meanLuma = stats.pixels > 0 ? lumaSum / stats.pixels : 0;
    if (stats.bright > 0) {
      const double bearing = std::atan2(sumX, sumY) / (2 * std::numbers::pi);
      stats.brightBearing = bearing - std::floor(bearing);
    }
    return stats;
  }

  // Pixels of `area` whose luma rose, and fell, by more than `step` from `before` to `after`.
  std::pair<int, int>
  lumaChanges(const test::GlPixels& before, const test::GlPixels& after, const Polar& area, int step) {
    const auto scope =
        luciole::workbench::fitRadarScope(static_cast<float>(after.width), static_cast<float>(after.height));
    std::pair<int, int> changes{0, 0};
    for (int y = 0; y < after.height; ++y) {
      for (int x = 0; x < after.width; ++x) {
        const double dx = x + 0.5 - scope.centerX;
        const double dy = scope.centerY - (y + 0.5);
        const double r = std::hypot(dx, dy) / scope.radius;
        double bearing = std::atan2(dx, dy) / (2 * std::numbers::pi);
        bearing -= std::floor(bearing);
        if (r < area.innerRadius || r > area.outerRadius || bearing < area.fromBearing || bearing > area.toBearing) {
          continue;
        }
        const int change = test::audio::luma(after.at(x, y)) - test::audio::luma(before.at(x, y));
        changes.first += change > step ? 1 : 0;
        changes.second += change < -step ? 1 : 0;
      }
    }
    return changes;
  }

  double share(const PolarStats& stats) {
    return stats.pixels > 0 ? static_cast<double>(stats.bright) / stats.pixels : 0;
  }

  void checkSound(const AudioVisualCapture& capture) {
    TEST_CHECK(capture.stateKept);
    TEST_CHECK(capture.insideOnly);
    TEST_CHECK(capture.meanCpuMilliseconds < 0.5);
  }

  // Band 30 of 63 sits at 0.48 of the radius: after a full turn of a steady tone a glowing ring lies there, brightest
  // just behind the arm, and nothing else in the scope is lit but the arm and its marks. The ring's two brightest
  // texels span radii 0.477 to 0.492.
  void toneTracesAnArc(AudioVisualRig& rig) {
    auto visual = makeRadarVisual();
    AudioFrameSource source(AudioScene::Tone);
    const auto capture = rig.run(*visual, source, kTurnFrames, {.width = 450, .height = 600, .scale = 1});
    checkSound(capture);
    const auto arc = polarStats(capture.pixels, {.innerRadius = 0.479, .outerRadius = 0.49}, 60);
    const auto outer = polarStats(capture.pixels, {.innerRadius = 0.6, .outerRadius = 0.9}, 60);
    std::println("radar tone: {:.2f} of the arc ring lit, {:.3f} of the outer ring", share(arc), share(outer));
    TEST_CHECK(share(arc) > 0.9);
    TEST_CHECK(share(outer) < 0.05);
    // The arm is back at north: the arc just behind it is fresher than the arc half a turn old.
    const Polar freshArc{.innerRadius = 0.479, .outerRadius = 0.49, .fromBearing = 0.9, .toBearing = 0.98};
    const Polar oldArc{.innerRadius = 0.479, .outerRadius = 0.49, .fromBearing = 0.45, .toBearing = 0.55};
    const auto fresh = polarStats(capture.pixels, freshArc, 60);
    const auto old = polarStats(capture.pixels, oldArc, 60);
    TEST_CHECK(fresh.meanLuma > old.meanLuma + 30);
  }

  // Forwards to `inner`, with each frame's step replaced by the next of `steps`, round and round.
  class SteppedVisual final : public AudioVisual {
  public:
    SteppedVisual(AudioVisual& inner, std::span<const float> steps) : m_inner(inner), m_steps(steps) {}

    void reset() override { m_inner.reset(); }
    void advance(const luciole::workbench::AudioFrame& frame) override {
      auto stepped = frame;
      stepped.deltaSeconds = m_steps[m_next % m_steps.size()];
      ++m_next;
      m_turns += static_cast<double>(stepped.deltaSeconds) * luciole::workbench::kRadarTurnsPerSecond;
      m_inner.advance(stepped);
    }
    void draw(const AudioVisualDraw& draw) override { m_inner.draw(draw); }
    void release() override { m_inner.release(); }
    void abandon() noexcept override { m_inner.abandon(); }

    // The arm's bearing as a share of a turn.
    double bearing() const { return m_turns - std::floor(m_turns); }

  private:
    AudioVisual& m_inner;
    std::span<const float> m_steps;
    std::size_t m_next = 0;
    double m_turns = 0;
  };

  // The luma of the arc ring at radius 0.484, one sample per degree clockwise from north.
  std::array<int, 360> ringLumas(const test::GlPixels& image) {
    const auto scope =
        luciole::workbench::fitRadarScope(static_cast<float>(image.width), static_cast<float>(image.height));
    const double radius = 0.484 * scope.radius;
    std::array<int, 360> lumas{};
    for (std::size_t degree = 0; degree < lumas.size(); ++degree) {
      const double angle = static_cast<double>(degree) / 180 * std::numbers::pi;
      const auto x = static_cast<int>(scope.centerX + (radius * std::sin(angle)));
      const auto y = static_cast<int>(scope.centerY - (radius * std::cos(angle)));
      lumas[degree] = test::audio::luma(image.at(x, y));
    }
    return lumas;
  }

  // Degrees of the ring darker by more than `depth` than both samples three degrees away, which a smooth fade, the
  // step at the arm and the crosshair never are.
  int ringNotches(const std::array<int, 360>& lumas, int depth) {
    int notches = 0;
    for (std::size_t degree = 0; degree < lumas.size(); ++degree) {
      const int before = lumas[(degree + 357) % 360];
      const int after = lumas[(degree + 3) % 360];
      notches += lumas[degree] + depth < std::min(before, after) ? 1 : 0;
    }
    return notches;
  }

  // Frames longer than a row of the history, uneven or as long as a frame may be, still leave an unbroken arc.
  void unevenStepsLeaveAnUnbrokenArc(AudioVisualRig& rig) {
    constexpr float kSixtieth = 1.0F / 60;
    static constexpr std::array kDropped{
        kSixtieth, kSixtieth, kSixtieth, kSixtieth, kSixtieth,
        kSixtieth, kSixtieth, kSixtieth, kSixtieth, 2 * kSixtieth,
    };
    static constexpr std::array kThirtieth{1.0F / 30};
    static constexpr std::array kTenth{0.1F};
    constexpr std::array<std::tuple<std::span<const float>, int, std::string_view>, 3> kRuns{{
        {kDropped, 300, "dropped"},
        {kThirtieth, 150, "30fps"},
        {kTenth, 50, "10fps"},
    }};
    const AudioVisualTarget size{.width = 450, .height = 600, .scale = 1};
    for (const auto& [steps, frames, name] : kRuns) {
      auto radar = makeRadarVisual();
      SteppedVisual visual(*radar, steps);
      AudioFrameSource source(AudioScene::Tone);
      const auto capture = rig.run(visual, source, frames, size);
      checkSound(capture);
      const auto lumas = ringLumas(capture.pixels);
      const int notches = ringNotches(lumas, 15);
      const auto arc = polarStats(capture.pixels, {.innerRadius = 0.479, .outerRadius = 0.49}, 60);
      std::println(
          "radar tone at {} steps: {} notches, {:.2f} of the arc ring lit, arm at {:.3f}", name, notches, share(arc),
          visual.bearing()
      );
      TEST_CHECK(notches == 0);
      TEST_CHECK(share(arc) > 0.9);
      test::audio::keepForViewing(
          capture.pixels, test::audio::captureName("radar", std::format("tone-{}", name), size)
      );
      radar->release();
    }
  }

  // Rows the arm has not reached on its first turn stay dark, also just before north, where the spoke of the first
  // hit lies on the far side.
  void unreachedRowsStayDark(AudioVisualRig& rig) {
    auto visual = makeRadarVisual();
    AudioFrameSource source(AudioScene::Kicks);
    const auto capture = rig.run(*visual, source, 200, {.width = 900, .height = 500, .scale = 1.5F});
    checkSound(capture);
    const Polar ahead{.innerRadius = 0.4, .outerRadius = 0.9, .fromBearing = 0.9, .toBearing = 0.985};
    const auto stats = polarStats(capture.pixels, ahead, 60);
    std::println("radar ahead of the arm on its first turn: {:.3f} lit", share(stats));
    TEST_CHECK(stats.bright == 0);
  }

  // The arm turns clockwise a quarter turn a second: after 30 frames it points north-east, 30 frames later east.
  void armTurnsBetweenFrames(AudioVisualRig& rig) {
    auto visual = makeRadarVisual();
    AudioFrameSource source(AudioScene::Silence);
    const AudioVisualTarget size{.width = 900, .height = 500, .scale = 1.5F};
    const auto first = rig.run(*visual, source, 30, size);
    const auto firstArm = polarStats(first.pixels, {.innerRadius = 0.5, .outerRadius = 0.9}, 140);
    const auto second = rig.run(*visual, source, 30, size);
    const auto secondArm = polarStats(second.pixels, {.innerRadius = 0.5, .outerRadius = 0.9}, 140);
    std::println("radar arm at {:.4f} then {:.4f} of a turn", firstArm.brightBearing, secondArm.brightBearing);
    TEST_CHECK(firstArm.bright > 50 && secondArm.bright > 50);
    TEST_NEAR(firstArm.brightBearing, 0.125, 0.01);
    TEST_NEAR(secondArm.brightBearing, 0.25, 0.01);
  }

  // A hit writes a spoke along the whole radius under the arm, which stays behind as the arm moves on, and lifts the
  // whole scope for a moment.
  void kicksLeaveSpokes(AudioVisualRig& rig) {
    auto visual = makeRadarVisual();
    AudioFrameSource source(AudioScene::Kicks);
    const AudioVisualTarget size{.width = 450, .height = 600, .scale = 1.5F};
    const auto before = rig.run(*visual, source, 60, size);
    const auto hit = rig.run(*visual, source, 1, size);
    checkSound(before);
    checkSound(hit);
    // Frame 30 hit at 31 / 240 of a turn, in history row 24 of 192, which starts at 0.125; the next row still holds
    // the hit fading. Nothing hit between it and frame 60.
    const Polar spokeRows{.innerRadius = 0.4, .outerRadius = 0.9, .fromBearing = 0.126, .toBearing = 0.134};
    const Polar quietRows{.innerRadius = 0.4, .outerRadius = 0.9, .fromBearing = 0.17, .toBearing = 0.23};
    const auto spoke = polarStats(hit.pixels, spokeRows, 60);
    const auto between = polarStats(hit.pixels, quietRows, 60);
    std::println("radar spoke: {:.2f} lit, between spokes {:.3f}", share(spoke), share(between));
    TEST_CHECK(share(spoke) > 0.8);
    TEST_CHECK(share(between) < 0.02);
    // The hit lifts the whole scope: on the far side, where the arm has not been yet, the marks brighten.
    const Polar farSide{.innerRadius = 0, .outerRadius = 1.04, .fromBearing = 0.4, .toBearing = 0.95};
    const auto [rose, fell] = lumaChanges(before.pixels, hit.pixels, farSide, 1);
    std::println("radar hit: {} pixels of the far side brighter, {} darker", rose, fell);
    TEST_CHECK(rose > 1000);
    TEST_CHECK(fell == 0);
    test::audio::keepForViewing(hit.pixels, test::audio::captureName("radar", "hit", size));
  }

  // Silence shows the scope, its marks and the arm, and nothing else.
  void silenceShowsAnIdleScope(AudioVisualRig& rig) {
    auto visual = makeRadarVisual();
    AudioFrameSource source(AudioScene::Silence);
    const auto capture = rig.run(*visual, source, kTurnFrames, {.width = 900, .height = 500, .scale = 1});
    checkSound(capture);
    const auto background = test::audio::pixelOf(test::audio::rigPalette().background);
    const auto drawn =
        test::audio::pixelsOtherThan(capture.pixels, background, 4, test::audio::wholeRegion(capture.pixels));
    const double area = static_cast<double>(capture.pixels.width) * capture.pixels.height;
    std::println("radar silence: {:.3f} of the box drawn", drawn.count / area);
    TEST_CHECK(drawn.count > area * 0.03);
    // Away from the arm, which points north, no echo is lit: only the dim marks are drawn.
    const Polar face{.innerRadius = 0.1, .outerRadius = 0.9, .fromBearing = 0.05, .toBearing = 0.95};
    TEST_CHECK(polarStats(capture.pixels, face, 90).bright == 0);
  }

  void hostileFramesKeepTheContract(AudioVisualRig& rig) {
    auto visual = makeRadarVisual();
    AudioFrameSource source(AudioScene::Hostile);
    const auto capture = rig.run(*visual, source, 200, {.width = 450, .height = 600, .scale = 1.5F});
    checkSound(capture);
  }

  void sameFramesSamePixels(AudioVisualRig& rig) {
    const AudioVisualTarget size{.width = 450, .height = 600, .scale = 1.5F};
    auto first = makeRadarVisual();
    auto second = makeRadarVisual();
    AudioFrameSource firstSource(AudioScene::Noise);
    AudioFrameSource secondSource(AudioScene::Noise);
    const auto a = rig.run(*first, firstSource, 90, size);
    const auto b = rig.run(*second, secondSource, 90, size);
    TEST_CHECK(test::audio::differingPixels(a.pixels, b.pixels, 0) == 0);
  }

  // After release the next draw rebuilds the texture from memory: the picture is the one never released.
  void releaseRebuildsFromMemory(AudioVisualRig& rig) {
    const AudioVisualTarget size{.width = 450, .height = 600, .scale = 1};
    auto kept = makeRadarVisual();
    auto released = makeRadarVisual();
    AudioFrameSource keptSource(AudioScene::Tone);
    AudioFrameSource releasedSource(AudioScene::Tone);
    (void)rig.run(*kept, keptSource, 100, size);
    (void)rig.run(*released, releasedSource, 100, size);
    released->release();
    const auto a = rig.run(*kept, keptSource, 1, size);
    const auto b = rig.run(*released, releasedSource, 1, size);
    TEST_CHECK(test::audio::differingPixels(a.pixels, b.pixels, 0) == 0);
    // A reset forgets the arc.
    released->reset();
    const auto fresh = rig.run(*released, releasedSource, 1, size);
    TEST_CHECK(polarStats(fresh.pixels, {.innerRadius = 0.479, .outerRadius = 0.49}, 60).bright < 100);
    kept->release();
    released->release();
  }

  // An opacity that is not a number draws nothing.
  void brokenOpacityDrawsNothing(AudioVisualRig& rig) {
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    auto visual = makeRadarVisual();
    AudioFrameSource source(AudioScene::Noise);
    const auto capture = rig.run(*visual, source, 30, {.width = 450, .height = 600, .scale = 1, .opacity = kNan});
    checkSound(capture);
    const auto background = test::audio::pixelOf(test::audio::rigPalette().background);
    TEST_CHECK(
        test::audio::pixelsOtherThan(capture.pixels, background, 0, test::audio::wholeRegion(capture.pixels)).count == 0
    );
    visual->release();
  }

  void emptyBoxDrawsNothing(AudioVisualRig& rig) {
    auto visual = makeRadarVisual();
    AudioFrameSource source(AudioScene::Tone);
    for (const auto& size :
         {AudioVisualTarget{.width = 0, .height = 0, .scale = 1},
          AudioVisualTarget{.width = 300, .height = 0, .scale = 1.5F}}) {
      const auto capture = rig.run(*visual, source, 3, size);
      TEST_CHECK(capture.pixels.rgba.empty());
      TEST_CHECK(capture.insideOnly && capture.stateKept);
    }
  }

  // Every scene at every capture size, kept for viewing when LUCIOLE_CAPTURE_DIR is set.
  void capturesEverySize(AudioVisualRig& rig) {
    constexpr std::array<std::pair<AudioScene, std::string_view>, 5> kScenes{{
        {AudioScene::Tone, "tone"},
        {AudioScene::Noise, "noise"},
        {AudioScene::Kicks, "kicks"},
        {AudioScene::Silence, "silence"},
        {AudioScene::Hostile, "hostile"},
    }};
    for (const auto& [scene, name] : kScenes) {
      for (const auto& size : test::audio::kCaptureTargets) {
        auto visual = makeRadarVisual();
        AudioFrameSource source(scene);
        const auto capture = rig.run(*visual, source, 200, size);
        checkSound(capture);
        std::println(
            "radar {} {}x{} at {}: {:.4f} ms mean, {:.4f} ms max processor time per frame", name, size.width,
            size.height, size.scale, capture.meanCpuMilliseconds, capture.maxCpuMilliseconds
        );
        test::audio::keepForViewing(capture.pixels, test::audio::captureName("radar", name, size));
        visual->release();
      }
    }
  }
} // namespace

int main() {
  std::string reason;
  auto rig = AudioVisualRig::create(reason);
  if (!rig) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  toneTracesAnArc(*rig);
  unevenStepsLeaveAnUnbrokenArc(*rig);
  unreachedRowsStayDark(*rig);
  armTurnsBetweenFrames(*rig);
  kicksLeaveSpokes(*rig);
  silenceShowsAnIdleScope(*rig);
  hostileFramesKeepTheContract(*rig);
  sameFramesSamePixels(*rig);
  releaseRebuildsFromMemory(*rig);
  brokenOpacityDrawsNothing(*rig);
  emptyBoxDrawsNothing(*rig);
  capturesEverySize(*rig);
  std::println("radar visual checks passed");
  return 0;
}
