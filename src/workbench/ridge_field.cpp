#include "workbench/ridge_field.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace luciole::workbench {
  namespace {
    constexpr float kPlateau = 0.2F;          // share of the half width that is flat at full height
    constexpr float kEdgeBand = 48;           // band read at the raised middle's edges; the center reads band 0
    constexpr float kTilt = 0.25F;            // boost of the edges' higher bands over the center's bass
    constexpr float kSharpness = 1.25F;       // above 1 sharpens peaks against the slopes between them
    constexpr std::uint64_t kKnots = 22;      // coarse roughness knots across the line
    constexpr std::uint64_t kKnotStride = 89; // waveform samples between knots
    constexpr float kJitter = 0.55F;          // knot factors run from 1 - kJitter to 1 + 3 * kJitter
    constexpr float kJag = 0.1F;              // fine roughness in the middle, relative to the local height
    constexpr float kSkew = 4;                // most bands the right half reads off from the left
    constexpr float kWiggle = 0.012F;         // waveform wiggle everywhere
    constexpr float kCeiling = 0.45F;         // heights approach 1 / kCeiling
    constexpr float kWiggleFloor = 0.004F;    // a faint wiggle that keeps a silent line from being ruled straight
    constexpr float kGainTarget = 0.85F;
    constexpr float kGainFloor = 0.15F;
    constexpr float kGainRise = 1;
    constexpr float kGainFall = 5;
    constexpr float kPadY = 0.07F;
    constexpr float kPadX = 0.06F;
    constexpr float kPlotAspect = 0.78F;
    constexpr float kAmplitudeShare = 0.15F; // the full peak height as a share of the plot height
    constexpr float kHeadroom = 0.7F;        // room above the oldest baseline, in full peak heights

    // A fixed value in [-1, 1] for each line and point.
    float wiggleNoise(std::uint32_t seed, std::size_t index) {
      std::uint64_t x = (std::uint64_t{seed} << 32U) ^ static_cast<std::uint64_t>(index);
      x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
      x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
      x ^= x >> 31U;
      return (static_cast<float>(x >> 40U) / static_cast<float>(1U << 23U)) - 1;
    }

    // `value` over `peak`, kept within [-1, 1].
    float shapeOf(float value, float peak) { return std::clamp(value / peak, -1.0F, 1.0F); }
  } // namespace

  void ridgeEnvelope(std::span<float> out) {
    if (out.size() == 1) {
      out[0] = 1;
      return;
    }
    const auto last = static_cast<float>(out.size() - 1);
    for (std::size_t i = 0; i < out.size(); ++i) {
      const float d = std::abs((static_cast<float>(i) / last) - 0.5F) / kRidgeHalfWidth;
      if (d <= kPlateau) {
        out[i] = 1;
      } else if (d < 1) {
        out[i] = 0.5F + (0.5F * std::cos(std::numbers::pi_v<float> * (d - kPlateau) / (1 - kPlateau)));
      } else {
        out[i] = 0;
      }
    }
  }

  float bandAt(std::span<const float> bands, float position) {
    if (bands.empty()) {
      return 0;
    }
    const auto last = static_cast<float>(bands.size() - 1);
    if (std::isnan(position) || position <= 0) {
      return bands.front();
    }
    if (position >= last) {
      return bands.back();
    }
    const auto i = static_cast<std::size_t>(position);
    const float f = position - static_cast<float>(i);
    return bands[i] + ((bands[i + 1] - bands[i]) * f);
  }

  void RidgeGain::step(float peak, float seconds) {
    peak = std::isnan(peak) ? 0.0F : peak;
    seconds = std::isnan(seconds) ? 0.0F : std::max(seconds, 0.0F);
    const float tau = peak > level ? kGainRise : kGainFall;
    level += (peak - level) * (1 - std::exp(-seconds / tau));
  }

  float RidgeGain::factor() const { return kGainTarget / std::max(level, kGainFloor); }

  float buildRidge(
      std::span<float> out, std::span<const float> envelope, std::span<const float> bands, std::span<const float> wave,
      float gain, std::uint32_t seed
  ) {
    if (out.size() != kRidgePoints
        || envelope.size() != kRidgePoints
        || bands.size() != kAudioBandCount
        || wave.size() != kAudioWaveformSamples) {
      return 0;
    }
    const std::size_t last = kRidgePoints - 1;
    const std::size_t n = wave.size();
    const std::size_t stride = std::max<std::size_t>(1, n / kRidgePoints);
    const std::size_t phase = (std::uint64_t{seed} * 37U) % stride;
    const auto sample = [&](std::size_t i) { return wave[(i * stride) + phase]; };
    // The roughness reads the waveform's shape, not its loudness.
    float peak = 1e-3F;
    for (std::size_t i = 0; i <= last; ++i) {
      peak = std::max(peak, std::abs(sample(i)));
    }
    const std::uint64_t knotBase = std::uint64_t{seed} * 53U;
    const float skew = kSkew * shapeOf(wave[((std::uint64_t{seed} * 131U) + 17U) % n], peak);
    const auto knot = [&](std::uint64_t k) { return shapeOf(wave[((k * kKnotStride) + knotBase) % n], peak); };
    float raw = 0;
    for (std::size_t i = 0; i <= last; ++i) {
      const float x = static_cast<float>(i) / static_cast<float>(last);
      const float w = sample(i);
      float y = (kWiggleFloor * wiggleNoise(seed, i)) + (kWiggle * std::clamp(3 * w, -1.0F, 1.0F));
      const float e = envelope[i];
      if (e > 0) {
        const float t = std::abs(x - 0.5F) / kRidgeHalfWidth;
        // The right half reads the spectrum a few bands off from the left, so the halves never mirror.
        const float r = e * bandAt(bands, (t * kEdgeBand) + (x > 0.5F ? skew : 0.0F));
        raw = std::max(raw, r);
        // Coarse roughness, linear between knots for sharp peaks; squared so a few knots run tall.
        const float u = x * static_cast<float>(kKnots);
        const float j = std::floor(u);
        const auto k = static_cast<std::uint64_t>(j);
        const float k0 = knot(k);
        const float c = 0.5F + (0.5F * (k0 + ((knot(k + 1) - k0) * (u - j))));
        const float m = 1 - kJitter + (4 * kJitter * c * c);
        const float g = gain * r * (1 - kTilt + (2 * kTilt * t));
        const float v = std::pow(g * std::max(m, 0.0F), kSharpness);
        y += (v / (1 + (kCeiling * v))) + (kJag * g * (w / peak));
      }
      out[i] = y;
    }
    return raw;
  }

  float ridgeSlope(std::span<const float> row, std::size_t index) {
    if (row.size() < 2 || index >= row.size()) {
      return 0;
    }
    if (index == 0) {
      return row[1] - row[0];
    }
    if (index == row.size() - 1) {
      return row[index] - row[index - 1];
    }
    return (row[index + 1] - row[index - 1]) / 2;
  }

  RidgeLayout ridgeLayout(float width, float height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
      return {};
    }
    const float padY = height * kPadY;
    const float padX = width * kPadX;
    const float plotHeight = height - (2 * padY);
    RidgeLayout out;
    out.width = std::min(width - (2 * padX), plotHeight * kPlotAspect);
    out.left = (width - out.width) / 2;
    out.bottom = height - padY;
    out.top = padY + (plotHeight * kAmplitudeShare * kHeadroom);
    out.spacing = (out.bottom - out.top) / static_cast<float>(kRidgeLines - 1);
    out.amplitude = plotHeight * kAmplitudeShare;
    return out;
  }

  RidgeField::RidgeField() : m_rows(kRidgeLines * kRidgePoints, 0.0F), m_wave(kAudioWaveformSamples, 0.0F) {
    ridgeEnvelope(m_envelope);
  }

  void RidgeField::reset() {
    std::ranges::fill(m_rows, 0.0F);
    m_head = 0;
    m_pushes = 0;
    m_seed = 0;
    m_fraction = 0;
    m_gain = RidgeGain{};
    m_factor = 1;
  }

  void RidgeField::advance(const AudioFrame& frame) {
    const float clock = m_fraction + (frameStep(frame) * kRidgeLinesPerSecond);
    const float due = std::floor(clock);
    m_fraction = std::clamp(clock - due, 0.0F, std::nextafter(1.0F, 0.0F));
    const auto lines = std::min(static_cast<std::size_t>(due), kRidgeLines);
    if (lines == 0) {
      return;
    }
    // The newest samples, oldest first; a short waveform is padded with silence in front.
    const auto size = static_cast<std::ptrdiff_t>(frame.waveform.size());
    const auto first = size - static_cast<std::ptrdiff_t>(m_wave.size());
    for (std::size_t k = 0; k < m_wave.size(); ++k) {
      const std::ptrdiff_t index = first + static_cast<std::ptrdiff_t>(k);
      m_wave[k] = index < 0 ? 0.0F : waveformSample(frame, static_cast<std::size_t>(index));
    }
    for (std::size_t band = 0; band < m_bands.size(); ++band) {
      m_bands[band] = spectrumBand(frame, band);
    }
    for (std::size_t k = 0; k < lines; ++k) {
      push();
    }
  }

  void RidgeField::push() {
    m_head = (m_head + 1) % kRidgeLines;
    const auto row = std::span(m_rows).subspan(m_head * kRidgePoints, kRidgePoints);
    const float raw = buildRidge(row, m_envelope, m_bands, m_wave, m_factor, ++m_seed);
    m_gain.step(raw, 1 / kRidgeLinesPerSecond);
    m_factor = m_gain.factor();
    ++m_pushes;
  }
} // namespace luciole::workbench
