#include "cpu_budget.h"
#include "test_check.h"
#include "workbench/audio_analysis.h"
#include "workbench/audio_frame.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <limits>
#include <numbers>
#include <print>
#include <span>
#include <vector>

namespace {
  using namespace luciole::workbench;

  constexpr double kFramesPerSecond = 60;
  constexpr double kFrameStep = 1.0 / kFramesPerSecond;
  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInfinity = std::numeric_limits<float>::infinity();

  // A continuous mono signal: the value of sample `index` at kAudioWaveformRate.
  using Signal = std::function<float(std::int64_t index)>;

  // The window that ends at frame `frame` of a stream drawn at `fps`.
  std::vector<float> windowAt(const Signal& signal, int frame, double fps = kFramesPerSecond) {
    const auto newest = static_cast<std::int64_t>(std::floor(frame / fps * kAudioWaveformRate));
    std::vector<float> samples(kAudioWaveformSamples);
    for (std::size_t i = 0; i < samples.size(); ++i) {
      samples[i] = signal(newest - static_cast<std::int64_t>(kAudioWaveformSamples - 1 - i));
    }
    return samples;
  }

  Signal sine(double hz, float amplitude) {
    return [hz, amplitude](std::int64_t index) {
      const double seconds = static_cast<double>(index) / kAudioWaveformRate;
      return amplitude * static_cast<float>(std::sin(2 * std::numbers::pi * hz * seconds));
    };
  }

  // A fixed pseudo-random value in [0, 1) for each index.
  float hashed(std::int64_t index) {
    auto x = static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return static_cast<float>((x ^ (x >> 31U)) >> 40U) / static_cast<float>(1U << 24U);
  }

  float whiteNoise(std::int64_t index) { return hashed(index) - 0.5F; }

  // A decaying 55 Hz thump every `period` seconds from time 0, silence before.
  Signal kicks(double period) {
    return [period](std::int64_t index) {
      if (index < 0) {
        return 0.0F;
      }
      const double age = std::fmod(static_cast<double>(index) / kAudioWaveformRate, period);
      return 0.9F * static_cast<float>(std::exp(-age / 0.08) * std::sin(2 * std::numbers::pi * 55 * age));
    };
  }

  // Analyzes `frames` frames of `signal` from frame `first`, returning the last.
  AudioFrame play(AudioAnalysis& analysis, const Signal& signal, int first, int frames) {
    AudioFrame frame;
    for (int i = 0; i < frames; ++i) {
      const auto samples = windowAt(signal, first + i);
      frame = analysis.analyze(samples, kFrameStep);
    }
    return frame;
  }

  std::size_t loudestBand(const AudioFrame& frame) {
    std::size_t loudest = 0;
    for (std::size_t band = 1; band < frame.bands.size(); ++band) {
      if (frame.bands[band] > frame.bands[loudest]) {
        loudest = band;
      }
    }
    return loudest;
  }

  // A frame with no sound in it: no waveform, no bass, level or hit.
  void checkQuiet(const AudioFrame& frame) {
    TEST_CHECK(frame.waveform.empty());
    TEST_CHECK(frame.bass == 0.0F);
    TEST_CHECK(frame.level == 0.0F);
    TEST_CHECK(!frame.hit);
    TEST_CHECK(frame.bands.size() == kAudioBandCount);
  }

  void checkBandsZero(const AudioFrame& frame) {
    TEST_CHECK(frame.bands.size() == kAudioBandCount);
    for (const float band : frame.bands) {
      TEST_CHECK(band == 0.0F);
    }
  }

  // Bands spaced from 40 Hz to 10 kHz: 100 Hz falls in band 10, 1 kHz in band 37 and 8 kHz in band 61. A low tone
  // spreads over more bands, which are narrower there than the transform's resolution.
  void testToneLightsItsBandAndNeighborsOnly() {
    struct Case {
      double hz;
      std::size_t band;
      std::size_t spread;
    };
    for (const auto& [hz, band, spread] : {Case{100, 10, 5}, Case{1000, 37, 1}, Case{8000, 61, 1}}) {
      AudioAnalysis analysis;
      const auto frame = play(analysis, sine(hz, 0.1F), 0, 60);
      TEST_CHECK(loudestBand(frame) == band);
      TEST_CHECK(frame.bands[band] > 0.4F);
      // Only the low tone has bass, steady at its own average.
      TEST_NEAR(frame.bass, hz < 150 ? 1.0F : 0.0F, 0.01F);
      for (std::size_t other = 0; other < frame.bands.size(); ++other) {
        const auto distance = other > band ? other - band : band - other;
        if (distance > spread) {
          TEST_CHECK(frame.bands[other] < 0.1F);
        }
      }
    }
  }

