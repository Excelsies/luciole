#pragma once

#include "workbench/audio_frame.h"
#include "workbench/audio_history.h"
#include "workbench/stargate_motion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The stargate visual's history rows and the state that writes them.
namespace luciole::workbench {
  // Builds history rows of kStargateLanes RGBA texels from the audio:
  //   red    the first half of the waveform from its first rising zero crossing, so a steady tone stays in place
  //          from row to row, lifted by a slowly falling peak level (twelve and a half times at most) and stored as
  //          0.5 + value / 2;
  //   green  each lane's size, held and fading by 7 % a row, which draws the streaks;
  //   blue   the centroid of the bands, 0 for the lowest to 1 for the highest, 0.5 in silence;
  //   alpha  the mean band.
  class StargateRowBuilder {
  public:
    void reset();
    // Throws std::invalid_argument for a row that is not kStargateLanes texels.
    void build(const AudioFrame& frame, std::span<std::uint8_t> row);

  private:
    std::array<float, kStargateLanes> m_streaks{};
    float m_level = 0;
  };

  // The stargate's whole state apart from its texture: the roll, the speed, the pulse, the horizon and the rows.
  class StargateFlight {
  public:
    void reset();
    // Moves on by the frame's step and writes a row into `history` when one falls due. Throws std::invalid_argument
    // for a history that is not kStargateLanes by kStargateRows.
    void advance(const AudioFrame& frame, AudioHistory& history);

    float roll() const { return m_roll.angle(); }
    float pulse() const { return m_pulse.value(); }
    // The ring row of the newest line, and how far the lines have glided toward the next push.
    std::size_t head() const { return m_schedule.head(); }
    float fraction() const { return m_schedule.fraction(); }
    float surge() const { return m_surge.value(); }
    const StargateGlow& glow() const { return m_glow; }

  private:
    StargateRoll m_roll;
    StargateSurge m_surge;
    StargatePulse m_pulse;
    StargateSchedule m_schedule;
    StargateGlow m_glow;
    StargateRowBuilder m_builder;
  };
} // namespace luciole::workbench
