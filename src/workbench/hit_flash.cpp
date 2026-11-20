#include "workbench/hit_flash.h"

#include <algorithm>
#include <cmath>

namespace luciole::workbench {
  namespace {
    constexpr float kMinRiseInterval = 1 / kMaxFlashesPerSecond;
    constexpr float kWeakestHit = 0.4F;
    constexpr float kHitPerBass = 0.3F;

    float unit(float value) { return std::isnan(value) ? 0.0F : std::clamp(value, 0.0F, 1.0F); }

    float stepSeconds(float seconds) {
      return std::isnan(seconds) ? 0.0F : std::clamp(seconds, 0.0F, kMaxAudioFrameSeconds);
    }
  } // namespace

  float FlashLimiter::step(float target, float seconds) {
    const float wanted = unit(target);
    m_sinceRise = std::min(m_sinceRise + stepSeconds(seconds), kMinRiseInterval);
    if (wanted > m_value) {
      if (!m_rising && m_sinceRise >= kMinRiseInterval) {
        m_rising = true;
        m_sinceRise = 0;
      }
      if (m_rising) {
        m_value = wanted;
      }
    } else if (wanted < m_value) {
      m_rising = false;
      m_value = wanted;
    }
    return m_value;
  }

  void FlashLimiter::reset() {
    m_value = 0;
    m_sinceRise = kMinRiseInterval;
    m_rising = false;
  }

  float kickEnvelope(float value, float kick, float ratePerSecond, float seconds) {
    const float rate = std::isnan(ratePerSecond) ? 0.0F : std::max(ratePerSecond, 0.0F);
    return std::max(unit(kick), unit(value) * std::exp(-rate * stepSeconds(seconds)));
  }

  float hitStrength(const AudioFrame& frame) {
    if (!frame.hit) {
      return 0;
    }
    return std::min(1.0F, kWeakestHit + (kHitPerBass * std::max(0.0F, bassRatio(frame) - 1)));
  }
} // namespace luciole::workbench
