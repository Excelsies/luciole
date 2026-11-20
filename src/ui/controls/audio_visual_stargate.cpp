#include "ui/controls/audio_history_canvas.h"
#include "ui/controls/audio_visual.h"
#include "workbench/audio_history.h"
#include "workbench/stargate_motion.h"
#include "workbench/stargate_rows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace {
  using luciole::workbench::AudioFrame;
  using luciole::workbench::AudioHistory;
  using luciole::workbench::kStargateLanes;
  using luciole::workbench::kStargateRows;
  using luciole::workbench::StargateFlight;

  // Two endless planes, above and below the eye, seen while traveling between them. Each pixel is turned back by
  // the roll and cast onto the plane on its side; the depth it meets picks a history row, newest at the far end, and
  // the side position picks a lane, so the lines stream toward the eye as rows are pushed. Four reads along the depth
  // smear each lane into a streak. Far lanes fade into the horizon, which glows in the color of the newest sound. The
  // lanes are painted along a ramp through the three accents and their hot cores in u_core.
  constexpr char kStargateShader[] = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

uniform sampler2D u_history;
uniform vec2 u_roll;
uniform float u_head;
uniform float u_glide;
uniform float u_smear;
uniform float u_footprint;
uniform float u_drift;
uniform float u_bright;
uniform float u_glowAt;
uniform float u_glowLevel;
uniform vec3 u_accent0;
uniform vec3 u_accent1;
uniform vec3 u_accent2;
uniform vec3 u_core;
uniform float u_opacity;
varying vec2 v_point;

const float ROWS = 128.0;
const float FAR = 14.0;
const float NEAR = 0.24;
const float LANE = 0.32;

vec3 ramp(float t) {
    float x = fract(t) * 3.0;
    if (x < 1.0) {
        return mix(u_accent0, u_accent1, smoothstep(0.0, 1.0, x));
    }
    if (x < 2.0) {
        return mix(u_accent1, u_accent2, smoothstep(0.0, 1.0, x - 1.0));
    }
    return mix(u_accent2, u_accent0, smoothstep(0.0, 1.0, x - 2.0));
}

