#include "ui/controls/audio_visual.h"
#include "ui/controls/audio_visual_lines.h"
#include "ui/controls/audio_visual_mesh.h"
#include "workbench/ring_field.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Cosmic Peanut: waveform rings on an implied sphere. Each ring is drawn as a closed loop of GL lines whose points
// the vertex shader places from the ring's row, the newest row and the share of a push interval gone, so a frame
// uploads only the rings pushed since the last draw. The rings add light on a dark background and cover a light one.
namespace {
  using luciole::workbench::kRingAmplitude;
  using luciole::workbench::kRingCount;
  using luciole::workbench::kRingPoints;

  constexpr float kGain = 0.5F;   // overall brightness of one ring
  constexpr float kLift = 0.042F; // brightening per unit of pulse

  const std::string kVertexShader = std::format(
      R"(
precision highp float;

attribute vec4 a_place;
attribute float a_value;
uniform float u_head;
uniform float u_count;
uniform float u_fraction;
uniform float u_swell;
uniform float u_lift;
uniform float u_near;
uniform float u_cover;
uniform float u_opacity;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec3 u_newest;
uniform vec3 u_middle;
uniform vec3 u_oldest;
uniform vec3 u_highlight;
varying vec4 v_color;

const float ROWS = {:.1f};
const float POINTS = {:.1f};
const float AMPLITUDE = {:.6f};
const float GAIN = {:.6f};
const float PI = 3.14159265;

void main() {{
    float age = mod(u_head - a_place.x + ROWS, ROWS);
    if (age >= u_count || age >= ROWS - 1.0) {{
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_color = vec4(0.0);
        return;
    }}
    float t = (age + u_fraction) / (ROWS - 1.0);
    float theta = t * PI;
    float phi = a_place.y / POINTS * 2.0 * PI;
    float r = (1.0 + AMPLITUDE * a_value) * u_swell;
    vec4 view = u_view * vec4(r * vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi)), 1.0);
    float back = clamp((-view.z - u_near) * 0.5, 0.0, 1.0);
    float edges = smoothstep(0.0, 0.035, t) * (1.0 - smoothstep(0.88, 1.0, t));
    float weight = edges * mix(1.0, 0.16, back) * mix(1.5, 0.35, t) * GAIN;
    vec3 ramp = t < 0.5 ? mix(u_newest, u_middle, t * 2.0) : mix(u_middle, u_oldest, t * 2.0 - 1.0);
    vec3 color = ramp + u_highlight * (abs(a_value) * 0.15 + u_lift);
    float alpha = min(weight, 1.0) * u_cover;
    vec3 light = u_cover > 0.5 ? min(color, vec3(1.0)) * alpha : color * weight;
    v_color = vec4(light, alpha) * u_opacity;
    gl_Position = u_projection * view;
}}
)",
      static_cast<float>(kRingCount), static_cast<float>(kRingPoints), kRingAmplitude, kGain
  );

  constexpr char kFragmentShader[] = R"(
precision mediump float;

varying vec4 v_color;

