#pragma once

#include "workbench/audio_frame.h"

// Flashes on hits that stay gentle on the eyes: a decaying envelope a hit restarts, and a limiter on rises.
namespace luciole::workbench {
  // New rises a FlashLimiter lets start per second.
  inline constexpr float kMaxFlashesPerSecond = 3;

  // Limits how often a brightness that changes a large area may rise. A fall passes at once and so does a rise
  // under way, but a new rise starts only when the last one started at least 1 / kMaxFlashesPerSecond seconds ago:
  // a strobe becomes three flashes a second while an isolated hit passes without delay.
  class FlashLimiter {
  public:
    // The brightness to show after `seconds` for the wanted `target`, each within [0, 1]; a target that is not a
    // number reads as 0 and a step that is not a number as 0.
    float step(float target, float seconds);
    float value() const { return m_value; }
    void reset();

  private:
    float m_value = 0;
    float m_sinceRise = 1 / kMaxFlashesPerSecond;
    bool m_rising = false;
  };

  // `value` decayed for `seconds` at `ratePerSecond`, or `kick` when that is larger; each value within [0, 1], and
  // anything that is not a number reads as 0.
  float kickEnvelope(float value, float kick, float ratePerSecond, float seconds);

  // How hard the frame's hit lands, from 0.4 at an average bass to 1 at three times the average; 0 without a hit.
  float hitStrength(const AudioFrame& frame);
} // namespace luciole::workbench
