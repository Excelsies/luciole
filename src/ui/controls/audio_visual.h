#pragma once

#include "render/core/color.h"
#include "render/scene/custom_draw_node.h"
#include "workbench/audio_frame.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>

// The colors a visual draws with, taken by the caller from palette roles. A visual maps its color ramp onto these
// and uses no other color.
struct AudioVisualPalette {
  std::array<Color, 3> accents; // the theme's Primary, Secondary and Tertiary
  Color background;             // what the rectangle shows before the visual draws
  Color text;                   // the text color on that background
};

// A rectangle of the framebuffer in buffer pixels, in GL window coordinates: x from the left edge and y from the
// bottom edge, as glViewport takes it.
struct AudioVisualRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct AudioVisualDraw {
  CustomDrawContext node;     // what the hosting node's CustomDrawNode::draw received
  AudioVisualRect rect;       // the node's box: where the visual draws
  float scale = 1;            // buffer pixels per logical pixel
  AudioVisualPalette palette; // already resolved for this frame
};

// One sound visual. advance() moves the picture's state from the audio and makes no GL call; draw(), release()
// and abandon() run on the thread whose GL context is current.
//
// On entry to draw: the host's framebuffer is bound; blending is on with premultiplied alpha (GL_ONE,
// GL_ONE_MINUS_SRC_ALPHA for color and alpha); depth, stencil and face culling are off; the scissor test may be on,
// clipping to an ancestor's clip, which may be larger than draw.rect; draw.rect shows draw.palette.background. The
// viewport, the program, the buffer bindings, the vertex attribute arrays, the active texture unit and the bound
// textures are unknown.
//
// A visual changes only the viewport, the program, the array and element array buffer bindings, the vertex
// attribute arrays at its program's locations, the active texture unit and the texture bound to unit 0, and puts
// every one of them back before draw returns: an AudioVisualGlState around the GL work does exactly that. It never
// changes blending, the blend color, the scissor, depth, the depth mask, stencil, culling, the color mask, the clear
// color, the line width, the framebuffer binding or the pixel store state; an RGBA8 upload needs none of them, and a
// line wider than one pixel is drawn as triangles. Its output is premultiplied, multiplied by draw.node.opacity, and
// stays inside draw.rect by its own geometry or viewport, never by the scissor; light added to what is below is
// written with alpha 0.
class AudioVisual {
public:
  AudioVisual() = default;
  AudioVisual(const AudioVisual&) = delete;
  AudioVisual& operator=(const AudioVisual&) = delete;
  virtual ~AudioVisual() = default;

  // Forgets everything heard, for a restart or another file: the next picture is a new visual's.
  virtual void reset() = 0;
  // Takes the audio of one drawn frame, with bounded work whatever the frame holds. The host advances only in a
  // frame it draws, so at most one call comes between two draws, and a history texture takes one row per draw.
  virtual void advance(const luciole::workbench::AudioFrame& frame) = 0;
  // Draws the state after the last advance, creating the GL objects on first use. Drawing again without an advance
  // gives the same picture and uploads nothing. A rectangle without area draws nothing and makes no GL call.
  // Throws std::runtime_error when the program does not compile.
  virtual void draw(const AudioVisualDraw& draw) = 0;
  // Frees every GL object; the next draw makes them again from the state kept in memory. The owner calls it before
  // destroying a visual that has drawn.
  virtual void release() = 0;
  // Forgets every GL object without freeing it, after the context was lost.
  virtual void abandon() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<AudioVisual> makeCosmicPeanutVisual();
[[nodiscard]] std::unique_ptr<AudioVisual> makePulsarVisual();
[[nodiscard]] std::unique_ptr<AudioVisual> makeBarsVisual();
[[nodiscard]] std::unique_ptr<AudioVisual> makeOrbitVisual();
[[nodiscard]] std::unique_ptr<AudioVisual> makeHaloVisual();
[[nodiscard]] std::unique_ptr<AudioVisual> makeRadarVisual();
[[nodiscard]] std::unique_ptr<AudioVisual> makeEclipseVisual();
[[nodiscard]] std::unique_ptr<AudioVisual> makeDarkSunVisual();
[[nodiscard]] std::unique_ptr<AudioVisual> makeStargateVisual();

// Saves the GL state a visual may change and puts it back when it goes out of scope: the viewport, the program, the
// array and element array buffer bindings, the active texture unit and the texture bound to unit 0, and the vertex
// attribute arrays named by keepAttributes(). Constructed before the visual creates or binds anything; leaves
// texture unit 0 active.
class AudioVisualGlState {
public:
  static constexpr std::size_t kMaxAttributes = 4;

  AudioVisualGlState() {
    glGetIntegerv(GL_VIEWPORT, m_viewport.data());
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_arrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &m_elementBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texture);
  }

  // Saves the vertex attribute arrays at `locations`, once the program that names them exists and before they
  // change. Negative locations and locations already kept are skipped, so each comes back as it was at its first
  // keep. Throws std::length_error past kMaxAttributes distinct locations.
  void keepAttributes(std::span<const GLint> locations) {
    for (const GLint location : locations) {
      if (location < 0 || isKept(static_cast<GLuint>(location))) {
        continue;
      }
      if (m_count == kMaxAttributes) {
        throw std::length_error("a sound visual uses more vertex attributes than its GL state keeps");
      }
      auto& a = m_attributes[m_count++];
      a.location = static_cast<GLuint>(location);
      glGetVertexAttribiv(a.location, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
      glGetVertexAttribiv(a.location, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
      glGetVertexAttribiv(a.location, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
      glGetVertexAttribiv(a.location, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
      glGetVertexAttribiv(a.location, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
      glGetVertexAttribiv(a.location, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
      glGetVertexAttribPointerv(a.location, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
    }
  }

  ~AudioVisualGlState() {
    for (const auto& a : std::span(m_attributes).first(m_count)) {
      glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a.buffer));
      glVertexAttribPointer(
          a.location, a.size, static_cast<GLenum>(a.type), static_cast<GLboolean>(a.normalized), a.stride, a.pointer
      );
      if (a.enabled != 0) {
        glEnableVertexAttribArray(a.location);
      } else {
        glDisableVertexAttribArray(a.location);
      }
    }
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(m_arrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(m_elementBuffer));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_texture));
    glActiveTexture(static_cast<GLenum>(m_activeTexture));
    glUseProgram(static_cast<GLuint>(m_program));
    glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
  }

  AudioVisualGlState(const AudioVisualGlState&) = delete;
  AudioVisualGlState& operator=(const AudioVisualGlState&) = delete;

private:
  struct Attribute {
    GLuint location = 0;
    GLint enabled = 0;
    GLint size = 4;
    GLint type = GL_FLOAT;
    GLint normalized = 0;
    GLint stride = 0;
    GLint buffer = 0;
    void* pointer = nullptr;
  };

  [[nodiscard]] bool isKept(GLuint location) const {
    return std::ranges::any_of(std::span(m_attributes).first(m_count), [location](const Attribute& a) {
      return a.location == location;
    });
  }

  std::array<GLint, 4> m_viewport{};
  GLint m_program = 0;
  GLint m_arrayBuffer = 0;
  GLint m_elementBuffer = 0;
  GLint m_activeTexture = GL_TEXTURE0;
  GLint m_texture = 0;
  std::array<Attribute, kMaxAttributes> m_attributes{};
  std::size_t m_count = 0;
};
