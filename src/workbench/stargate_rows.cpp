#include "workbench/stargate_rows.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace luciole::workbench {
  namespace {
    // A peak level below this is not lifted further, so hiss stays dark.
    constexpr float kLevelFloor = 0.08F;
    // What the peak level and a streak keep of themselves per row.
    constexpr float kLevelKeep = 0.994F;
    constexpr float kStreakKeep = 0.93F;
    // The fewest samples a row is drawn from.
    constexpr std::size_t kMinSamples = 4;

    std::uint8_t toByte(float value) {
      return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    }

    // The first rising zero crossing in [1, end) of the samples from `offset`, or 0 when there is none.
    std::size_t risingCrossing(const AudioFrame& frame, std::size_t offset, std::size_t end) {
      for (std::size_t i = 1; i < end; ++i) {
        if (waveformSample(frame, offset + i - 1) <= 0 && waveformSample(frame, offset + i) > 0) {
          return i;
        }
      }
      return 0;
    }

    // `span` samples from `start` resampled onto the lanes, linear between the two nearest samples.
    std::array<float, kStargateLanes> lanesOf(const AudioFrame& frame, std::size_t start, std::size_t span) {
      std::array<float, kStargateLanes> lanes{};
      const std::size_t last = start + span - 1;
      const double step = static_cast<double>(span - 1) / static_cast<double>(kStargateLanes - 1);
      for (std::size_t lane = 0; lane < kStargateLanes; ++lane) {
        const double x = static_cast<double>(start) + (static_cast<double>(lane) * step);
        const std::size_t low = std::min(static_cast<std::size_t>(x), last);
        const std::size_t high = std::min(low + 1, last);
        const float a = waveformSample(frame, low);
        const float b = waveformSample(frame, high);
        lanes[lane] = a + ((b - a) * static_cast<float>(x - static_cast<double>(low)));
      }
      return lanes;
    }
  } // namespace

  void StargateRowBuilder::reset() { *this = {}; }

  void StargateRowBuilder::build(const AudioFrame& frame, std::span<std::uint8_t> row) {
    if (row.size() != kStargateLanes * AudioHistory::kBytesPerTexel) {
      throw std::invalid_argument("a stargate row must be 128 texels");
    }
    const std::size_t total = frame.waveform.size();
    const std::size_t count = std::min(total, kAudioWaveformSamples);
    const std::size_t offset = total - count;
    std::array<float, kStargateLanes> lanes{};
    if (count >= kMinSamples) {
      const std::size_t span = count / 2;
      const std::size_t start = risingCrossing(frame, offset, count - span + 1);
      lanes = lanesOf(frame, offset + start, span);
    }
    float peak = 0;
    for (const float value : lanes) {
      peak = std::max(peak, std::abs(value));
    }
    m_level = std::max(peak, m_level * kLevelKeep);
    const float gain = 1 / std::max(m_level, kLevelFloor);

    float sum = 0;
    float moment = 0;
    for (std::size_t band = 0; band < kAudioBandCount; ++band) {
      sum += spectrumBand(frame, band);
      moment += spectrumBand(frame, band) * static_cast<float>(band);
    }
    const float hue = sum > 1e-9F ? moment / sum / static_cast<float>(kAudioBandCount - 1) : 0.5F;
    const std::uint8_t hueByte = toByte(hue);
    const std::uint8_t energyByte = toByte(sum / static_cast<float>(kAudioBandCount));
    for (std::size_t lane = 0; lane < kStargateLanes; ++lane) {
      const float value = std::clamp(lanes[lane] * gain, -1.0F, 1.0F);
      m_streaks[lane] = std::max(std::abs(value), m_streaks[lane] * kStreakKeep);
      const std::size_t at = lane * AudioHistory::kBytesPerTexel;
      row[at] = toByte(0.5F + (value / 2));
      row[at + 1] = toByte(m_streaks[lane]);
      row[at + 2] = hueByte;
      row[at + 3] = energyByte;
    }
  }

  void StargateFlight::reset() {
    m_roll.reset();
    m_surge.reset();
    m_pulse.reset();
    m_schedule.reset();
    m_glow.reset();
    m_builder.reset();
  }

  void StargateFlight::advance(const AudioFrame& frame, AudioHistory& history) {
    if (history.width() != kStargateLanes || history.rows() != kStargateRows) {
      throw std::invalid_argument("the stargate history must be 128 by 128 texels");
    }
    const float seconds = frameStep(frame);
    const float bass = bassRatio(frame);
    m_roll.step(seconds, bass);
    const float surge = m_surge.step(seconds, bass);
    m_pulse.step(seconds, frame.hit, bass);
    m_glow.step(seconds, frame);
    const float rowsPerSecond = static_cast<float>(kStargateRows) / kStargateTravelSeconds * surge;
    if (m_schedule.step(seconds, rowsPerSecond)) {
      m_builder.build(frame, history.writeRow(m_schedule.head()));
    }
  }
} // namespace luciole::workbench
