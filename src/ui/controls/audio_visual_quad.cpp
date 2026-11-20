#include "ui/controls/audio_visual_quad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace {
  constexpr char kVertexShader[] = R"(
attribute vec2 a_corner;
uniform vec2 u_size;
varying vec2 v_pixel;

void main() {
    v_pixel = vec2(a_corner.x, 1.0 - a_corner.y) * u_size;
    gl_Position = vec4(a_corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

  constexpr char kFragmentHead[] = R"(
precision highp float;

varying vec2 v_pixel;
uniform vec2 u_size;
uniform float u_opacity;
uniform sampler2D u_row;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float valueNoise(vec2 p, float period) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float x0 = mod(i.x, period);
    float x1 = mod(i.x + 1.0, period);
    float a = hash(vec2(x0, i.y));
    float b = hash(vec2(x1, i.y));
    float c = hash(vec2(x0, i.y + 1.0));
    float d = hash(vec2(x1, i.y + 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

vec4 over(vec4 below, vec3 color, float alpha) {
    return vec4(color * alpha, alpha) + below * (1.0 - alpha);
}
)";

  // The rectangle as a strip of two triangles, from its bottom left corner.
  constexpr std::array<float, 8> kCorners{0, 0, 1, 0, 0, 1, 1, 1};
} // namespace

bool AudioVisualQuad::begin(const AudioVisualDraw& draw, AudioVisualGlState& saved) {
  bool made = false;
  if (!m_program.isValid()) {
    const std::string fragment = std::string(kFragmentHead) + m_fragmentSource;
    m_program.create(kVertexShader, fragment.c_str());
    m_corner = glGetAttribLocation(m_program.id(), "a_corner");
    m_size = glGetUniformLocation(m_program.id(), "u_size");
    m_opacity = glGetUniformLocation(m_program.id(), "u_opacity");
    m_rowSampler = glGetUniformLocation(m_program.id(), "u_row");
    if (m_corner < 0) {
      m_program.destroy();
      throw std::runtime_error("a sound visual's program has no corner attribute");
    }
    made = true;
  }
  if (m_buffer == 0) {
    glGenBuffers(1, &m_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCorners), kCorners.data(), GL_STATIC_DRAW);
  }
  if (m_row == 0) {
    glGenTextures(1, &m_row);
    glBindTexture(GL_TEXTURE_2D, m_row);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kRowTexels, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    m_rowRevision = 0;
  }
  saved.keepAttributes(std::array{m_corner});
  glViewport(draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height);
  glUseProgram(m_program.id());
  glBindTexture(GL_TEXTURE_2D, m_row);
  glUniform2f(m_size, static_cast<float>(draw.rect.width), static_cast<float>(draw.rect.height));
  glUniform1f(m_opacity, std::isfinite(draw.node.opacity) ? std::clamp(draw.node.opacity, 0.0F, 1.0F) : 0.0F);
  glUniform1i(m_rowSampler, 0);
  return made;
}

void AudioVisualQuad::writeRow(std::span<const float> values, std::uint64_t revision) {
  if (revision == m_rowRevision) {
    return;
  }
  std::array<std::uint8_t, static_cast<std::size_t>(kRowTexels) * 4> texels{};
  for (std::size_t i = 0; i < std::min(values.size(), static_cast<std::size_t>(kRowTexels)); ++i) {
    const float value = std::isnan(values[i]) ? 0.0F : std::clamp(values[i], 0.0F, 1.0F);
    texels[i * 4] = static_cast<std::uint8_t>(std::lround(value * 255));
    texels[(i * 4) + 3] = 255;
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kRowTexels, 1, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
  m_rowRevision = revision;
}

void AudioVisualQuad::finish() const {
  const auto corner = static_cast<GLuint>(m_corner);
  glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
  glVertexAttribPointer(corner, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(corner);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

GLint AudioVisualQuad::uniform(const char* name) const { return glGetUniformLocation(m_program.id(), name); }

void AudioVisualQuad::release() {
  m_program.destroy();
  if (m_buffer != 0) {
    glDeleteBuffers(1, &m_buffer);
    m_buffer = 0;
  }
  if (m_row != 0) {
    glDeleteTextures(1, &m_row);
    m_row = 0;
  }
  m_rowRevision = 0;
}

void AudioVisualQuad::abandon() noexcept {
  m_program.abandon();
  m_buffer = 0;
  m_row = 0;
  m_rowRevision = 0;
}

void AudioVisualQuad::setColor(GLint location, const Color& color) { glUniform3f(location, color.r, color.g, color.b); }

const Color& AudioVisualQuad::darker(const Color& a, const Color& b) {
  const auto luma = [](const Color& c) { return (0.2126F * c.r) + (0.7152F * c.g) + (0.0722F * c.b); };
  return luma(b) < luma(a) ? b : a;
}