void main() {
    gl_FragColor = v_color;
}
)";

  float luminance(const Color& c) { return (0.2126F * c.r) + (0.7152F * c.g) + (0.0722F * c.b); }

  class CosmicPeanutVisual final : public AudioVisual {
  public:
    CosmicPeanutVisual()
        : m_mesh(
              {.rows = kRingCount,
               .verticesPerRow = kRingPoints,
               .valuesPerVertex = 1,
               .indicesPerRow = 2 * kRingPoints},
              kVertexShader.c_str(), kFragmentShader
          ) {}

    void reset() override {
      m_field.reset();
      m_mesh.invalidate();
    }

    void advance(const luciole::workbench::AudioFrame& frame) override { m_field.advance(frame); }

    void draw(const AudioVisualDraw& draw) override {
      if (draw.rect.width <= 0 || draw.rect.height <= 0) {
        return;
      }
      AudioVisualGlState saved;
      if (!m_mesh.ready()) {
        create();
      }
      m_mesh.bind(saved);
      const auto rows = m_field.rows();
      m_mesh.sync(m_field.pushes(), m_field.head(), [&](std::size_t row, std::span<float> out) {
        std::ranges::copy(rows.subspan(row * kRingPoints, kRingPoints), out.begin());
      });
      glViewport(draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height);
      setUniforms(draw);
      m_mesh.drawOldestFirst(GL_LINES, m_field.head());
    }

    void release() override { m_mesh.release(); }

    void abandon() noexcept override { m_mesh.abandon(); }

  private:
    struct Uniforms {
      GLint head = -1;
      GLint count = -1;
      GLint fraction = -1;
      GLint swell = -1;
      GLint lift = -1;
      GLint near = -1;
      GLint cover = -1;
      GLint opacity = -1;
      GLint view = -1;
      GLint projection = -1;
      GLint newest = -1;
      GLint middle = -1;
      GLint oldest = -1;
      GLint highlight = -1;
    };

    void create() {
      std::vector<AudioRowMesh::Place> places;
      std::vector<GLushort> indices;
      places.reserve(kRingCount * kRingPoints);
      indices.reserve(kRingCount * kRingPoints * 2);
      for (std::size_t row = 0; row < kRingCount; ++row) {
        const std::size_t first = row * kRingPoints;
        for (std::size_t point = 0; point < kRingPoints; ++point) {
          places.push_back({static_cast<std::uint8_t>(row), static_cast<std::uint8_t>(point), 0, 0});
          // Each point joins the next, the last the first, so the ring closes.
          indices.push_back(static_cast<GLushort>(first + point));
          indices.push_back(static_cast<GLushort>(first + ((point + 1) % kRingPoints)));
        }
      }
      m_mesh.create(places, indices);
      m_uniforms = {
          .head = m_mesh.uniform("u_head"),
          .count = m_mesh.uniform("u_count"),
          .fraction = m_mesh.uniform("u_fraction"),
          .swell = m_mesh.uniform("u_swell"),
          .lift = m_mesh.uniform("u_lift"),
          .near = m_mesh.uniform("u_near"),
          .cover = m_mesh.uniform("u_cover"),
          .opacity = m_mesh.uniform("u_opacity"),
          .view = m_mesh.uniform("u_view"),
          .projection = m_mesh.uniform("u_projection"),
          .newest = m_mesh.uniform("u_newest"),
          .middle = m_mesh.uniform("u_middle"),
          .oldest = m_mesh.uniform("u_oldest"),
          .highlight = m_mesh.uniform("u_highlight"),
      };
    }

    void setUniforms(const AudioVisualDraw& draw) const {
      const float aspect = static_cast<float>(draw.rect.width) / static_cast<float>(draw.rect.height);
      const auto view = luciole::workbench::ringView(m_field.camera(), aspect);
      const auto& palette = draw.palette;
      glUniform1f(m_uniforms.head, static_cast<float>(m_field.head()));
      glUniform1f(m_uniforms.count, static_cast<float>(m_field.count()));
      glUniform1f(m_uniforms.fraction, m_field.fraction());
      glUniform1f(m_uniforms.swell, luciole::workbench::ringSwell(m_field.pulse()));
      glUniform1f(m_uniforms.lift, kLift * m_field.pulse());
      glUniform1f(m_uniforms.near, view.distance - 1);
      glUniform1f(m_uniforms.cover, luminance(palette.background) < luminance(palette.text) ? 0.0F : 1.0F);
      glUniform1f(m_uniforms.opacity, audioVisualOpacity(draw));
      glUniformMatrix4fv(m_uniforms.view, 1, GL_FALSE, view.view.data());
      glUniformMatrix4fv(m_uniforms.projection, 1, GL_FALSE, view.projection.data());
      // The newest rings take the second accent, the middle the third, the oldest the first.
      setColorUniform(m_uniforms.newest, palette.accents[1]);
      setColorUniform(m_uniforms.middle, palette.accents[2]);
      setColorUniform(m_uniforms.oldest, palette.accents[0]);
      setColorUniform(m_uniforms.highlight, palette.text);
    }

    luciole::workbench::RingField m_field;
    AudioRowMesh m_mesh;
    Uniforms m_uniforms;
  };
} // namespace

std::unique_ptr<AudioVisual> makeCosmicPeanutVisual() { return std::make_unique<CosmicPeanutVisual>(); }
