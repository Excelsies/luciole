#include "ui/controls/audio_history_canvas.h"
#include "ui/controls/audio_visual.h"
#include "workbench/audio_history.h"
#include "workbench/radar_sweep.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace {
  using luciole::workbench::AudioFrame;
  using luciole::workbench::AudioHistory;
  using luciole::workbench::kRadarBearings;
  using luciole::workbench::kRadarRadii;
  using luciole::workbench::RadarSweep;

  // A round scope over the polar history: each pixel reads the echoes the arm last wrote at its radius in the two
  // nearest rows, faded by the turns since then, with a soft glow from four nearby echoes. The arm, the fan of light
  // behind it, the range rings, the crosshair, the bearing ticks and the bezel are drawn from the geometry alone. The
  // phosphor and the marks are painted in u_glow and the arm and the freshest echoes in u_hot over it.
  constexpr char kRadarShader[] = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

uniform sampler2D u_history;
uniform float u_rows;
uniform float u_radius;
uniform float u_line;
uniform float u_turn;
uniform float u_lap;
uniform float u_gain;
uniform vec3 u_glow;
uniform vec3 u_hot;
uniform float u_opacity;
varying vec2 v_point;

const float TAU = 6.28318530718;
const float AFTERGLOW = 1.0;
const float BEZEL = 0.035;
const float GRATICULE = 0.6;
const int ARM_REACH = 5;

float line(float d, float w) {
    return 1.0 - smoothstep(w, w + 1.0, abs(d));
}

float bearingOf(vec2 p) {
    return fract(atan(p.x, p.y) / TAU);
}

// Whether `texel` of history row `row` was written on the arm's latest pass over that row.
bool latest(vec4 texel, float row, float armRow) {
    float lap = u_lap - (row > armRow ? 1.0 : 0.0);
    return texel.a > 0.5 && mod(lap - floor(texel.g * 255.0 + 0.5), 256.0) < 0.5;
}

// The echo of history row `row` at radius r, faded by the turns since the arm left the row. A row the arm stepped
// over takes the echo of the row that frame wrote, at most ARM_REACH rows on, which counts the rows it stepped over;
// a row the arm never reached shows nothing. Reading at a row's center keeps the filter from mixing two rows.
float rowEcho(float row, float r) {
    float armRow = floor(u_turn * u_rows);
    for (int k = 0; k <= ARM_REACH; k++) {
        float at = mod(row + float(k), u_rows);
        vec4 texel = texture2D(u_history, vec2(r, (at + 0.5) / u_rows));
        if (latest(texel, at, armRow)) {
            if (floor(texel.b * 255.0 + 0.5) < float(k)) {
                return 0.0;
            }
            float age = at == armRow ? 0.0 : fract(u_turn - (at + 1.0) / u_rows);
            return texel.r * exp(-age / AFTERGLOW);
        }
    }
    return 0.0;
}

// The echo at p, blended between the two nearest rows.
float phosphor(vec2 p) {
    float r = length(p);
    if (r > 1.0 || r < 0.00001) {
        return 0.0;
    }
    float position = bearingOf(p) * u_rows - 0.5;
    float below = floor(position);
    float near = mod(below + u_rows, u_rows);
    float far = mod(below + 1.0, u_rows);
    return mix(rowEcho(near, r), rowEcho(far, r), position - below);
}

float respond(float v) {
    return pow(max(v, 0.0), 1.6);
}

