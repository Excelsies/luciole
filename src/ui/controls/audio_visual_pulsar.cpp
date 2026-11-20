#include "ui/controls/audio_visual.h"
#include "ui/controls/audio_visual_lines.h"
#include "ui/controls/audio_visual_mesh.h"
#include "workbench/ridge_field.h"

#include <GLES2/gl2.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Pulsar: ridge lines stacked into the distance. Every line is a strip filled with the background from its curve
// down past the next line's baseline, then the line itself as a band of triangles with a soft edge, drawn back to
// front so a nearer line hides what is behind it. Each point has four vertices: the strip's bottom, the curve, and
// the line band's lower and upper edges; the vertex shader places them from the line's row, the newest row and the
// share of a push interval gone, so a frame uploads only the lines pushed since the last draw.
namespace {
  using luciole::workbench::kRidgeLines;
  using luciole::workbench::kRidgePoints;

  constexpr std::size_t kRoles = 4;           // vertices per point: strip bottom, curve, band lower, band upper
  constexpr float kLineWidth = 1.5F;          // logical pixels
  constexpr float kDistanceFade = 0.45F;      // how far the oldest line fades toward the background
  constexpr std::size_t kSegmentIndices = 12; // two strip triangles and two band triangles

  const std::string kVertexShader = std::format(
      R"(
precision highp float;

attribute vec4 a_place;
attribute vec2 a_value;
uniform float u_head;
uniform float u_fraction;
uniform vec2 u_size;
uniform vec4 u_plot;
uniform float u_amplitude;
uniform float u_half;
uniform float u_depth;
uniform float u_opacity;
uniform vec3 u_ground;
uniform vec3 u_front;
uniform vec3 u_back;
varying vec4 v_color;
varying vec2 v_edge;

const float LINES = {:.1f};
const float POINTS = {:.1f};
const float FADE = {:.6f};

void main() {{
    float age = mod(u_head - a_place.x + LINES, LINES);
    float pitch = u_plot.y / (POINTS - 1.0);
    float x = u_plot.x + a_place.y * pitch;
    float base = u_plot.z - (age + u_fraction) * u_plot.w;
    float y = base - a_value.x * u_amplitude;
    float role = a_place.z;
    float arrive = age < 0.5 ? u_fraction : (age > LINES - 1.5 ? 1.0 - u_fraction : 1.0);
    float reach = u_half + 1.0;
    v_edge = vec2(0.0, reach);
    if (role < 0.5) {{
        y = base + u_depth;
    }} else if (role > 1.5) {{
        float steep = a_value.y * u_amplitude / pitch;
        float side = role > 2.5 ? -1.0 : 1.0;
        y += side * reach * min(sqrt(1.0 + steep * steep), 4.0);
        v_edge.x = side * reach;
    }}
    if (role < 1.5) {{
        v_color = vec4(u_ground, 1.0) * (arrive * u_opacity);
    }} else {{
        float far = age / (LINES - 1.0);
        vec3 line = mix(mix(u_front, u_back, far), u_ground, far * FADE);
        v_color = vec4(line, 1.0) * (arrive * u_opacity);
    }}
    gl_Position = vec4(x / u_size.x * 2.0 - 1.0, 1.0 - y / u_size.y * 2.0, 0.0, 1.0);
}}
)",
      static_cast<float>(kRidgeLines), static_cast<float>(kRidgePoints), kDistanceFade
  );

  // v_edge holds the distance from the line's middle in pixels and the band's reach, half the width and a pixel.
  constexpr char kFragmentShader[] = R"(
precision mediump float;

varying vec4 v_color;
varying vec2 v_edge;

