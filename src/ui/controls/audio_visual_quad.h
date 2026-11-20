#pragma once

#include "render/core/color.h"
#include "render/core/shader_program.h"
#include "ui/controls/audio_visual.h"

#include <GLES2/gl2.h>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

// What a sound visual drawn by one fragment program over its whole rectangle shares: the program, the rectangle's
// two triangles, and a one-row RGBA8 texture of kRowTexels values in its red channel.
//
// The fragment source given is compiled after `precision highp float;` and these declarations:
//   varying vec2 v_pixel;     // the fragment's position in buffer pixels from the rectangle's top left corner
//   uniform vec2 u_size;      // the rectangle's size in buffer pixels
//   uniform float u_opacity;  // what the output is multiplied by
//   uniform sampler2D u_row;  // the row on texture unit 0, filtered linearly and clamped at its ends
//   float hash(vec2 p);                       // a fixed value in [0, 1) for each point
//   float valueNoise(vec2 p, float period);   // smooth noise in [0, 1], repeating every `period` along x
//   vec4 over(vec4 below, vec3 color, float alpha); // premultiplied `color` at `alpha` over `below`
class AudioVisualQuad {
public:
  static constexpr int kRowTexels = 64;

  explicit AudioVisualQuad(std::string fragmentSource) : m_fragmentSource(std::move(fragmentSource)) {}

  // Makes the program, the buffer and the row on first use, keeps the program's vertex attribute in `saved`, sets
  // the viewport to the rectangle, uses the program, binds the row to unit 0 and sets the shared uniforms. True when
  // the program was just made, so the caller looks up its own uniforms again. Throws std::runtime_error when the
  // program does not compile.
  bool begin(const AudioVisualDraw& draw, AudioVisualGlState& saved);
  // Writes `values`, clamped to [0, 1], into the row when `revision` differs from the one it holds; at most
  // kRowTexels values, and 0 past the end of `values`. Call between begin() and finish().
  void writeRow(std::span<const float> values, std::uint64_t revision);
  // Draws the rectangle with one call.
  void finish() const;
  // The location of a uniform of the program made by the last begin().
  GLint uniform(const char* name) const;

  // Frees every GL object; the next begin() makes them again.
  void release();
  // Forgets every GL object without freeing it.
  void abandon() noexcept;

  // Sets a vec3 uniform of the program in use to a color's red, green and blue.
  static void setColor(GLint location, const Color& color);
  // Whichever of two colors has the lower luma; `a` when they tie.
  static const Color& darker(const Color& a, const Color& b);

private:
  std::string m_fragmentSource;
  ShaderProgram m_program;
  GLuint m_buffer = 0;
  GLuint m_row = 0;
  GLint m_corner = -1;
  GLint m_size = -1;
  GLint m_opacity = -1;
  GLint m_rowSampler = -1;
  std::uint64_t m_rowRevision = 0; // 0 when the row holds nothing
};
