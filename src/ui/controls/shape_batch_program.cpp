#include "ui/controls/shape_batch_program.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>

namespace {
  using luciole::workbench::ShapeVertex;

  constexpr std::array<const char*, ShapeBatchProgram::kAttributes> kAttributeNames{
      "a_position", "a_local", "a_params", "a_fill", "a_stroke", "a_hatch", "a_style",
  };
  constexpr std::array<GLint, ShapeBatchProgram::kAttributes> kAttributeSizes{2, 2, 4, 4, 4, 4, 4};
  constexpr std::array<std::size_t, ShapeBatchProgram::kAttributes> kAttributeOffsets{
      offsetof(ShapeVertex, position), offsetof(ShapeVertex, local),  offsetof(ShapeVertex, params),
      offsetof(ShapeVertex, fill),     offsetof(ShapeVertex, stroke), offsetof(ShapeVertex, hatch),
      offsetof(ShapeVertex, style),
  };

  constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec2 a_local;
attribute vec4 a_params;
attribute vec4 a_fill;
attribute vec4 a_stroke;
attribute vec4 a_hatch;
attribute vec4 a_style;
uniform vec2 u_surface_size;
uniform mat3 u_transform;
varying vec4 v_point;
varying vec4 v_params;
varying vec4 v_fill;
varying vec4 v_stroke;
varying vec4 v_hatch;
varying vec4 v_style;

void main() {
    vec2 pixel = (u_transform * vec3(a_position, 1.0)).xy / u_surface_size;
    gl_Position = vec4(pixel.x * 2.0 - 1.0, 1.0 - pixel.y * 2.0, 0.0, 1.0);
    v_point = vec4(a_position, a_local);
    v_params = a_params;
    v_fill = a_fill;
    v_stroke = a_stroke;
    v_hatch = a_hatch;
    v_style = a_style;
}
)";

  // v_point holds the part-local position (xy) and the shape-local coordinate (zw). Distances are logical px.
  constexpr char kFragmentShader[] = R"(
precision highp float;

uniform float u_opacity;
varying vec4 v_point;
varying vec4 v_params;
varying vec4 v_fill;
varying vec4 v_stroke;
varying vec4 v_hatch;
varying vec4 v_style;

const float PI = 3.14159265;
const float HATCH_PERIOD = {:.6f};
const float HATCH_WIDTH = {:.6f};
const float DASH_LENGTH = {:.6f};
const float DASH_PERIOD = {:.6f};

vec4 premultiplied(vec4 color) {{
    return vec4(color.rgb * color.a, color.a);
}}

// Signed distance to a rounded box; `along` is the length along the side the point is nearest to.
float boxDistance(vec2 local, vec3 params, out float along) {{
    vec2 q = abs(local) - params.xy + params.z;
    along = q.y > q.x ? local.x : local.y;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - params.z;
}}

// Signed distance to an annular sector whose middle points to -y; `along` runs along the nearest edge.
float sectorDistance(vec2 local, vec3 params, out float along) {{
    float rho = length(local);
    float dr = max(params.x - rho, rho - params.y);
    float arc = atan(local.x, -local.y) * rho;
    if (params.z >= PI - 0.0001) {{
        along = arc;
        return dr;
    }}
    vec2 c = vec2(sin(params.z), cos(params.z));
    vec2 p = vec2(abs(local.x), -local.y);
    float da = length(p - c * max(dot(p, c), 0.0)) * sign(c.y * p.x - c.x * p.y);
    along = dr >= da ? arc : rho;
    return max(dr, da);
}}

// Coverage at signed distance `d` from an edge, negative inside: linear over one device pixel, or a smooth step
// over [-soft, soft] for a soft edge.
float inside(float d, float aa, float soft) {{
    return soft > 0.0 ? 1.0 - smoothstep(-soft, soft, d) : clamp(0.5 - d / aa, 0.0, 1.0);
}}