void main() {
    vec2 p = v_point;
    float r = length(p);
    if (r > 1.0 + BEZEL + 3.0 / u_radius) {
        gl_FragColor = vec4(0.0);
        return;
    }
    float bearing = r > 0.00001 ? bearingOf(p) : 0.0;
    float s = u_line;

    float echo = respond(phosphor(p));
    float k = 0.012;
    float halo = phosphor(p + vec2(k, 0.0)) + phosphor(p - vec2(k, 0.0))
        + phosphor(p + vec2(0.0, k)) + phosphor(p - vec2(0.0, k));
    halo = respond(halo * 0.25);

    float inside = step(r, 1.0);
    float behind = fract(u_turn - bearing);
    vec2 arm = vec2(sin(u_turn * TAU), cos(u_turn * TAU));
    float across = abs(p.x * arm.y - p.y * arm.x) * u_radius;
    float beam = dot(p, arm) > 0.0 ? line(across, 0.9 * s) + 0.5 * exp(-across / (5.0 * s)) : 0.0;
    beam *= inside * (0.55 + 0.45 * smoothstep(0.0, 0.3, r));
    float fan = inside * exp(-behind / 0.035) * 0.16 * smoothstep(0.0, 0.2, r);

    float marks = 0.0;
    if (r <= 1.0) {
        float ring = floor(r * 4.0 + 0.5);
        float ringWeight = ring >= 4.0 ? 1.0 : (ring >= 1.0 ? 0.6 : 0.0);
        marks += line((r - ring * 0.25) * u_radius, 0.5 * s) * ringWeight;
        marks += 0.5 * (line(p.x * u_radius, 0.4 * s) + line(p.y * u_radius, 0.4 * s));
        float degrees = bearing * 360.0;
        float tick = mod(degrees + 2.5, 30.0) < 5.0 ? 0.07 : (mod(degrees + 2.5, 10.0) < 5.0 ? 0.04 : 0.02);
        float nearest = floor(degrees / 5.0 + 0.5) * 5.0;
        float apart = abs(degrees - nearest) / 360.0 * TAU * r * u_radius;
        float tickWeight = mod(nearest + 2.5, 10.0) < 5.0 ? 1.0 : 0.6;
        marks += line(apart, 0.5 * s) * step(1.0 - tick, r) * tickWeight;
    }
    float bezel = 0.0;
    if (r > 1.0) {
        float t = (r - 1.0) / BEZEL;
        bezel = 0.10 * (1.0 - smoothstep(0.0, 1.0, t)) * (0.7 + 0.3 * p.y / max(r, 0.0001));
        bezel += 0.35 * line((r - 1.0 - BEZEL) * u_radius, 0.6 * s);
    }
    marks = min(marks, 1.2) * GRATICULE * 0.28;

    float glowLight = (1.15 * echo + 0.35 * halo + fan + marks + bezel * GRATICULE) * u_gain;
    float hotLight = (0.35 * echo * echo + 0.9 * beam) * u_gain;
    float glowCover = 1.0 - exp(-1.3 * glowLight);
    float hotCover = 1.0 - exp(-1.3 * hotLight);
    vec3 color = u_hot * hotCover + u_glow * glowCover * (1.0 - hotCover);
    float alpha = hotCover + glowCover * (1.0 - hotCover);
    gl_FragColor = vec4(color, alpha) * u_opacity;
}
)";
  static_assert(luciole::workbench::kRadarArmReach == 5, "the shader's ARM_REACH");

  // How much a hit lifts the whole scope, the bezel's width as a share of the radius, and the pixels drawn past it.
  constexpr float kKickLift = 0.2F;
  constexpr float kBezel = 0.035F;
  constexpr float kEdgePixels = 4;

  class RadarVisual final : public AudioVisual {
  public:
    RadarVisual() : m_history(kRadarRadii, kRadarBearings), m_canvas(kRadarShader, HistorySampling::Smooth) {}

    void reset() override {
      m_sweep.reset();
      m_history.clear();
    }

    void advance(const AudioFrame& frame) override { m_sweep.advance(frame, m_history); }

    void draw(const AudioVisualDraw& draw) override {
      if (draw.rect.width <= 0 || draw.rect.height <= 0) {
        return;
      }
      const auto width = static_cast<float>(draw.rect.width);
      const auto height = static_cast<float>(draw.rect.height);
      const auto scope = luciole::workbench::fitRadarScope(width, height);
      AudioVisualGlState saved;
      if (m_canvas.begin(saved, draw.rect, m_history)) {
        lookUpUniforms();
      }
      const float lineScale = std::isfinite(draw.scale) && draw.scale > 0 ? draw.scale : 1.0F;
      const Color& glow = draw.palette.accents[2];
      const Color& hot = draw.palette.text;
      glUniform1f(m_rows, static_cast<float>(kRadarBearings));
      glUniform1f(m_radius, scope.radius);
      glUniform1f(m_line, lineScale);
      glUniform1f(m_turn, m_sweep.bearing());
      glUniform1f(m_lap, static_cast<float>(m_sweep.lap()));
      glUniform1f(m_gain, 1 + (kKickLift * m_sweep.kick()));
      glUniform3f(m_glow, glow.r, glow.g, glow.b);
      glUniform3f(m_hot, hot.r, hot.g, hot.b);
      glUniform1f(m_opacity, std::isfinite(draw.node.opacity) ? std::clamp(draw.node.opacity, 0.0F, 1.0F) : 0.0F);
      const float reach = (scope.radius * (1 + kBezel)) + (kEdgePixels * lineScale);
      const std::array extent{std::min(1.0F, reach / (width / 2)), std::min(1.0F, reach / (height / 2))};
      m_canvas.drawSquare(extent, {extent[0] * width / 2 / scope.radius, extent[1] * height / 2 / scope.radius});
    }

    void release() override { m_canvas.release(); }
    void abandon() noexcept override { m_canvas.abandon(); }

  private:
    void lookUpUniforms() {
      m_rows = m_canvas.uniform("u_rows");
      m_radius = m_canvas.uniform("u_radius");
      m_line = m_canvas.uniform("u_line");
      m_turn = m_canvas.uniform("u_turn");
      m_lap = m_canvas.uniform("u_lap");
      m_gain = m_canvas.uniform("u_gain");
      m_glow = m_canvas.uniform("u_glow");
      m_hot = m_canvas.uniform("u_hot");
      m_opacity = m_canvas.uniform("u_opacity");
    }

    RadarSweep m_sweep;
    AudioHistory m_history;
    AudioHistoryCanvas m_canvas;
    GLint m_rows = -1;
    GLint m_radius = -1;
    GLint m_line = -1;
    GLint m_turn = -1;
    GLint m_lap = -1;
    GLint m_gain = -1;
    GLint m_glow = -1;
    GLint m_hot = -1;
    GLint m_opacity = -1;
  };
} // namespace

std::unique_ptr<AudioVisual> makeRadarVisual() { return std::make_unique<RadarVisual>(); }
