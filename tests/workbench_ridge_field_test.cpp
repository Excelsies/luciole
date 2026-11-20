#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/ridge_field.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <print>
#include <span>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

  // Row `age` pushes old, 0 the newest.
  std::span<const float> rowOf(const RidgeField& field, std::size_t age) {
    const std::size_t row = (field.head() + kRidgeLines - age) % kRidgeLines;
    return field.rows().subspan(row * kRidgePoints, kRidgePoints);
  }

  float largest(std::span<const float> values) {
    float most = 0;
    for (const float v : values) {
      most = std::max(most, std::abs(v));
    }
    return most;
  }

  void envelopeIsFlatInTheMiddleAndZeroAtTheSides() {
    std::vector<float> envelope(101);
    ridgeEnvelope(envelope);
    TEST_NEAR(envelope[50], 1.0F, 0.0);
    TEST_NEAR(envelope[45], 1.0F, 0.0); // inside the flat top
    TEST_NEAR(envelope[40], 0.892208F, 1e-5);
    TEST_NEAR(envelope[60], 0.892208F, 1e-5);
    TEST_NEAR(envelope[0], 0.0F, 0.0);
    TEST_NEAR(envelope[22], 0.0F, 0.0);
    TEST_NEAR(envelope[100], 0.0F, 0.0);
    std::array<float, 1> one{};
    ridgeEnvelope(one);
    TEST_NEAR(one[0], 1.0F, 0.0);
  }

  void readsBandsBetweenBands() {
    const std::array<float, 3> bands{0, 1, 0.5F};
    TEST_NEAR(bandAt(bands, 0.5F), 0.5F, 1e-6);
    TEST_NEAR(bandAt(bands, 1.5F), 0.75F, 1e-6);
    TEST_NEAR(bandAt(bands, -1), 0.0F, 0.0);
    TEST_NEAR(bandAt(bands, 5), 0.5F, 0.0);
    TEST_NEAR(bandAt(bands, kNan), 0.0F, 0.0);
    TEST_NEAR(bandAt({}, 1), 0.0F, 0.0);
  }

  void gainRisesQuicklyAndFallsSlowly() {
    RidgeGain rising;
    rising.step(1, 1);
    TEST_NEAR(rising.level, 0.81606F, 1e-5);
    TEST_NEAR(rising.factor(), 1.04159F, 1e-4);
    RidgeGain falling;
    falling.step(0, 5);
    TEST_NEAR(falling.level, 0.18394F, 1e-5);
    TEST_NEAR(falling.factor(), 4.62108F, 1e-3);
    // Silence is not boosted past a bound.
    for (int i = 0; i < 100; ++i) {
      falling.step(0, 5);
    }
    TEST_NEAR(falling.factor(), 0.85F / 0.15F, 1e-4);
    RidgeGain hostile;
    hostile.step(kNan, 1);
    hostile.step(1, kNan);
    TEST_CHECK(std::isfinite(hostile.level) && std::isfinite(hostile.factor()));
  }

  void buildsALineFromBandsAndWaveform() {
    std::vector<float> envelope(kRidgePoints);
    ridgeEnvelope(envelope);
    const std::vector<float> silentWave(kAudioWaveformSamples, 0.0F);
    const std::vector<float> fullBands(kAudioBandCount, 1.0F);
    const std::vector<float> noBands(kAudioBandCount, 0.0F);
    std::vector<float> line(kRidgePoints);

    // Bass at the center, a still waveform: a peak of known height and flat sides.
    TEST_NEAR(buildRidge(line, envelope, fullBands, silentWave, 1, 7), 1.0F, 1e-6);
    TEST_NEAR(line[48], 0.53114F, 0.005);
    TEST_CHECK(std::abs(line[0]) <= 0.0041F && std::abs(line[96]) <= 0.0041F);
    TEST_CHECK(std::abs(line[10]) <= 0.0041F);

    // No sound: only the faint wiggle that keeps a line from being ruled straight.
    TEST_NEAR(buildRidge(line, envelope, noBands, silentWave, 1, 7), 0.0F, 0.0);
    TEST_CHECK(largest(line) <= 0.0041F && largest(line) > 0);

    // A wrong-sized row or envelope writes nothing.
    std::vector<float> shortLine(10, 5.0F);
    TEST_NEAR(buildRidge(shortLine, envelope, fullBands, silentWave, 1, 7), 0.0F, 0.0);
    TEST_CHECK(std::ranges::all_of(shortLine, [](float v) { return v == 5.0F; }));
  }

  void slopesAreCentralDifferences() {
    const std::array<float, 3> row{0, 1, 4};
    TEST_NEAR(ridgeSlope(row, 0), 1.0F, 0.0);
    TEST_NEAR(ridgeSlope(row, 1), 2.0F, 0.0);
    TEST_NEAR(ridgeSlope(row, 2), 3.0F, 0.0);
    TEST_NEAR(ridgeSlope(row, 9), 0.0F, 0.0);
    const std::array<float, 1> single{3};
    TEST_NEAR(ridgeSlope(single, 0), 0.0F, 0.0);
  }

  void layoutIsAPortraitPlotInTheMiddle() {
    const auto tall = ridgeLayout(450, 600);
    TEST_NEAR(tall.left, 27.0F, 1e-3);
    TEST_NEAR(tall.width, 396.0F, 1e-3);
    TEST_NEAR(tall.bottom, 558.0F, 1e-3);
    TEST_NEAR(tall.top, 96.18F, 1e-3);
    TEST_NEAR(tall.spacing, (558.0F - 96.18F) / 79, 1e-3);
    TEST_NEAR(tall.amplitude, 77.4F, 1e-3);
    const auto wide = ridgeLayout(900, 500);
    TEST_NEAR(wide.width, 335.4F, 1e-3);
    TEST_NEAR(wide.left, 282.3F, 1e-3);
    for (const auto& empty : {ridgeLayout(0, 500), ridgeLayout(900, -1), ridgeLayout(kNan, 500)}) {
      TEST_NEAR(empty.width, 0.0F, 0.0);
      TEST_NEAR(empty.amplitude, 0.0F, 0.0);
      TEST_NEAR(empty.spacing, 0.0F, 0.0);
    }
  }

  void pushesTenLinesASecond() {
    RidgeField field;
    TEST_CHECK(largest(field.rows()) == 0); // flat lines before any sound
    AudioFrameSource source(AudioScene::Tone);
    for (int i = 0; i < test::audio::kFramesPerSecond; ++i) {
      field.advance(source.next());
    }
    TEST_CHECK(field.pushes() == 10 || field.pushes() == 9);
    TEST_CHECK(field.fraction() >= 0 && field.fraction() < 1);
  }

  // The tone sits in band 30: two humps either side of the center, nothing at the center or at the sides.
  void toneRaisesHumpsOffCenter() {
    RidgeField field;
    AudioFrameSource source(AudioScene::Tone);
    for (int i = 0; i < 120; ++i) {
      field.advance(source.next());
    }
    const auto line = rowOf(field, 0);
    TEST_CHECK(largest(line.subspan(44, 9)) < 0.05F);
    TEST_CHECK(largest(line.subspan(24, 18)) > 0.2F);
    TEST_CHECK(largest(line.subspan(55, 18)) > 0.2F);
    TEST_CHECK(largest(line.subspan(0, 18)) < 0.05F);
    TEST_CHECK(largest(line.subspan(79, 18)) < 0.05F);
  }

  // A kick fills the lowest bands: a tall spike at the center of the line pushed just after it, once the gain has
  // settled over two kicks.
  void kickRaisesTheCenter() {
    RidgeField field;
    AudioFrameSource source(AudioScene::Kicks);
    for (int i = 0; i < (2 * test::audio::kKickFrames) + 6; ++i) {
      field.advance(source.next());
    }
    const auto line = rowOf(field, 0);
    TEST_CHECK(largest(line.subspan(44, 9)) > 0.3F);
    TEST_CHECK(largest(line.subspan(0, 18)) < 0.05F);
    // Between kicks the bands have died down and the center is low again.
    for (int i = 0; i < 18; ++i) {
      field.advance(source.next());
    }
    TEST_CHECK(largest(rowOf(field, 0).subspan(44, 9)) < 0.1F);
  }

  void silenceGivesCalmLines() {
    RidgeField field;
    AudioFrameSource source(AudioScene::Silence);
    for (int i = 0; i < 600; ++i) {
      field.advance(source.next());
    }
    TEST_CHECK(field.pushes() >= 99);
    TEST_CHECK(largest(field.rows()) <= 0.0041F);
  }

  void hostileFramesStayBounded() {
    RidgeField field;
    AudioFrameSource source(AudioScene::Hostile);
    for (int i = 0; i < 400; ++i) {
      const auto before = field.pushes();
      field.advance(source.next());
      TEST_CHECK(field.pushes() - before <= 1);
    }
    TEST_CHECK(field.pushes() > 0);
    TEST_CHECK(std::ranges::all_of(field.rows(), [](float v) { return std::isfinite(v) && std::abs(v) < 3; }));
    TEST_CHECK(std::isfinite(field.fraction()) && field.fraction() >= 0 && field.fraction() < 1);
  }

  void sameFramesSameField() {
    RidgeField a;
    RidgeField b;
    AudioFrameSource first(AudioScene::Noise);
    AudioFrameSource second(AudioScene::Noise);
    for (int i = 0; i < 200; ++i) {
      a.advance(first.next());
      b.advance(second.next());
    }
    TEST_CHECK(a.head() == b.head() && a.pushes() == b.pushes() && a.fraction() == b.fraction());
    TEST_CHECK(std::ranges::equal(a.rows(), b.rows()));
    // Noise gives a different jagged line each time.
    TEST_CHECK(!std::ranges::equal(rowOf(a, 0), rowOf(a, 1)));
  }

  void resetForgetsEverything() {
    RidgeField field;
    AudioFrameSource source(AudioScene::Noise);
    for (int i = 0; i < 90; ++i) {
      field.advance(source.next());
    }
    field.reset();
    const RidgeField fresh;
    TEST_CHECK(field.pushes() == 0 && field.head() == fresh.head() && field.fraction() == 0);
    TEST_CHECK(largest(field.rows()) == 0);
    AudioFrameSource again(AudioScene::Noise);
    RidgeField other;
    for (int i = 0; i < 30; ++i) {
      const auto frame = again.next();
      field.advance(frame);
      other.advance(frame);
    }
    TEST_CHECK(std::ranges::equal(field.rows(), other.rows()));
  }
} // namespace

int main() {
  envelopeIsFlatInTheMiddleAndZeroAtTheSides();
  readsBandsBetweenBands();
  gainRisesQuicklyAndFallsSlowly();
  buildsALineFromBandsAndWaveform();
  slopesAreCentralDifferences();
  layoutIsAPortraitPlotInTheMiddle();
  pushesTenLinesASecond();
  toneRaisesHumpsOffCenter();
  kickRaisesTheCenter();
  silenceGivesCalmLines();
  hostileFramesStayBounded();
  sameFramesSameField();
  resetForgetsEverything();
  std::println("ridge field checks passed");
  return 0;
}