  void testWhiteNoiseIsFlat() {
    AudioAnalysis analysis;
    std::vector<double> sums(kAudioBandCount, 0.0);
    constexpr int kFrames = 60;
    for (int i = 0; i < kFrames; ++i) {
      const auto samples = windowAt(whiteNoise, 60 + i);
      const auto frame = analysis.analyze(samples, kFrameStep);
      for (std::size_t band = 0; band < kAudioBandCount; ++band) {
        sums[band] += frame.bands[band];
      }
    }
    const auto [low, high] = std::ranges::minmax(sums);
    TEST_CHECK(low / kFrames > 0.4);
    TEST_CHECK(TEST_WITHIN_BUDGET(high / kFrames, 0.75));
    TEST_CHECK(TEST_WITHIN_BUDGET((high - low) / kFrames, 0.2));
  }

  // Frames of `period`-second kicks over four seconds at `fps`: one hit per kick, at most 75 ms after it starts.
  void checkKicksAtPeriod(double period, double fps) {
    constexpr double kLatestHit = 0.075;
    AudioAnalysis analysis;
    const auto signal = kicks(period);
    const int frames = static_cast<int>(std::lround(4 * fps));
    std::vector<double> hits;
    for (int i = 0; i < frames; ++i) {
      const auto samples = windowAt(signal, i, fps);
      if (analysis.analyze(samples, 1 / fps).hit) {
        hits.push_back(i / fps);
      }
    }
    const double lastFrame = (frames - 1) / fps;
    const auto kickCount = static_cast<std::size_t>(std::floor((lastFrame - kLatestHit) / period)) + 1;
    TEST_CHECK(hits.size() == kickCount);
    for (std::size_t kick = 0; kick < std::min(hits.size(), kickCount); ++kick) {
      const double start = static_cast<double>(kick) * period;
      TEST_CHECK(hits[kick] >= start);
      TEST_CHECK(hits[kick] <= start + kLatestHit);
    }
  }

  void testKicksHitOncePerKick() {
    for (const double period : {1.0, 0.5, 1.0 / 3, 0.25}) {
      checkKicksAtPeriod(period, kFramesPerSecond);
    }
    checkKicksAtPeriod(0.5, 144);
    checkKicksAtPeriod(0.5, 30);
  }

  void testSteadySoundDoesNotHit() {
    for (const Signal& signal : {sine(100, 0.5F), Signal{whiteNoise}}) {
      AudioAnalysis analysis;
      for (int i = 0; i < 300; ++i) {
        const auto samples = windowAt(signal, i);
        const bool hit = analysis.analyze(samples, kFrameStep).hit;
        // Sound starting after silence is a hit.
        TEST_CHECK(hit == (i == 0));
      }
    }
  }

  // Noise that turns ten times louder and stays loud is one hit, however long it stays above the old average.
  void testLouderSoundHitsOnce() {
    const Signal swell = [](std::int64_t index) {
      return (index < 2 * kAudioWaveformRate ? 0.05F : 0.5F) * whiteNoise(index);
    };
    AudioAnalysis analysis;
    std::vector<int> hits;
    for (int i = 0; i < 240; ++i) {
      const auto samples = windowAt(swell, i);
      if (analysis.analyze(samples, kFrameStep).hit) {
        hits.push_back(i);
      }
    }
    TEST_CHECK(hits.size() == 2);
    TEST_CHECK(hits.front() == 0);
    TEST_CHECK(hits.back() >= 120 && hits.back() <= 125);
  }

