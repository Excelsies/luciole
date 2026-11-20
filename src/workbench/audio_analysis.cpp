#include "workbench/audio_analysis.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace luciole::workbench {
  namespace {
    constexpr std::size_t kSize = kAudioWaveformSamples;
    constexpr double kBinHz = static_cast<double>(kAudioWaveformRate) / kSize;
    constexpr double kLowestHz = 40;
    constexpr double kHighestHz = 10000;
    // Band power in decibels maps to 0 at the floor and 1 at the ceiling.
    constexpr float kFloorDecibels = -70;
    constexpr float kCeilingDecibels = -10;
    constexpr float kTinyPower = 1e-12F;
    // A window quieter than -70 dBFS is silence.
    constexpr double kSilentMeanSquare = 1e-7;
    // A band falling toward 0 reads 0 once below this.
    constexpr float kBandRest = 1e-3F;
    constexpr float kRiseSeconds = 0.025F;
    constexpr float kFallSeconds = 0.3F;
    constexpr double kBassHz = 150;
    constexpr float kBassAverageSeconds = 3;
    // Bass magnitude below this, about -60 dBFS, is no bass.
    constexpr float kBassFloor = 1e-3F;
    constexpr double kHitTopHz = 2000;
    constexpr float kHitAverageSeconds = 1;
    // A hit is energy this many times its recent average, above a floor, at most once per refractory time. The next
    // hit waits until the energy fell below kRearmRatio times the average.
    constexpr float kHitRatio = 2;
    constexpr float kRearmRatio = 1.25F;
    constexpr float kHitFloor = 1e-7F;
    constexpr float kHitRefractorySeconds = 0.1F;
    // Running averages count frames up to this, then weigh by time alone.
    constexpr int kAverageFrames = 1 << 20;

    // The weight of a one-pole step of `step` seconds toward a target, reaching 63% after `seconds`.
    float smoothing(float step, float seconds) { return 1.0F - std::exp(-step / seconds); }

    // A running average that starts as a plain mean, so the first frames carry no bias toward 0.
    void average(float& value, int& frames, float sample, float step, float seconds) {
      frames = std::min(frames + 1, kAverageFrames);
      const float weight = std::max(1.0F / static_cast<float>(frames), smoothing(step, seconds));
      value += (sample - value) * weight;
    }

    double bandEdgeHz(std::size_t edge) {
      const double share = static_cast<double>(edge) / kAudioBandCount;
      return kLowestHz * std::pow(kHighestHz / kLowestHz, share);
    }

    // Bin k covers (k - 0.5) to (k + 0.5) times kBinHz.
    std::size_t binOf(double hz) { return static_cast<std::size_t>(std::floor((hz / kBinHz) + 0.5)); }
  } // namespace

  AudioAnalysis::AudioAnalysis()
      : m_window(kSize), m_cosines(kHalf), m_sines(kHalf), m_reversed(kHalf), m_waveform(kSize), m_real(kHalf),
        m_imaginary(kHalf), m_power(kHalf) {
    // A Hann window scaled so a full-scale sine centered on a bin reads 1.
    for (std::size_t i = 0; i < kSize; ++i) {
      const double angle = 2 * std::numbers::pi * static_cast<double>(i) / kSize;
      m_window[i] = static_cast<float>((1 - std::cos(angle)) * 2 / kSize);
    }
    for (std::size_t k = 0; k < kHalf; ++k) {
      const double angle = 2 * std::numbers::pi * static_cast<double>(k) / kSize;
      m_cosines[k] = static_cast<float>(std::cos(angle));
      m_sines[k] = static_cast<float>(std::sin(angle));
      std::size_t reversed = 0;
      for (std::size_t bit = 1; bit < kHalf; bit <<= 1U) {
        reversed = (reversed << 1U) | ((k & bit) != 0 ? 1U : 0U);
      }
      m_reversed[k] = reversed;
    }
    // Each band is the mean power of the bins it overlaps, each weighed by its overlap.
    for (std::size_t band = 0; band < kAudioBandCount; ++band) {
      const double low = bandEdgeHz(band);
      const double high = bandEdgeHz(band + 1);
      auto& span = m_bandSpans[band];
      span.weight = m_weights.size();
      span.bin = binOf(low);
      const std::size_t last = std::min(binOf(high), kHalf - 1);
      for (std::size_t bin = span.bin; bin <= last; ++bin) {
        const double binLow = (static_cast<double>(bin) - 0.5) * kBinHz;
        const double overlap = std::min(high, binLow + kBinHz) - std::max(low, binLow);
        m_weights.push_back(static_cast<float>(std::max(overlap, 0.0) / (high - low)));
        m_binCount = std::max(m_binCount, bin + 1);
      }
      span.count = m_weights.size() - span.weight;
      if (low < kHitTopHz) {
        m_hitBands = band + 1;
      }
    }
    reset();
  }

  AudioFrame AudioAnalysis::analyze(std::span<const float> samples, double stepSeconds) {
    const double step = std::isnan(stepSeconds) ? 0.0 : std::clamp(stepSeconds, 0.0, double{kMaxAudioFrameSeconds});
    const auto stepF = static_cast<float>(step);
    m_playedSeconds += step;
    AudioFrame frame{
        .waveform = {},
        .bands = m_bands,
        .deltaSeconds = stepF,
        .playedSeconds = m_playedSeconds,
    };
    m_sinceHit = std::min(m_sinceHit + stepF, kHitRefractorySeconds);
    if (!load(samples)) {
      fadeBands(stepF);
      m_hitArmed = true;
      return frame;
    }
    transform();
    foldBands(stepF);
    frame.waveform = m_waveform;
    frame.bass = bassFrom(stepF);
    frame.level = std::min(static_cast<float>(std::sqrt(m_meanSquare)), 1.0F);
    frame.hit = hitFrom(stepF);
    return frame;
  }

  void AudioAnalysis::reset(double startSeconds) {
    m_bands.fill(0);
    m_bassAverage = 0;
    m_bassFrames = 0;
    m_hitAverage = 0;
    m_hitFrames = 0;
    m_hitArmed = true;
    m_sinceHit = kHitRefractorySeconds;
    m_playedSeconds = std::isfinite(startSeconds) ? std::max(startSeconds, 0.0) : 0.0;
  }

  // Copies the newest window, clamped, into m_waveform; false when it is silence or cannot be heard.
  bool AudioAnalysis::load(std::span<const float> samples) {
    if (samples.size() < kSize) {
      return false;
    }
    const float* source = samples.last(kSize).data();
    float* target = m_waveform.data();
    double squares = 0;
    for (std::size_t i = 0; i < kSize; ++i) {
      if (!std::isfinite(source[i])) {
        return false;
      }
      const float sample = std::clamp(source[i], -1.0F, 1.0F);
      target[i] = sample;
      squares += static_cast<double>(sample) * sample;
    }
    m_meanSquare = squares / kSize;
    return m_meanSquare >= kSilentMeanSquare;
  }

  // The power of each bin below m_binCount: the even and odd samples go through one transform of half the size as
  // its real and imaginary parts, then the two halves are separated.
  void AudioAnalysis::transform() {
    float* re = m_real.data();
    float* im = m_imaginary.data();
    const float* cosines = m_cosines.data();
    const float* sines = m_sines.data();
    const float* window = m_window.data();
    const float* wave = m_waveform.data();
    for (std::size_t i = 0; i < kHalf; ++i) {
      const std::size_t j = m_reversed[i];
      re[j] = wave[2 * i] * window[2 * i];
      im[j] = wave[(2 * i) + 1] * window[(2 * i) + 1];
    }
    // Radix-2 butterflies: a twiddle of 2 pi k / size is entry k * kSize / size of the tables.
    for (std::size_t size = 2; size <= kHalf; size <<= 1U) {
      const std::size_t half = size / 2;
      const std::size_t stride = kSize / size;
      for (std::size_t k = 0; k < half; ++k) {
        const float wr = cosines[k * stride];
        const float wi = -sines[k * stride];
        for (std::size_t top = k; top < kHalf; top += size) {
          const std::size_t bottom = top + half;
          const float tr = (wr * re[bottom]) - (wi * im[bottom]);
          const float ti = (wr * im[bottom]) + (wi * re[bottom]);
          re[bottom] = re[top] - tr;
          im[bottom] = im[top] - ti;
          re[top] += tr;
          im[top] += ti;
        }
      }
    }
    float* power = m_power.data();
    for (std::size_t k = 0; k < m_binCount; ++k) {
      const std::size_t mirror = (kHalf - k) % kHalf;
      const float evenRe = 0.5F * (re[k] + re[mirror]);
      const float evenIm = 0.5F * (im[k] - im[mirror]);
      const float oddRe = 0.5F * (im[k] + im[mirror]);
      const float oddIm = -0.5F * (re[k] - re[mirror]);
      const float xr = evenRe + (cosines[k] * oddRe) + (sines[k] * oddIm);
      const float xi = evenIm + (cosines[k] * oddIm) - (sines[k] * oddRe);
      power[k] = (xr * xr) + (xi * xi);
    }
  }

  void AudioAnalysis::foldBands(float step) {
    const float rise = smoothing(step, kRiseSeconds);
    const float fall = smoothing(step, kFallSeconds);
    const float scale = 10.0F / (kCeilingDecibels - kFloorDecibels);
    const float offset = -kFloorDecibels / (kCeilingDecibels - kFloorDecibels);
    const float* power = m_power.data();
    const float* weights = m_weights.data();
    for (std::size_t band = 0; band < kAudioBandCount; ++band) {
      const auto [weight, bin, count] = m_bandSpans[band];
      float sum = 0;
      for (std::size_t i = 0; i < count; ++i) {
        sum += weights[weight + i] * power[bin + i];
      }
      m_bandPower[band] = sum;
      const float target = std::clamp((scale * std::log10(sum + kTinyPower)) + offset, 0.0F, 1.0F);
      float& value = m_bands[band];
      value += (target - value) * (target > value ? rise : fall);
      if (target == 0.0F && value < kBandRest) {
        value = 0.0F;
      }
    }
  }

  void AudioAnalysis::fadeBands(float step) {
    const float keep = 1.0F - smoothing(step, kFallSeconds);
    for (float& value : m_bands) {
      value = value * keep < kBandRest ? 0.0F : value * keep;
    }
  }

  // The magnitude below kBassHz as a ratio to its running average.
  float AudioAnalysis::bassFrom(float step) {
    const std::size_t last = std::min(binOf(kBassHz), m_binCount);
    float magnitude = 0;
    for (std::size_t bin = 1; bin < last; ++bin) {
      magnitude += std::sqrt(m_power[bin]);
    }
    magnitude = magnitude < kBassFloor ? 0.0F : magnitude;
    average(m_bassAverage, m_bassFrames, magnitude, step, kBassAverageSeconds);
    return std::min(magnitude / std::max(m_bassAverage, kTinyPower), kMaxAudioBass);
  }

  // Whether the power of the low and middle bands rose above kHitRatio times its recent average in this frame.
  bool AudioAnalysis::hitFrom(float step) {
    float energy = 0;
    for (std::size_t band = 0; band < m_hitBands; ++band) {
      energy += m_bandPower[band];
    }
    const bool hit =
        m_hitArmed && energy > kHitFloor && energy > kHitRatio * m_hitAverage && m_sinceHit >= kHitRefractorySeconds;
    if (hit) {
      m_hitArmed = false;
      m_sinceHit = 0;
    } else if (energy < kRearmRatio * m_hitAverage) {
      m_hitArmed = true;
    }
    average(m_hitAverage, m_hitFrames, energy, step, kHitAverageSeconds);
    return hit;
  }
} // namespace luciole::workbench
