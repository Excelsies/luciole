#include "workbench/pillar_ring.h"

#include "workbench/spectrum_meter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace luciole::workbench {
  namespace {
    constexpr float kTau = 2 * std::numbers::pi_v<float>;
    constexpr float kRestingHeight = 0.08F;
    constexpr float kHeightPerLevel = 3.2F;
    constexpr float kLevelKeep = 0.75F;
    constexpr float kBassKeep = 0.9F;
    constexpr float kMaxBass = 2;
    constexpr float kCoreRadius = 0.7F;
    constexpr float kCorePerBass = 0.35F;
    constexpr float kKickDecayPerSecond = 5;

    // One turn around the ring in 32 s, run faster by up to 2.5 times while a hit's pulse lasts.
    constexpr float kOrbitPerSecond = kTau / 32;
    constexpr float kOrbitPerPulse = 1.5F;
    constexpr float kCoreTurnPerSecond = 0.3F;
    constexpr float kCoreTiltPerSecond = 0.17F;
    constexpr float kStarTurnPerSecond = -0.01F;

    // The camera: distance from the ring's axis and height, how far a full dip moves it in and down, the height it
    // sways by over two turns, and the point it looks at.
    constexpr float kEyeDistance = 11;
    constexpr float kEyeHeight = 4.6F;
    constexpr float kEyeSway = 1.2F;
    constexpr float kDipIn = 0.6F;
    constexpr float kDipDown = 0.25F;
    constexpr OrbitPoint kTarget{0, 0.9F, 0};
    constexpr float kHalfField = 25 * std::numbers::pi_v<float> / 180;
    constexpr float kNear = 0.1F;
    constexpr float kFar = 200;

    OrbitPoint minus(const OrbitPoint& a, const OrbitPoint& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

    float dot(const OrbitPoint& a, const OrbitPoint& b) { return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]); }

    OrbitPoint cross(const OrbitPoint& a, const OrbitPoint& b) {
      return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]), (a[0] * b[1]) - (a[1] * b[0])};
    }

    OrbitPoint unit(const OrbitPoint& v) {
      const float length = std::sqrt(dot(v, v));
      return {v[0] / length, v[1] / length, v[2] / length};
    }

    float pillarAngle(std::size_t index) { return static_cast<float>(index) * kTau / kPillarCount; }

    float advanced(float angle, float by, float period) { return std::fmod(angle + by, period); }

    // The squared distance on the floor from `eye` to pillar `index`.
    float floorDistance(std::size_t index, const OrbitPoint& eye) {
      const float dx = (kPillarRingRadius * std::cos(pillarAngle(index))) - eye[0];
      const float dz = (kPillarRingRadius * std::sin(pillarAngle(index))) - eye[2];
      return (dx * dx) + (dz * dz);
    }
  } // namespace

  std::size_t pillarBand(std::size_t index) {
    if (index >= kPillarCount) {
      return 0;
    }
    return index < kPillarCount / 2 ? index * 2 : (kPillarCount - 1 - index) * 2;
  }

  float pillarHeight(float level) {
    const float clamped = std::isnan(level) ? 0.0F : std::clamp(level, 0.0F, 1.0F);
    return kRestingHeight + (clamped * kHeightPerLevel);
  }

  std::array<std::uint8_t, kPillarCount> drawOrder(const OrbitPoint& eye) {
    std::array<float, kPillarCount> distance{};
    std::array<std::uint8_t, kPillarCount> order{};
    for (std::size_t i = 0; i < kPillarCount; ++i) {
      distance[i] = floorDistance(i, eye);
      order[i] = static_cast<std::uint8_t>(i);
    }
    std::ranges::sort(order, [&distance](std::uint8_t a, std::uint8_t b) {
      return distance[a] != distance[b] ? distance[a] > distance[b] : a < b;
    });
    return order;
  }

  std::size_t pillarsBehindCore(const OrbitPoint& eye) {
    const float center = (eye[0] * eye[0]) + (eye[2] * eye[2]);
    std::size_t behind = 0;
    for (std::size_t i = 0; i < kPillarCount; ++i) {
      behind += floorDistance(i, eye) > center ? 1 : 0;
    }
    return behind;
  }

  void PillarRing::reset() {
    m_levels.fill(0);
    m_bass = 0;
    m_kick = 0;
    m_flash.reset();
    m_orbit = 0;
    m_coreTurn = {};
    m_starTurn = 0;
  }

  void PillarRing::advance(const AudioFrame& frame) {
    const float seconds = frameStep(frame);
    std::array<float, kAudioBandCount> bands{};
    readBands(frame, bands);
    std::array<float, kPillarCount> targets{};
    for (std::size_t i = 0; i < kPillarCount; ++i) {
      targets[i] = bands.at(pillarBand(i));
    }
    smoothLevels(m_levels, targets, kLevelKeep, seconds);
    m_bass = easeToward(m_bass, std::min(bassRatio(frame), kMaxBass), kBassKeep, seconds);
    m_kick = kickEnvelope(m_kick, hitStrength(frame), kKickDecayPerSecond, seconds);
    m_flash.step(m_kick, seconds);
    const float speed = kOrbitPerSecond * (1 + (kOrbitPerPulse * m_flash.value()));
    m_orbit = advanced(m_orbit, seconds * speed, 2 * kTau);
    m_coreTurn = {
        advanced(m_coreTurn[0], seconds * kCoreTurnPerSecond, kTau),
        advanced(m_coreTurn[1], seconds * kCoreTiltPerSecond, kTau),
    };
    m_starTurn = advanced(m_starTurn, seconds * kStarTurnPerSecond, kTau);
  }

  float PillarRing::coreRadius() const { return kCoreRadius + (kCorePerBass * m_bass); }

  OrbitView PillarRing::view(float aspect) const {
    const float ratio = std::isfinite(aspect) && aspect > 0 ? aspect : 1.0F;
    const float dip = m_flash.value() * m_flash.value();
    const float distance = kEyeDistance - (kDipIn * dip);
    const OrbitPoint eye{
        std::cos(m_orbit) * distance,
        kEyeHeight + (std::sin(m_orbit * 0.5F) * kEyeSway) - (kDipDown * dip),
        std::sin(m_orbit) * distance,
    };
    const OrbitPoint forward = unit(minus(kTarget, eye));
    const OrbitPoint right = unit(cross(forward, {0, 1, 0}));
    const OrbitPoint up = cross(right, forward);

    const float tanVertical = std::tan(kHalfField) / std::min(ratio, 1.0F);
    const float sy = 1 / tanVertical;
    const float sx = sy / ratio;
    const float sz = (kFar + kNear) / (kNear - kFar);
    const float tz = 2 * kFar * kNear / (kNear - kFar);
    // The projection times the view, written out: rows of the view are right, up and minus forward.
    OrbitView view{.eye = eye, .right = right, .up = up};
    auto& m = view.viewProjection;
    for (std::size_t column = 0; column < 3; ++column) {
      m[(column * 4) + 0] = sx * right[column];
      m[(column * 4) + 1] = sy * up[column];
      m[(column * 4) + 2] = -sz * forward[column];
      m[(column * 4) + 3] = forward[column];
    }
    m[12] = -sx * dot(right, eye);
    m[13] = -sy * dot(up, eye);
    m[14] = (sz * dot(forward, eye)) + tz;
    m[15] = -dot(forward, eye);
    return view;
  }
} // namespace luciole::workbench