  // Clicks closer together than the refractory time never give two hits less than 0.1 s apart.
  void testHitsKeepTheirDistance() {
    constexpr std::int64_t kSpacing = kAudioWaveformRate * 7 / 100;
    const Signal clicks = [](std::int64_t index) {
      const std::int64_t offset = index < 0 ? kSpacing : index % kSpacing;
      return offset < 44 ? 0.9F * static_cast<float>(std::sin(std::numbers::pi * static_cast<double>(offset) / 22))
                         : 0.0F;
    };
    AudioAnalysis analysis;
    std::vector<int> hits;
    for (int i = 0; i < 600; ++i) {
      const auto samples = windowAt(clicks, i, 600);
      if (analysis.analyze(samples, 1.0 / 600).hit) {
        hits.push_back(i);
      }
    }
    TEST_CHECK(hits.size() >= 3);
    for (std::size_t i = 1; i < hits.size(); ++i) {
      TEST_CHECK(hits[i] - hits[i - 1] >= 60);
    }
  }

  void testBassFollowsKicksAndReturnsToOne() {
    AudioAnalysis analysis;
    const auto beat = kicks(0.5);
    float atHit = 0;
    float between = kMaxAudioBass;
    for (int i = 0; i < 240; ++i) {
      const auto samples = windowAt(beat, i);
      const auto frame = analysis.analyze(samples, kFrameStep);
      if (i >= 120 && i % 30 == 3) {
        atHit = std::max(atHit, frame.bass);
      }
      if (i >= 120 && i % 30 == 25) {
        between = std::min(between, frame.bass);
      }
      TEST_CHECK(frame.bass >= 0.0F && frame.bass <= kMaxAudioBass);
    }
    TEST_CHECK(atHit > 2.0F);
    TEST_CHECK(between < 0.5F);
    const auto frame = play(analysis, sine(80, 0.3F), 240, 12 * 60);
    TEST_NEAR(frame.bass, 1.0F, 0.08F);
  }

  void testLevelIsTheLoudness() {
    AudioAnalysis analysis;
    // A sine of amplitude 0.6 has a root mean square of 0.6 / sqrt(2).
    TEST_NEAR(play(analysis, sine(440, 0.6F), 0, 5).level, 0.4243F, 0.005F);
    // Uniform noise in [-0.5, 0.5) has a root mean square of 1 / sqrt(12).
    TEST_NEAR(play(analysis, whiteNoise, 5, 5).level, 0.2887F, 0.01F);
    const std::vector<float> full(kAudioWaveformSamples, 1.0F);
    TEST_NEAR(analysis.analyze(full, kFrameStep).level, 1.0F, 1e-5F);
  }

  void testWaveformIsTheNewestWindow() {
    AudioAnalysis analysis;
    std::vector<float> samples(kAudioWaveformSamples + 100);
    for (std::size_t i = 0; i < samples.size(); ++i) {
      samples[i] = static_cast<float>(i % 50) / 50.0F - 0.5F;
    }
    samples.back() = 3.0F;
    const auto frame = analysis.analyze(samples, kFrameStep);
    TEST_CHECK(frame.waveform.size() == kAudioWaveformSamples);
    TEST_CHECK(frame.waveform[0] == samples[100]);
    TEST_CHECK(frame.waveform[1000] == samples[1100]);
    TEST_CHECK(frame.waveform.back() == 1.0F);
  }

  // A 1 kHz tone rises to its steady value within a few frames and falls back over a few tenths of a second of
  // silence, reaching exactly 0.
  void testBandsRiseFastAndFallSlowly() {
    constexpr std::size_t kBand = 37;
    AudioAnalysis steady;
    const float settled = play(steady, sine(1000, 0.5F), 0, 60).bands[kBand];
    TEST_CHECK(settled > 0.8F);

    AudioAnalysis rising;
    const auto tone = sine(1000, 0.5F);
    TEST_CHECK(play(rising, tone, 0, 1).bands[kBand] > 0.4F * settled);
    TEST_CHECK(play(rising, tone, 1, 1).bands[kBand] < 0.85F * settled);
    TEST_CHECK(play(rising, tone, 2, 4).bands[kBand] > 0.95F * settled);

    const std::span<const float> none;
    const auto fall = [&](int frames) {
      AudioFrame frame;
      for (int i = 0; i < frames; ++i) {
        frame = steady.analyze(none, kFrameStep);
        checkQuiet(frame);
      }
      return frame.bands[kBand];
    };
    const float afterTenth = fall(6);
    TEST_CHECK(afterTenth > 0.6F * settled && afterTenth < 0.8F * settled);
    const float afterThreeTenths = fall(12);
    TEST_CHECK(afterThreeTenths > 0.3F * settled && afterThreeTenths < 0.45F * settled);
    fall(132);
    checkBandsZero(steady.analyze(none, kFrameStep));

    // The band also falls slowly when the sound moves to another band.
    AudioAnalysis moving;
    const float before = play(moving, tone, 0, 60).bands[kBand];
    const auto after = play(moving, sine(8000, 0.5F), 60, 6);
    TEST_CHECK(loudestBand(after) == 61);
    TEST_CHECK(after.bands[kBand] > 0.6F * before && after.bands[kBand] < 0.8F * before);
    // Then it settles at exactly 0 while the other sound plays on.
    TEST_CHECK(play(moving, sine(8000, 0.5F), 66, 180).bands[kBand] == 0.0F);
  }

