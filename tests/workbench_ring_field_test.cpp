#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/ring_field.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  // A rising ramp that crosses zero at `zero`, scaled down so it stays small.
  std::vector<float> ramp(std::size_t size, double zero) {
    std::vector<float> samples(size);
    for (std::size_t i = 0; i < size; ++i) {
      samples[i] = static_cast<float>((static_cast<double>(i) - zero) * 0.01);
    }
    return samples;
  }

  void startsAtTheFirstRisingCrossing() {
    TEST_NEAR(ringStart(ramp(40, 10.25), 20), 10.25, 1e-4);
    // A falling crossing comes first and is skipped.
    std::vector<float> samples(40, 0.5F);
    samples[3] = -0.5F;
    samples[4] = -0.5F;
    samples[5] = 0.5F;
    TEST_NEAR(ringStart(samples, 20), 4.5, 1e-6);
    // A crossing that leaves less than a full window after it is not taken.
    TEST_NEAR(ringStart(ramp(40, 25.5), 20), 0.0, 0.0);
    // Too few samples, or none.
    TEST_NEAR(ringStart(ramp(30, 10.25), 20), 0.0, 0.0);
    TEST_NEAR(ringStart({}, 20), 0.0, 0.0);
    TEST_NEAR(ringStart(ramp(40, 10.25), 0), 0.0, 0.0);
  }

  void resamplesWithoutAveraging() {
    std::vector<float> samples(12);
    for (std::size_t i = 0; i < samples.size(); ++i) {
      samples[i] = 0.1F * static_cast<float>(i);
    }
    std::array<float, 4> out{};
    const float peak = resampleRing(samples, 1.5, 8, out);
    TEST_NEAR(out[0], 0.15F, 1e-6);
    TEST_NEAR(out[1], 0.35F, 1e-6);
    TEST_NEAR(out[2], 0.55F, 1e-6);
    TEST_NEAR(out[3], 0.75F, 1e-6);
    TEST_NEAR(peak, 0.75F, 1e-6);
    // Points past the end read the last sample.
    const float end = resampleRing(samples, 10.0, 8, out);
    TEST_NEAR(out[3], 1.1F, 1e-6);
    TEST_NEAR(end, 1.1F, 1e-6);
    // Nothing to read gives a flat ring.
    TEST_NEAR(resampleRing({}, 0.0, 8, out), 0.0F, 0.0);
    TEST_CHECK(std::ranges::all_of(out, [](float v) { return v == 0.0F; }));
  }

  void tracksThePeak() {
    TEST_NEAR(nextRingPeak(0.5F, 0.3F), 0.496F, 1e-6);
    TEST_NEAR(nextRingPeak(0.5F, 0.6F), 0.6F, 1e-6);
  }

  void finishesWithATaperAndAClamp() {
    std::vector<float> ring(100, 0.4F);
    finishRing(ring, 0.5F);
    TEST_NEAR(ring[0], 0.0F, 1e-6);
    TEST_NEAR(ring[99], 0.0F, 1e-6);
    // Eight points of taper at each end: halfway up at the fourth.
    TEST_NEAR(ring[4], 0.4F, 1e-6);
    TEST_NEAR(ring[95], 0.4F, 1e-6);
    TEST_NEAR(ring[50], 0.8F, 1e-6);
    std::vector<float> loud(100, -0.4F);
    finishRing(loud, 0.001F); // the floor of 0.02 divides, and the result is clamped
    TEST_NEAR(loud[50], -1.5F, 1e-6);
  }

  void pulsesOnTheBass() {
    TEST_NEAR(ringPulseTarget(1), 0.0F, 0.0);
    TEST_NEAR(ringPulseTarget(1.5F), 0.6F, 1e-6);
    TEST_NEAR(ringPulseTarget(3), 1.5F, 0.0);
    TEST_NEAR(ringPulseTarget(std::numeric_limits<float>::quiet_NaN()), 0.0F, 0.0);
    TEST_NEAR(stepRingPulse(0, 3, 1.0F / 60), 0.38877F, 1e-4);
    TEST_NEAR(stepRingPulse(1.5F, 0, 1.0F / 60), 1.40326F, 1e-4);
    TEST_NEAR(stepRingPulse(1, 1.5F, 0), 1.0F, 0.0);
  }

  void cameraTurnsAndFramesTheSphere() {
    RingCamera camera;
    const RingCamera start = camera;
    for (int i = 0; i < 600; ++i) {
      camera = stepRingCamera(camera, 1.0F / 60);
    }
    TEST_NEAR(camera.yaw - start.yaw, 1.2F, 1e-3);
    TEST_CHECK(camera.pitch > 0.12F && camera.pitch < 0.72F);
    TEST_CHECK(camera.pitch != start.pitch);
    TEST_NEAR(stepRingCamera(camera, 0).yaw, camera.yaw, 0.0);

    const auto view = ringView(camera, 1.5F);
    TEST_NEAR(view.distance, 4.67694F, 1e-4);
    TEST_NEAR(ringView(camera, 0.5F).distance, 2 * 4.67694F, 1e-3);
    TEST_NEAR(ringView(camera, 0).distance, 4.67694F, 1e-4);
    TEST_NEAR(ringView(camera, std::numeric_limits<float>::quiet_NaN()).distance, 4.67694F, 1e-4);
    // The sphere's center lies straight ahead at the camera distance.
    const auto& v = view.view;
    TEST_NEAR(v[12], 0.0F, 1e-5);
    TEST_NEAR(v[13], 0.0F, 1e-5);
    TEST_NEAR(v[14], -view.distance, 1e-4);
    // Every point of the swollen sphere's outline projects inside the frame.
    for (const auto aspect : {0.5F, 1.0F, 1.8F}) {
      const auto framed = ringView(camera, aspect);
      for (int k = 0; k < 64; ++k) {
        const double angle = 2 * std::numbers::pi * k / 64;
        const auto x = static_cast<float>(1.3 * std::cos(angle));
        const auto y = static_cast<float>(1.3 * std::sin(angle));
        const auto& p = framed.projection;
        const float clipX = p[0] * x;
        const float clipY = p[5] * y;
        const float w = framed.distance;
        TEST_CHECK(std::abs(clipX / w) < 1 && std::abs(clipY / w) < 1);
      }
    }
    // The swollen sphere's nearest point, center and farthest point lie between the near and far planes, also in a
    // rectangle so narrow that the eye moves far back.
    for (const auto aspect : {0.05F, 0.5F, 1.8F}) {
      const auto framed = ringView(camera, aspect);
      const auto& p = framed.projection;
      for (const float offset : {-1.6F, 0.0F, 1.6F}) {
        const float z = -(framed.distance + offset);
        const float depth = ((p[10] * z) + p[14]) / -z;
        TEST_CHECK(depth > -1 && depth < 1);
      }
    }
  }

  void pushesRingsAtAFixedRate() {
    RingField field;
    AudioFrameSource source(AudioScene::Tone);
    for (int i = 0; i < 7 * test::audio::kFramesPerSecond; ++i) {
      field.advance(source.next());
    }
    TEST_CHECK(field.pushes() == kRingCount || field.pushes() == kRingCount - 1);
    TEST_CHECK(field.count() == field.pushes());
    TEST_CHECK(field.fraction() >= 0 && field.fraction() < 1);
    for (int i = 0; i < 60; ++i) {
      field.advance(source.next());
    }
    TEST_CHECK(field.count() == kRingCount);
    TEST_CHECK(field.pushes() > kRingCount);
  }

  // The newest ring and the one before it, from one steady tone, are the same ring.
  void steadyToneGivesIdenticalRings() {
    RingField field;
    AudioFrameSource source(AudioScene::Tone);
    for (int i = 0; i < 120; ++i) {
      field.advance(source.next());
    }
    const auto rows = field.rows();
    const std::size_t newest = field.head() * kRingPoints;
    const std::size_t before = ((field.head() + kRingCount - 1) % kRingCount) * kRingPoints;
    float largest = 0;
    float difference = 0;
    for (std::size_t j = 0; j < kRingPoints; ++j) {
      largest = std::max(largest, std::abs(rows[newest + j]));
      difference = std::max(difference, std::abs(rows[newest + j] - rows[before + j]));
    }
    TEST_CHECK(largest > 0.9F);
    TEST_CHECK(difference < 0.01F);
  }

  void silenceGivesFlatRings() {
    RingField field;
    AudioFrameSource source(AudioScene::Silence);
    for (int i = 0; i < 60; ++i) {
      field.advance(source.next());
    }
    TEST_CHECK(field.count() > 0);
    TEST_CHECK(std::ranges::all_of(field.rows(), [](float v) { return v == 0.0F; }));
    TEST_NEAR(field.pulse(), 0.0F, 0.0);
  }

  void kicksSwellTheForm() {
    RingField field;
    AudioFrameSource source(AudioScene::Kicks);
    float justAfter = 0;
    float beforeNext = 0;
    for (int i = 0; i < 4 * test::audio::kKickFrames; ++i) {
      field.advance(source.next());
      if (i % test::audio::kKickFrames == 3) {
        justAfter = field.pulse();
      }
      if (i % test::audio::kKickFrames == test::audio::kKickFrames - 1) {
        beforeNext = field.pulse();
      }
    }
    // The pulse rises within a few frames of a hit and falls back slowly.
    TEST_CHECK(justAfter > 0.85F);
    TEST_CHECK(beforeNext < 0.25F);
    TEST_NEAR(ringSwell(justAfter), 1 + (kRingSwell * justAfter), 1e-6);
    TEST_CHECK(ringSwell(justAfter) > 1.017F && ringSwell(kRingPulseMax) < 1.04F);
    TEST_NEAR(ringSwell(0), 1.0F, 0.0);
  }

  void hostileFramesStayBounded() {
    RingField field;
    AudioFrameSource source(AudioScene::Hostile);
    for (int i = 0; i < 400; ++i) {
      const auto before = field.pushes();
      field.advance(source.next());
      // A step is never longer than kMaxAudioFrameSeconds, so at most two rings come in one frame.
      TEST_CHECK(field.pushes() - before <= 2);
    }
    TEST_CHECK(field.count() > 0);
    TEST_CHECK(std::ranges::all_of(field.rows(), [](float v) {
      return std::isfinite(v) && std::abs(v) <= kRingClamp;
    }));
    TEST_CHECK(std::isfinite(field.pulse()) && field.pulse() >= 0 && field.pulse() <= kRingPulseMax);
    TEST_CHECK(std::isfinite(field.camera().yaw) && std::isfinite(field.camera().pitch));
    TEST_CHECK(std::isfinite(field.fraction()) && field.fraction() >= 0 && field.fraction() < 1);
  }

  void sameFramesSameField() {
    RingField a;
    RingField b;
    AudioFrameSource first(AudioScene::Noise);
    AudioFrameSource second(AudioScene::Noise);
    for (int i = 0; i < 200; ++i) {
      a.advance(first.next());
      b.advance(second.next());
    }
    TEST_CHECK(a.head() == b.head() && a.pushes() == b.pushes());
    TEST_CHECK(std::ranges::equal(a.rows(), b.rows()));
    TEST_CHECK(a.fraction() == b.fraction() && a.pulse() == b.pulse());
  }

  void resetForgetsEverything() {
    RingField field;
    AudioFrameSource source(AudioScene::Kicks);
    for (int i = 0; i < 90; ++i) {
      field.advance(source.next());
    }
    field.reset();
    const RingField fresh;
    TEST_CHECK(field.pushes() == 0 && field.count() == 0 && field.head() == fresh.head());
    TEST_CHECK(std::ranges::all_of(field.rows(), [](float v) { return v == 0.0F; }));
    TEST_CHECK(field.fraction() == 0 && field.pulse() == 0);
    TEST_CHECK(field.camera().yaw == fresh.camera().yaw && field.camera().pitch == fresh.camera().pitch);
  }
} // namespace

int main() {
  startsAtTheFirstRisingCrossing();
  resamplesWithoutAveraging();
  tracksThePeak();
  finishesWithATaperAndAClamp();
  pulsesOnTheBass();
  cameraTurnsAndFramesTheSphere();
  pushesRingsAtAFixedRate();
  steadyToneGivesIdenticalRings();
  silenceGivesFlatRings();
  kicksSwellTheForm();
  hostileFramesStayBounded();
  sameFramesSameField();
  resetForgetsEverything();
  std::println("ring field checks passed");
  return 0;
}