// Coverage of the diagonal stripes: the distance across them to the nearest stripe's edge, faded over one device
// pixel.
float stripes(vec2 position, float aa) {{
    float t = mod(position.x + position.y - HATCH_WIDTH * 0.5, HATCH_PERIOD);
    float across = (min(t, HATCH_PERIOD - t) - HATCH_WIDTH * 0.5) * 0.70710678;
    return clamp(0.5 - across / aa, 0.0, 1.0);
}}

void main() {{
    if (v_style.x > 1.5) {{
        gl_FragColor = premultiplied(v_fill) * clamp(v_point.z, 0.0, 1.0) * u_opacity;
        return;
    }}
    float along = 0.0;
    float d = v_style.x < 0.5 ? boxDistance(v_point.zw, v_params.xyz, along)
                              : sectorDistance(v_point.zw, v_params.xyz, along);
    float aa = v_style.w;
    float soft = v_params.w;
    vec4 color = premultiplied(v_fill);
    if (abs(v_style.y - 1.0) < 0.5) {{
        vec4 stripe = premultiplied(v_hatch) * stripes(v_point.xy, aa);
        color = stripe + color * (1.0 - stripe.a);
    }}
    float width = v_style.z;
    if (width > 0.0) {{
        float band = inside(-(d + width), aa, soft);
        if (v_style.y > 1.5 && mod(along, DASH_PERIOD) >= DASH_LENGTH) {{
            band = 0.0;
        }}
        vec4 stroke = premultiplied(v_stroke) * band;
        color = stroke + color * (1.0 - stroke.a);
    }}
    gl_FragColor = color * inside(d, aa, soft) * u_opacity;
}}
)";

  // Program, array buffer binding and the attribute arrays this program uses, as they were before a draw.
  struct SavedState {
    struct Attribute {
      GLint enabled = 0;
      GLint size = 4;
      GLint type = GL_FLOAT;
      GLint normalized = 0;
      GLint stride = 0;
      GLint buffer = 0;
      void* pointer = nullptr;
    };
    GLint program = 0;
    GLint arrayBuffer = 0;
    std::array<Attribute, ShapeBatchProgram::kAttributes> attributes{};

    void save(const std::array<GLint, ShapeBatchProgram::kAttributes>& locations) {
      glGetIntegerv(GL_CURRENT_PROGRAM, &program);
      glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
      for (std::size_t i = 0; i < locations.size(); ++i) {
        const auto location = static_cast<GLuint>(locations[i]);
        auto& a = attributes[i];
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
        glGetVertexAttribPointerv(location, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
      }
    }

    void restore(const std::array<GLint, ShapeBatchProgram::kAttributes>& locations) const {
      for (std::size_t i = 0; i < locations.size(); ++i) {
        const auto location = static_cast<GLuint>(locations[i]);
        const auto& a = attributes[i];
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a.buffer));
        glVertexAttribPointer(
            location, a.size, static_cast<GLenum>(a.type), static_cast<GLboolean>(a.normalized), a.stride, a.pointer
        );
        if (a.enabled != 0) {
          glEnableVertexAttribArray(location);
        } else {
          glDisableVertexAttribArray(location);
        }
      }
      glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
      glUseProgram(static_cast<GLuint>(program));
    }
  };

  std::uint64_t nextSerial() {
    static std::atomic<std::uint64_t> serial{0};
    return ++serial;
  }
} // namespace

ShapeBatchProgram::ShapeBatchProgram() : m_serial(nextSerial()) { m_attributes.fill(-1); }

void ShapeBatchProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }
  using luciole::workbench::kDashGap;
  using luciole::workbench::kDashLength;
  using luciole::workbench::kHatchPeriod;
  using luciole::workbench::kHatchWidth;
  const std::string fragment =
      std::format(kFragmentShader, kHatchPeriod, kHatchWidth, kDashLength, kDashLength + kDashGap);
  m_program.create(kVertexShader, fragment.c_str());
  const GLuint id = m_program.id();
  for (std::size_t i = 0; i < kAttributes; ++i) {
    m_attributes[i] = glGetAttribLocation(id, kAttributeNames[i]);
  }
  m_surfaceSizeLocation = glGetUniformLocation(id, "u_surface_size");
  m_transformLocation = glGetUniformLocation(id, "u_transform");
  m_opacityLocation = glGetUniformLocation(id, "u_opacity");
  const bool attributesFound = std::ranges::all_of(m_attributes, [](GLint location) { return location >= 0; });
  if (!attributesFound || m_surfaceSizeLocation < 0 || m_transformLocation < 0 || m_opacityLocation < 0) {
    destroy();
    throw std::runtime_error("failed to query shape batch shader locations");
  }
}