  void testBadInputGivesACalmFrame() {
    std::vector<float> zeros(kAudioWaveformSamples, 0.0F);
    std::vector<float> withNan = windowAt(sine(440, 0.5F), 10);
    withNan[700] = kNan;
    std::vector<float> withInfinity = windowAt(sine(440, 0.5F), 10);
    withInfinity[3] = -kInfinity;
    const std::vector<float> few(kAudioWaveformSamples - 1, 0.5F);
    const std::vector<std::span<const float>> inputs{{}, zeros, withNan, withInfinity, few};
    for (const auto samples : inputs) {
      AudioAnalysis analysis;
      const auto frame = analysis.analyze(samples, kFrameStep);
      checkQuiet(frame);
      checkBandsZero(frame);
      TEST_NEAR(frame.deltaSeconds, kFrameStep, 1e-6);
    }
    AudioAnalysis analysis;
    play(analysis, sine(1000, 0.5F), 0, 30);
    for (const auto samples : inputs) {
      const auto frame = analysis.analyze(samples, kFrameStep);
      checkQuiet(frame);
      for (const float band : frame.bands) {
        TEST_CHECK(band >= 0.0F && band <= 1.0F);
      }
    }
  }

  void testOutOfRangeSamplesAreClamped() {
    AudioAnalysis analysis;
    std::vector<float> loud(kAudioWaveformSamples);
    for (std::size_t i = 0; i < loud.size(); ++i) {
      loud[i] = i % 2 == 0 ? 1e30F : -7.0F;
    }
    const auto frame = analysis.analyze(loud, kFrameStep);
    TEST_CHECK(frame.waveform.size() == kAudioWaveformSamples);
    TEST_CHECK(frame.waveform[0] == 1.0F && frame.waveform[1] == -1.0F);
    TEST_NEAR(frame.level, 1.0F, 1e-5F);
    for (const float band : frame.bands) {
      TEST_CHECK(band >= 0.0F && band <= 1.0F);
    }
  }

  void testStepIsBounded() {
    AudioAnalysis analysis;
    const std::span<const float> none;
    TEST_NEAR(analysis.analyze(none, 1e6).deltaSeconds, kMaxAudioFrameSeconds, 1e-7);
    TEST_NEAR(
        analysis.analyze(none, std::numeric_limits<double>::infinity()).deltaSeconds, kMaxAudioFrameSeconds, 1e-7
    );
    TEST_CHECK(analysis.analyze(none, std::numeric_limits<double>::quiet_NaN()).deltaSeconds == 0.0F);
    TEST_CHECK(analysis.analyze(none, -1).deltaSeconds == 0.0F);
    const auto frame = analysis.analyze(none, 0.05);
    TEST_NEAR(frame.deltaSeconds, 0.05F, 1e-7);
    TEST_NEAR(frame.playedSeconds, 0.25, 1e-6);

    // A stalled frame falls as far as one longest step, not as far as the stall.
    constexpr std::size_t kBand = 37;
    AudioAnalysis stalled;
    const float settled = play(stalled, sine(1000, 0.5F), 0, 60).bands[kBand];
    TEST_CHECK(stalled.analyze(none, 30).bands[kBand] > 0.6F * settled);
  }

