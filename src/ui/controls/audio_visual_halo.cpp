#include "ui/controls/audio_visual.h"
#include "ui/controls/audio_visual_mesh.h"
#include "workbench/halo_field.h"

#include <array>
#include <cstdint>
#include <memory>

namespace {
  using luciole::workbench::HaloField;
  using luciole::workbench::HaloRing;
  using luciole::workbench::kHaloTexels;

  // v_point: the fragment's place from the box's center, in units of its shorter side, y up.
  constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec4 a_vertex;
uniform vec2 u_size;
varying vec2 v_point;

void main() {
    vec2 corner = a_vertex.zw;
    v_point = (corner - 0.5) * u_size / min(u_size.x, u_size.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

  // The ring's light falls off as one over the distance to it and is thicker where its band is loud; a soft glow
  // sits in the middle and brightens on a flash. Light from 0 up maps to the accent, then toward the text color, at
  // the coverage 1 - exp(-light). u_ring: radius, band reach, ripple amplitude, ripple phase.
  constexpr char kFragmentShader[] = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

uniform sampler2D u_bands;
uniform vec4 u_ring;
uniform float u_flash;
uniform vec3 u_accent;
uniform vec3 u_text;
uniform float u_opacity;
varying vec2 v_point;

const float PI = 3.14159265;
const float BANDS = 64.0;

void main() {
    float r = length(v_point);
    float a = atan(v_point.y, v_point.x);
    float f = (1.0 - abs(a) / PI) * (BANDS - 1.0);
    float band = texture2D(u_bands, vec2((f + 0.5) / BANDS, 0.5)).r;
    float radius = u_ring.x + u_ring.y * band + u_ring.z * sin(a * 7.0 + u_ring.w);
    float ring = 0.0035 * (1.0 + band) * (1.0 + 0.5 * u_flash) / max(abs(r - radius), 0.0001);
    float glow = 0.35 * exp(-r * 4.0) * (0.4 + u_flash);
    float coverage = 1.0 - exp(-(ring + glow));
    vec3 color = mix(u_accent, u_text, coverage * coverage);
    gl_FragColor = vec4(color * coverage, coverage) * u_opacity;
}
)";

  constexpr std::array<AudioVisualVertex, 6> kQuad{
      {{0, 0, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}, {0, 0, 1, 0}, {0, 0, 1, 1}, {0, 0, 0, 1}}
  };
  constexpr std::uint64_t kQuadShape = 1;

  struct HaloUniforms {
    GLint size = -1;
    GLint bands = -1;
    GLint ring = -1;
    GLint flash = -1;
    GLint accent = -1;
    GLint text = -1;
    GLint opacity = -1;
  };

  class HaloVisual final : public AudioVisual {
  public:
    void reset() override {
      m_field.reset();
      ++m_revision;
    }

    void advance(const luciole::workbench::AudioFrame& frame) override {
      m_field.advance(frame);
      ++m_revision;
    }

    void draw(const AudioVisualDraw& draw) override {
      if (draw.rect.width <= 0 || draw.rect.height <= 0) {
        return;
      }
      AudioVisualGlState saved;
      ensureObjects();
      saved.keepAttributes(std::array{m_mesh.vertexLocation()});
      m_mesh.bind();
      glBindTexture(GL_TEXTURE_2D, m_texture);
      if (m_uploaded != m_revision) {
        std::array<std::uint8_t, 4 * kHaloTexels> row{};
        m_field.writeRow(row);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kHaloTexels, 1, GL_RGBA, GL_UNSIGNED_BYTE, row.data());
        m_uploaded = m_revision;
      }
      glViewport(draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height);
      const HaloRing ring = m_field.ring();
      glUniform2f(m_uniforms.size, static_cast<float>(draw.rect.width), static_cast<float>(draw.rect.height));
      glUniform1i(m_uniforms.bands, 0);
      glUniform4f(m_uniforms.ring, ring.radius, ring.reach, ring.wobble, ring.phase);
      glUniform1f(m_uniforms.flash, ring.flash);
      setColorUniform(m_uniforms.accent, draw.palette.accents[0]);
      setColorUniform(m_uniforms.text, draw.palette.text);
      glUniform1f(m_uniforms.opacity, audioVisualOpacity(draw));
      glDrawArrays(GL_TRIANGLES, 0, m_mesh.vertexCount());
    }

    void release() override {
      m_mesh.release();
      if (m_texture != 0) {
        glDeleteTextures(1, &m_texture);
      }
      m_texture = 0;
      m_uploaded = m_revision - 1;
    }

    void abandon() noexcept override {
      m_mesh.abandon();
      m_texture = 0;
      m_uploaded = m_revision - 1;
    }

  private:
    // The program, the quad and the band row; texture unit 0 is active and the row stays bound.
    void ensureObjects() {
      if (!m_mesh.hasProgram()) {
        m_mesh.ensureProgram(kVertexShader, kFragmentShader);
        m_uniforms = {
            .size = m_mesh.uniform("u_size"),
            .bands = m_mesh.uniform("u_bands"),
            .ring = m_mesh.uniform("u_ring"),
            .flash = m_mesh.uniform("u_flash"),
            .accent = m_mesh.uniform("u_accent"),
            .text = m_mesh.uniform("u_text"),
            .opacity = m_mesh.uniform("u_opacity"),
        };
      }
      if (!m_mesh.holds(kQuadShape)) {
        m_mesh.fill(kQuadShape, kQuad);
      }
      if (m_texture == 0) {
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kHaloTexels, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_uploaded = m_revision - 1;
      }
    }

    HaloField m_field;
    AudioVisualMesh m_mesh;
    HaloUniforms m_uniforms;
    GLuint m_texture = 0;
    std::uint64_t m_revision = 1;
    std::uint64_t m_uploaded = 0;
  };
} // namespace

std::unique_ptr<AudioVisual> makeHaloVisual() { return std::make_unique<HaloVisual>(); }