void ShapeBatchProgram::destroy() {
  for (const auto& buffer : m_buffers) {
    if (buffer->m_buffer != 0) {
      glDeleteBuffers(1, &buffer->m_buffer);
    }
    buffer->m_buffer = 0;
    buffer->m_revision = 0;
  }
  m_program.destroy();
  m_attributes.fill(-1);
  m_surfaceSizeLocation = -1;
  m_transformLocation = -1;
  m_opacityLocation = -1;
}

void ShapeBatchProgram::abandon() noexcept {
  for (const auto& buffer : m_buffers) {
    buffer->m_buffer = 0;
    buffer->m_revision = 0;
  }
  m_program.abandon();
}

std::shared_ptr<ShapeBatchBuffer> ShapeBatchProgram::createBuffer() {
  auto buffer = std::make_shared<ShapeBatchBuffer>();
  buffer->m_program = m_serial;
  m_buffers.push_back(buffer);
  return buffer;
}

void ShapeBatchProgram::releaseUnused() {
  std::erase_if(m_buffers, [](const std::shared_ptr<ShapeBatchBuffer>& buffer) {
    if (buffer.use_count() > 1) {
      return false;
    }
    if (buffer->m_buffer != 0) {
      glDeleteBuffers(1, &buffer->m_buffer);
    }
    return true;
  });
}

void ShapeBatchProgram::upload(ShapeBatchBuffer& buffer, std::span<const ShapeVertex> vertices) {
  if (buffer.m_buffer == 0) {
    glGenBuffers(1, &buffer.m_buffer);
  }
  glBindBuffer(GL_ARRAY_BUFFER, buffer.m_buffer);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size_bytes()), vertices.data(), GL_DYNAMIC_DRAW);
  ++m_uploads;
}

// Relies on the state RenderContext sets before every node: premultiplied blending, the target's framebuffer and
// viewport, and the node's scissor. Changes the program, the array buffer binding and the attribute arrays at this
// program's locations, and restores all of them; it touches no texture unit, blending, scissor or depth state.
void ShapeBatchProgram::draw(
    ShapeBatchBuffer& buffer, std::span<const ShapeVertex> vertices, std::uint64_t revision, const ShapeBatchDraw& draw
) {
  releaseUnused();
  const bool surfaceValid = std::isfinite(draw.surfaceWidth)
      && std::isfinite(draw.surfaceHeight)
      && draw.surfaceWidth > 0
      && draw.surfaceHeight > 0;
  if (!m_program.isValid()
      || buffer.m_program != m_serial
      || vertices.empty()
      || !surfaceValid
      || !(draw.opacity > 0)) {
    return;
  }
  SavedState saved;
  saved.save(m_attributes);
  if (buffer.m_buffer == 0 || buffer.m_revision != revision) {
    upload(buffer, vertices);
    buffer.m_revision = revision;
  } else {
    glBindBuffer(GL_ARRAY_BUFFER, buffer.m_buffer);
  }
  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, draw.surfaceWidth, draw.surfaceHeight);
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, draw.transform.m.data());
  glUniform1f(m_opacityLocation, std::min(draw.opacity, 1.0F));
  for (std::size_t i = 0; i < kAttributes; ++i) {
    const auto location = static_cast<GLuint>(m_attributes[i]);
    glVertexAttribPointer(
        location, kAttributeSizes[i], GL_FLOAT, GL_FALSE, sizeof(ShapeVertex),
        reinterpret_cast<const void*>(kAttributeOffsets[i])
    );
    glEnableVertexAttribArray(location);
  }
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
  saved.restore(m_attributes);
}