  void testPlayedSecondsAddUp() {
    AudioAnalysis analysis;
    const auto frame = play(analysis, whiteNoise, 0, 90);
    TEST_NEAR(frame.playedSeconds, 1.5, 1e-9);
    analysis.reset();
    TEST_NEAR(analysis.analyze({}, kFrameStep).playedSeconds, kFrameStep, 1e-9);

    // A seek starts counting from where playback resumes.
    analysis.reset(120);
    TEST_NEAR(analysis.analyze({}, kFrameStep).playedSeconds, 120 + kFrameStep, 1e-9);
    TEST_NEAR(play(analysis, whiteNoise, 0, 60).playedSeconds, 121 + kFrameStep, 1e-9);
    for (const double start :
         {-5.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
      analysis.reset(start);
      TEST_NEAR(analysis.analyze({}, kFrameStep).playedSeconds, kFrameStep, 1e-9);
    }
  }

  void checkSameFrame(const AudioFrame& a, const AudioFrame& b) {
    TEST_CHECK(a.waveform.size() == b.waveform.size());
    for (std::size_t i = 0; i < std::min(a.waveform.size(), b.waveform.size()); ++i) {
      TEST_CHECK(a.waveform[i] == b.waveform[i]);
    }
    TEST_CHECK(a.bands.size() == b.bands.size());
    for (std::size_t i = 0; i < std::min(a.bands.size(), b.bands.size()); ++i) {
      TEST_CHECK(a.bands[i] == b.bands[i]);
    }
    TEST_CHECK(a.bass == b.bass);
    TEST_CHECK(a.level == b.level);
    TEST_CHECK(a.hit == b.hit);
    TEST_CHECK(a.deltaSeconds == b.deltaSeconds);
    TEST_CHECK(a.playedSeconds == b.playedSeconds);
  }

  // Kicks over noise, some frames silent: two objects agree frame by frame, and so does one after reset.
  void testSameSamplesGiveSameFrames() {
    const auto beat = kicks(0.4);
    const Signal mix = [&beat](std::int64_t index) { return beat(index) + (0.1F * whiteNoise(index)); };
    AudioAnalysis first;
    AudioAnalysis second;
    play(second, sine(300, 0.2F), 0, 50);
    second.reset();
    int hits = 0;
    for (int i = 0; i < 150; ++i) {
      const auto samples = i % 37 == 36 ? std::vector<float>{} : windowAt(mix, i);
      const auto a = first.analyze(samples, kFrameStep);
      // The frame's views stay valid while the other object analyzes.
      const auto b = second.analyze(samples, kFrameStep);
      checkSameFrame(a, b);
      hits += a.hit ? 1 : 0;
    }
    TEST_CHECK(hits >= 5);
  }

  double threadMilliseconds() {
    timespec now{};
    TEST_CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return (static_cast<double>(now.tv_sec) * 1e3) + (static_cast<double>(now.tv_nsec) / 1e6);
  }

  void testOneFrameIsCheap() {
    constexpr int kFrames = 600;
    std::vector<std::vector<float>> windows;
    windows.reserve(kFrames);
    for (int i = 0; i < kFrames; ++i) {
      windows.push_back(windowAt(whiteNoise, i));
    }
    AudioAnalysis analysis;
    const double start = threadMilliseconds();
    float sink = 0;
    for (const auto& samples : windows) {
      sink += analysis.analyze(samples, kFrameStep).level;
    }
    const double perFrame = (threadMilliseconds() - start) / kFrames;
    std::println("audio analysis: {:.4f} ms of processor time per frame", perFrame);
    TEST_CHECK(sink > 0.0F);
    TEST_CHECK(TEST_WITHIN_BUDGET(perFrame, 0.2));
  }
} // namespace

int main() {
  testToneLightsItsBandAndNeighborsOnly();
  testWhiteNoiseIsFlat();
  testKicksHitOncePerKick();
  testSteadySoundDoesNotHit();
  testLouderSoundHitsOnce();
  testHitsKeepTheirDistance();
  testBassFollowsKicksAndReturnsToOne();
  testLevelIsTheLoudness();
  testWaveformIsTheNewestWindow();
  testBandsRiseFastAndFallSlowly();
  testBadInputGivesACalmFrame();
  testOutOfRangeSamplesAreClamped();
  testStepIsBounded();
  testPlayedSecondsAddUp();
  testSameSamplesGiveSameFrames();
  testOneFrameIsCheap();
  std::println("workbench audio analysis checks passed");
  return 0;
}
