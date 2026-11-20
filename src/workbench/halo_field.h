#pragma once

#include "workbench/audio_frame.h"
#include "workbench/hit_flash.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The state behind the Halo sound visual: a ring of light whose radius around the circle follows the spectrum, lows
// on the left and highs on the right, mirrored top to bottom, swelling with the bass and flashing on hits.
namespace luciole::workbench {
  // Texels of the band row the ring is drawn from: one per band.
  inline constexpr std::size_t kHaloTexels = kAudioBandCount;

  // The ring's shape in units of the box's shorter side, centered in the box.
  struct HaloRing {
    float radius = 0; // where the ring lies under a silent band
    float reach = 0;  // how much further out a full band pushes it
    float wobble = 0; // amplitude of a sevenfold ripple around it
    float phase = 0;  // the ripple's turn in radians, from 0 to 2 pi
    float flash = 0;  // from 1 on a hit down to 0
  };

  class HaloField {
  public:
    void reset();
    void advance(const AudioFrame& frame);
    HaloRing ring() const;
    // The bands as RGBA8 texels, as many as fit in `rgba`, each band softened with its neighbors by 1, 2, 1 so one
    // loud band makes a rounded bump: red is the band from 0 to 255, green and blue 0, alpha 255.
    void writeRow(std::span<std::uint8_t> rgba) const;

  private:
    std::array<float, kAudioBandCount> m_bands{};
    float m_bass = 0;
    float m_middle = 0;
    float m_clock = 0;
    float m_kick = 0;
    FlashLimiter m_flash;
  };
} // namespace luciole::workbench
