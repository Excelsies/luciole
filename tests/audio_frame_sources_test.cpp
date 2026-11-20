#include "audio_frame_sources.h"
#include "model/audio_visual_kind.h"
#include "test_check.h"
#include "workbench/audio_frame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using luciole::AudioVisualKind;
  using luciole::workbench::AudioFrame;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInfinity = std::numeric_limits<float>::infinity();

  void kindsHaveTheirWords() {
    TEST_CHECK(luciole::kAudioVisualCount == 9);
    TEST_CHECK(luciole::audioVisualLabel(AudioVisualKind::CosmicPeanut) == "Cosmic Peanut");
    TEST_CHECK(luciole::audioVisualLabel(AudioVisualKind::DarkSun) == "Dark Sun");
    TEST_CHECK(luciole::audioVisualLabel(AudioVisualKind::Stargate) == "Stargate");
    TEST_CHECK(luciole::audioVisualName(AudioVisualKind::CosmicPeanut) == "cosmic-peanut");
    TEST_CHECK(luciole::audioVisualName(AudioVisualKind::DarkSun) == "dark-sun");
    TEST_CHECK(luciole::audioVisualName(AudioVisualKind::Radar) == "radar");
    const std::array<std::string, 9> words{"cosmic-peanut", "pulsar",  "bars",     "orbit",   "halo",
                                           "radar",         "eclipse", "dark-sun", "stargate"};
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto kind = luciole::audioVisualFromName(words[index]);
      TEST_CHECK(kind.has_value());
      TEST_CHECK(static_cast<std::size_t>(*kind) == index);
      TEST_CHECK(luciole::audioVisualName(*kind) == words[index]);
    }
    for (const std::string_view word :
         {"", "Cosmic Peanut", "cosmic_peanut", "COSMIC-PEANUT", "bars ", " bars", "dark sun", "orbits", "bar"}) {
      TEST_CHECK(!luciole::audioVisualFromName(word).has_value());
    }
    TEST_CHECK(luciole::audioVisualLabel(static_cast<AudioVisualKind>(9)).empty());
    TEST_CHECK(luciole::audioVisualName(static_cast<AudioVisualKind>(200)).empty());
  }

  void frameValuesAreChecked() {
    using namespace luciole::workbench;
    const std::array<float, 6> samples{0.5F, 2.0F, -3.0F, kNan, kInfinity, -0.25F};
    const std::array<float, 4> bands{0.5F, 1.5F, -0.5F, kNan};
    AudioFrame frame{.waveform = samples, .bands = bands};
    TEST_NEAR(waveformSample(frame, 0), 0.5F, 1e-6F);
    TEST_NEAR(waveformSample(frame, 1), 1.0F, 1e-6F);
    TEST_NEAR(waveformSample(frame, 2), -1.0F, 1e-6F);
    TEST_NEAR(waveformSample(frame, 3), 0.0F, 1e-6F);
    TEST_NEAR(waveformSample(frame, 4), 0.0F, 1e-6F);
    TEST_NEAR(waveformSample(frame, 5), -0.25F, 1e-6F);
    TEST_NEAR(waveformSample(frame, 6), 0.0F, 1e-6F);
    TEST_NEAR(spectrumBand(frame, 0), 0.5F, 1e-6F);
    TEST_NEAR(spectrumBand(frame, 1), 1.0F, 1e-6F);
    TEST_NEAR(spectrumBand(frame, 2), 0.0F, 1e-6F);
    TEST_NEAR(spectrumBand(frame, 3), 0.0F, 1e-6F);
    TEST_NEAR(spectrumBand(frame, 63), 0.0F, 1e-6F);

    frame.bass = 1.5F;
    frame.level = 0.3F;
    frame.deltaSeconds = 0.02F;
    frame.playedSeconds = 12.5;
    TEST_NEAR(bassRatio(frame), 1.5F, 1e-6F);
    TEST_NEAR(audioLevel(frame), 0.3F, 1e-6F);
    TEST_NEAR(frameStep(frame), 0.02F, 1e-6F);
    TEST_NEAR(playbackTime(frame), 12.5, 1e-9);

    frame.bass = kNan;
    frame.level = kNan;
    frame.deltaSeconds = kNan;
    frame.playedSeconds = std::numeric_limits<double>::quiet_NaN();
    TEST_NEAR(bassRatio(frame), 0.0F, 1e-6F);
    TEST_NEAR(audioLevel(frame), 0.0F, 1e-6F);
    TEST_NEAR(frameStep(frame), 0.0F, 1e-6F);
    TEST_NEAR(playbackTime(frame), 0.0, 1e-9);

    frame.bass = kInfinity;
    frame.level = 7.0F;
    frame.deltaSeconds = 1e9F;
    frame.playedSeconds = std::numeric_limits<double>::infinity();
    TEST_NEAR(bassRatio(frame), 4.0F, 1e-6F);
    TEST_NEAR(audioLevel(frame), 1.0F, 1e-6F);
    TEST_NEAR(frameStep(frame), 0.1F, 1e-6F);
    TEST_NEAR(playbackTime(frame), 0.0, 1e-9);

    frame.bass = -2.0F;
    frame.level = -1.0F;
    frame.deltaSeconds = -0.5F;
    frame.playedSeconds = -3.0;
    TEST_NEAR(bassRatio(frame), 0.0F, 1e-6F);
    TEST_NEAR(audioLevel(frame), 0.0F, 1e-6F);
    TEST_NEAR(frameStep(frame), 0.0F, 1e-6F);
    TEST_NEAR(playbackTime(frame), 0.0, 1e-9);
  }

  int zeroCrossings(const AudioFrame& frame) {
    int count = 0;
    for (std::size_t index = 1; index < frame.waveform.size(); ++index) {
      if ((frame.waveform[index - 1] < 0) != (frame.waveform[index] < 0)) {
        ++count;
      }
    }
    return count;
  }

  void toneIsOneSineAndOneBand() {
    AudioFrameSource source(AudioScene::Tone);
    for (int index = 0; index < 90; ++index) {
      const AudioFrame frame = source.next();
      TEST_CHECK(frame.waveform.size() == 2048);
      TEST_CHECK(frame.bands.size() == 64);
      TEST_CHECK(!frame.hit);
      TEST_NEAR(frame.deltaSeconds, 1.0F / 60.0F, 1e-6F);
      TEST_NEAR(frame.playedSeconds, index / 60.0, 1e-9);
      if (index == 45) {
        // 440 Hz over 2048 samples at 22050 Hz: about 40.9 cycles, two crossings each.
        TEST_CHECK(zeroCrossings(frame) >= 80 && zeroCrossings(frame) <= 83);
        TEST_NEAR(frame.level, 0.42F, 0.01F);
        TEST_NEAR(frame.bass, 1.0F, 1e-6F);
        for (std::size_t band = 0; band < frame.bands.size(); ++band) {
          TEST_CHECK((frame.bands[band] > 0.5F) == (band == 30));
        }
        float peak = 0;
        for (const float sample : frame.waveform) {
          peak = std::max(peak, std::abs(sample));
        }
        TEST_NEAR(peak, 0.6F, 0.01F);
      }
    }
    TEST_CHECK(source.frames() == 90);
  }

  void kicksHitEveryHalfSecond() {
    AudioFrameSource source(AudioScene::Kicks);
    std::vector<int> hits;
    for (int index = 0; index < 120; ++index) {
      const AudioFrame frame = source.next();
      if (frame.hit) {
        hits.push_back(index);
        TEST_CHECK(frame.bass > 1.0F);
        TEST_CHECK(frame.bands[0] > 0.5F);
        TEST_CHECK(frame.bands[40] == 0.0F);
        TEST_CHECK(frame.waveform.size() == 2048);
      }
      if (index == 20) {
        TEST_CHECK(frame.bass < 1.0F);
        TEST_CHECK(frame.bands[0] < 0.1F);
      }
    }
    TEST_CHECK((hits == std::vector<int>{0, 30, 60, 90}));
  }

  void noiseAndSilence() {
    AudioFrameSource first(AudioScene::Noise);
    AudioFrameSource second(AudioScene::Noise);
    AudioFrame a{};
    AudioFrame b{};
    std::vector<float> previous;
    for (int index = 0; index < 10; ++index) {
      previous.assign(a.waveform.begin(), a.waveform.end());
      a = first.next();
      b = second.next();
    }
    TEST_CHECK(a.waveform.size() == 2048 && b.waveform.size() == 2048);
    int same = 0;
    int changed = 0;
    double sum = 0;
    for (std::size_t index = 0; index < a.waveform.size(); ++index) {
      TEST_CHECK(a.waveform[index] >= -1.0F && a.waveform[index] <= 1.0F);
      same += a.waveform[index] == b.waveform[index] ? 1 : 0;
      changed += a.waveform[index] != previous[index] ? 1 : 0;
      sum += a.waveform[index];
    }
    TEST_CHECK(same == 2048);
    TEST_CHECK(changed > 2000);
    TEST_NEAR(sum / 2048, 0.0, 0.05);
    for (const float band : a.bands) {
      TEST_CHECK(band >= 0.25F && band <= 0.75F);
    }

    AudioFrameSource silence(AudioScene::Silence);
    for (int index = 0; index < 5; ++index) {
      const AudioFrame frame = silence.next();
      TEST_CHECK(frame.waveform.empty());
      TEST_CHECK(frame.bands.size() == 64);
      for (const float band : frame.bands) {
        TEST_CHECK(band == 0.0F);
      }
      TEST_CHECK(frame.bass == 0.0F && frame.level == 0.0F && !frame.hit);
    }
  }

  // Every hostile frame breaks at least one promise of the frame; together they break each.
  void hostileFramesBreakEveryPromise() {
    AudioFrameSource source(AudioScene::Hostile);
    bool nanSample = false;
    bool longWaveform = false;
    bool shortBands = false;
    bool hugeStep = false;
    bool negativeStep = false;
    bool nanBass = false;
    for (int index = 0; index < 8; ++index) {
      const AudioFrame frame = source.next();
      for (const float sample : frame.waveform) {
        nanSample = nanSample || std::isnan(sample);
      }
      longWaveform = longWaveform || frame.waveform.size() > 2048;
      shortBands = shortBands || frame.bands.size() < 64;
      hugeStep = hugeStep || frame.deltaSeconds > 1000.0F;
      negativeStep = negativeStep || frame.deltaSeconds < 0.0F;
      nanBass = nanBass || std::isnan(frame.bass);
    }
    TEST_CHECK(nanSample && longWaveform && shortBands && hugeStep && negativeStep && nanBass);
  }
} // namespace

int main() {
  kindsHaveTheirWords();
  frameValuesAreChecked();
  toneIsOneSineAndOneBand();
  kicksHitEveryHalfSecond();
  noiseAndSilence();
  hostileFramesBreakEveryPromise();
  std::println("audio frame source checks passed");
  return 0;
}