void main() {
    gl_FragColor = v_color * clamp(v_edge.y - 0.5 - abs(v_edge.x), 0.0, 1.0);
}
)";

  class PulsarVisual final : public AudioVisual {
  public:
    PulsarVisual()
        : m_mesh(
              {.rows = kRidgeLines,
               .verticesPerRow = kRidgePoints * kRoles,
               .valuesPerVertex = 2,
               .indicesPerRow = (kRidgePoints - 1) * kSegmentIndices},
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
        const auto line = rows.subspan(row * kRidgePoints, kRidgePoints);
        for (std::size_t point = 0; point < kRidgePoints; ++point) {
          const float height = line[point];
          const float slope = luciole::workbench::ridgeSlope(line, point);
          for (std::size_t role = 0; role < kRoles; ++role) {
            out[((point * kRoles) + role) * 2] = height;
            out[(((point * kRoles) + role) * 2) + 1] = slope;
          }
        }
      });
      glViewport(draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height);
      setUniforms(draw);
      m_mesh.drawOldestFirst(GL_TRIANGLES, m_field.head());
    }

    void release() override { m_mesh.release(); }

    void abandon() noexcept override { m_mesh.abandon(); }

  private:
    struct Uniforms {
      GLint head = -1;
      GLint fraction = -1;
      GLint size = -1;
      GLint plot = -1;
      GLint amplitude = -1;
      GLint half = -1;
      GLint depth = -1;
      GLint opacity = -1;
      GLint ground = -1;
      GLint front = -1;
      GLint back = -1;
    };

    void create() {
      std::vector<AudioRowMesh::Place> places;
      std::vector<GLushort> indices;
      places.reserve(kRidgeLines * kRidgePoints * kRoles);
      indices.reserve(kRidgeLines * (kRidgePoints - 1) * kSegmentIndices);
      for (std::size_t row = 0; row < kRidgeLines; ++row) {
        for (std::size_t point = 0; point < kRidgePoints; ++point) {
          for (std::size_t role = 0; role < kRoles; ++role) {
            places.push_back(
                {static_cast<std::uint8_t>(row), static_cast<std::uint8_t>(point), static_cast<std::uint8_t>(role), 0}
            );
          }
        }
        // The whole strip first, then the whole band, so the strip never covers the line's own lower half.
        for (const std::size_t role : {std::size_t{0}, std::size_t{2}}) {
          for (std::size_t point = 0; point + 1 < kRidgePoints; ++point) {
            const auto vertex = [&](std::size_t p, std::size_t r) {
              return static_cast<GLushort>((((row * kRidgePoints) + p) * kRoles) + r);
            };
            const std::array<GLushort, 6> quad{
                vertex(point, role),     vertex(point, role + 1),     vertex(point + 1, role),
                vertex(point, role + 1), vertex(point + 1, role + 1), vertex(point + 1, role),
            };
            indices.insert(indices.end(), quad.begin(), quad.end());
          }
        }
      }
      m_mesh.create(places, indices);
      m_uniforms = {
          .head = m_mesh.uniform("u_head"),
          .fraction = m_mesh.uniform("u_fraction"),
          .size = m_mesh.uniform("u_size"),
          .plot = m_mesh.uniform("u_plot"),
          .amplitude = m_mesh.uniform("u_amplitude"),
          .half = m_mesh.uniform("u_half"),
          .depth = m_mesh.uniform("u_depth"),
          .opacity = m_mesh.uniform("u_opacity"),
          .ground = m_mesh.uniform("u_ground"),
          .front = m_mesh.uniform("u_front"),
          .back = m_mesh.uniform("u_back"),
      };
    }

    void setUniforms(const AudioVisualDraw& draw) const {
      const auto width = static_cast<float>(draw.rect.width);
      const auto height = static_cast<float>(draw.rect.height);
      const auto layout = luciole::workbench::ridgeLayout(width, height);
      const float scale = std::isfinite(draw.scale) && draw.scale > 0 ? draw.scale : 1.0F;
      const float lineWidth = kLineWidth * scale;
      glUniform1f(m_uniforms.head, static_cast<float>(m_field.head()));
      glUniform1f(m_uniforms.fraction, m_field.fraction());
      glUniform2f(m_uniforms.size, width, height);
      glUniform4f(m_uniforms.plot, layout.left, layout.width, layout.bottom, layout.spacing);
      glUniform1f(m_uniforms.amplitude, layout.amplitude);
      glUniform1f(m_uniforms.half, lineWidth / 2);
      // Past the next line's baseline, so that line's strip covers this strip's bottom edge.
      glUniform1f(m_uniforms.depth, (2 * layout.spacing) + lineWidth);
      glUniform1f(m_uniforms.opacity, audioVisualOpacity(draw));
      // The front lines take the text color, the far ones the second accent, faded toward the background.
      setColorUniform(m_uniforms.ground, draw.palette.background);
      setColorUniform(m_uniforms.front, draw.palette.text);
      setColorUniform(m_uniforms.back, draw.palette.accents[1]);
    }

    luciole::workbench::RidgeField m_field;
    AudioRowMesh m_mesh;
    Uniforms m_uniforms;
  };
} // namespace

std::unique_ptr<AudioVisual> makePulsarVisual() { return std::make_unique<PulsarVisual>(); }