void main() {
    vec2 p = v_point;
    vec2 q = vec2(u_roll.x * p.x + u_roll.y * p.y, u_roll.x * p.y - u_roll.y * p.x);
    float height = max(abs(q.y), 0.0001);
    float side = q.y >= 0.0 ? 1.0 : -1.0;
    float depth = 1.0 / height;
    float age = (FAR - depth) / ((FAR - NEAR) / ROWS) - u_glide;
    float lane = q.x * depth * LANE + (side > 0.0 ? 0.0 : 0.37);
    float detail = clamp(2.0 - 2.0 * depth * u_footprint, 0.0, 1.0);

    float light = 0.0;
    float core = 0.0;
    float hue = 0.0;
    float energy = 0.0;
    float weights = 0.0;
    for (int k = 0; k < 4; k++) {
        float fk = float(k);
        float rowAge = age + fk * u_smear / 3.0;
        float weight = (1.0 - 0.22 * fk) * step(0.0, rowAge) * step(rowAge, ROWS - 1.5);
        vec4 texel = texture2D(u_history, vec2(lane, fract((u_head - rowAge + 0.5) / ROWS)));
        float wave = max(texel.r * 2.0 - 1.0, 0.0);
        float wave2 = wave * wave;
        float streak = texel.g;
        float streak2 = streak * streak;
        float streak4 = streak2 * streak2;
        light += weight * (mix(0.2, streak2 * streak, detail) + 0.45 * detail * wave2 * wave2);
        core += weight * streak4 * streak4 * detail;
        hue += weight * texel.b;
        energy += weight * texel.a;
        weights += weight;
    }
    float share = weights > 0.0 ? 1.0 / weights : 0.0;
    float fade = 1.0 - smoothstep(FAR * 0.7, FAR, depth);
    float gain = (0.35 + 3.0 * energy * share) * fade;
    float t = hue * share * 1.7 + 0.3 * side + 0.22 * sin(lane * 4.0) + 0.1 * sin(lane * 23.0) + u_drift;

    float laneLight = light * share * gain * 2.2;
    float glowLight = u_glowLevel * (0.9 * exp(-height * 60.0) + 0.3 * exp(-height * 14.0));
    float hold = (0.6 + 0.4 * smoothstep(0.0, 0.7, length(p))) * u_bright;
    vec3 color = (ramp(t) * laneLight + ramp(u_glowAt) * glowLight) / max(laneLight + glowLight, 0.0001);
    float colorCover = 1.0 - exp(-(laneLight + glowLight) * hold);
    float coreCover = 1.0 - exp(-core * share * gain * 0.9 * hold);
    vec3 rgb = u_core * coreCover + color * colorCover * (1.0 - coreCover);
    float alpha = coreCover + colorCover * (1.0 - coreCover);
    gl_FragColor = vec4(rgb, alpha) * u_opacity;
}
)";

  // How far the streaks smear with the speed, how bright the horizon glows at rest and with the bands, how far along
  // the ramp the bands move its color, and the lane width in texture widths per unit of side distance, as the shader
  // has it.
  constexpr float kStreak = 0.5F;
  constexpr float kGlowRest = 0.3F;
  constexpr float kGlowBands = 1.4F;
  constexpr float kHueSpread = 1.7F;
  constexpr float kLane = 0.32F;

  class StargateVisual final : public AudioVisual {
  public:
    StargateVisual()
        : m_history(kStargateLanes, kStargateRows), m_canvas(kStargateShader, HistorySampling::SmoothRepeat) {}

    void reset() override {
      m_flight.reset();
      m_history.clear();
    }

    void advance(const AudioFrame& frame) override { m_flight.advance(frame, m_history); }

    void draw(const AudioVisualDraw& draw) override {
      if (draw.rect.width <= 0 || draw.rect.height <= 0) {
        return;
      }
      const auto width = static_cast<float>(draw.rect.width);
      const auto height = static_cast<float>(draw.rect.height);
      AudioVisualGlState saved;
      if (m_canvas.begin(saved, draw.rect, m_history)) {
        lookUpUniforms();
      }
      const auto& glow = m_flight.glow();
      const auto& accents = draw.palette.accents;
      glUniform2f(m_roll, std::cos(m_flight.roll()), std::sin(m_flight.roll()));
      glUniform1f(m_head, static_cast<float>(m_flight.head()));
      glUniform1f(m_glide, m_flight.fraction());
      glUniform1f(m_smear, 1 + (3 * kStreak * m_flight.surge()));
      glUniform1f(m_footprint, 2 / height * kLane * static_cast<float>(kStargateLanes));
      glUniform1f(m_drift, glow.drift());
      glUniform1f(m_bright, m_flight.pulse());
      glUniform1f(m_glowAt, (glow.hue() * kHueSpread) + glow.drift());
      glUniform1f(m_glowLevel, (kGlowRest + (kGlowBands * glow.energy())) * m_flight.pulse());
      glUniform3f(m_accents[0], accents[0].r, accents[0].g, accents[0].b);
      glUniform3f(m_accents[1], accents[1].r, accents[1].g, accents[1].b);
      glUniform3f(m_accents[2], accents[2].r, accents[2].g, accents[2].b);
      glUniform3f(m_core, draw.palette.text.r, draw.palette.text.g, draw.palette.text.b);
      glUniform1f(m_opacity, std::isfinite(draw.node.opacity) ? std::clamp(draw.node.opacity, 0.0F, 1.0F) : 0.0F);
      m_canvas.drawSquare({1, 1}, {width / height, 1});
    }

    void release() override { m_canvas.release(); }
    void abandon() noexcept override { m_canvas.abandon(); }

  private:
    void lookUpUniforms() {
      m_roll = m_canvas.uniform("u_roll");
      m_head = m_canvas.uniform("u_head");
      m_glide = m_canvas.uniform("u_glide");
      m_smear = m_canvas.uniform("u_smear");
      m_footprint = m_canvas.uniform("u_footprint");
      m_drift = m_canvas.uniform("u_drift");
      m_bright = m_canvas.uniform("u_bright");
      m_glowAt = m_canvas.uniform("u_glowAt");
      m_glowLevel = m_canvas.uniform("u_glowLevel");
      m_accents = {m_canvas.uniform("u_accent0"), m_canvas.uniform("u_accent1"), m_canvas.uniform("u_accent2")};
      m_core = m_canvas.uniform("u_core");
      m_opacity = m_canvas.uniform("u_opacity");
    }

    StargateFlight m_flight;
    AudioHistory m_history;
    AudioHistoryCanvas m_canvas;
    GLint m_roll = -1;
    GLint m_head = -1;
    GLint m_glide = -1;
    GLint m_smear = -1;
    GLint m_footprint = -1;
    GLint m_drift = -1;
    GLint m_bright = -1;
    GLint m_glowAt = -1;
    GLint m_glowLevel = -1;
    std::array<GLint, 3> m_accents{-1, -1, -1};
    GLint m_core = -1;
    GLint m_opacity = -1;
  };
} // namespace

std::unique_ptr<AudioVisual> makeStargateVisual() { return std::make_unique<StargateVisual>(); }
