#include "ui/controls/mesh_program.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
  using luciole::MeshGeometry;
  using luciole::MeshVertex;

  // Share of the key light's color a face turned away from both lights keeps, and the fill light's strength.
  constexpr float kAmbient = 0.22F;
  constexpr float kFillStrength = 0.6F;

  constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec3 a_position;
attribute vec3 a_normal;
uniform mat4 u_model_view;
uniform mat4 u_projection;
varying vec3 v_normal;
varying vec3 v_view;

void main() {
    vec4 view = u_model_view * vec4(a_position, 1.0);
    v_view = view.xyz;
    v_normal = (u_model_view * vec4(a_normal, 0.0)).xyz;
    gl_Position = u_projection * view;
}
)";

  // v_view runs from the eye to the surface. A zero normal, from a degenerate triangle, faces the eye.
  constexpr char kFragmentShader[] = R"(
precision mediump float;

uniform vec3 u_key;
uniform vec3 u_fill;
varying vec3 v_normal;
varying vec3 v_view;

const vec3 KEY_DIRECTION = vec3({:.6f}, {:.6f}, {:.6f});
const vec3 FILL_DIRECTION = vec3({:.6f}, {:.6f}, {:.6f});
const float AMBIENT = {:.6f};
const float FILL_STRENGTH = {:.6f};

void main() {{
    vec3 n = v_normal;
    float size = length(n);
    n = size > 0.0001 ? n / size : normalize(-v_view);
    if (dot(n, v_view) > 0.0) {{
        n = -n;
    }}
    float key = max(dot(n, KEY_DIRECTION), 0.0);
    float fill = max(dot(n, FILL_DIRECTION), 0.0);
    vec3 color = u_key * (AMBIENT + (1.0 - AMBIENT) * key) + u_fill * (FILL_STRENGTH * fill);
    gl_FragColor = vec4(min(color, vec3(1.0)), 1.0);
}}
)";

  std::array<float, 3> unit(const std::array<float, 3>& v) {
    const float length = std::hypot(v[0], v[1], v[2]);
    return {v[0] / length, v[1] / length, v[2] / length};
  }

  bool hasExtension(std::string_view name) {
    const auto* list = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (list == nullptr) {
      return false;
    }
    const std::string_view all(list);
    for (std::size_t start = 0; start < all.size();) {
      const std::size_t end = std::min(all.find(' ', start), all.size());
      if (all.substr(start, end - start) == name) {
        return true;
      }
      start = end + 1;
    }
    return false;
  }

  // Program, buffers, depth and blend state and the two attribute arrays a draw changes, as they were before it.
  struct SavedDraw {
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
    GLint elementBuffer = 0;
    GLint depthFunc = GL_LESS;
    GLboolean depthTest = GL_FALSE;
    GLboolean depthMask = GL_TRUE;
    GLboolean blend = GL_FALSE;
    GLboolean cull = GL_FALSE;
    std::array<Attribute, 2> attributes{};

    void save(const std::array<GLuint, 2>& locations) {
      glGetIntegerv(GL_CURRENT_PROGRAM, &program);
      glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
      glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBuffer);
      glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
      glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
      depthTest = glIsEnabled(GL_DEPTH_TEST);
      blend = glIsEnabled(GL_BLEND);
      cull = glIsEnabled(GL_CULL_FACE);
      for (std::size_t i = 0; i < locations.size(); ++i) {
        auto& a = attributes.at(i);
        glGetVertexAttribiv(locations.at(i), GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
        glGetVertexAttribiv(locations.at(i), GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
        glGetVertexAttribiv(locations.at(i), GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
        glGetVertexAttribiv(locations.at(i), GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
        glGetVertexAttribiv(locations.at(i), GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
        glGetVertexAttribiv(locations.at(i), GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
        glGetVertexAttribPointerv(locations.at(i), GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
      }
    }

    static void setEnabled(GLenum capability, GLboolean enabled) {
      if (enabled != GL_FALSE) {
        glEnable(capability);
      } else {
        glDisable(capability);
      }
    }

    void restore(const std::array<GLuint, 2>& locations) const {
      for (std::size_t i = 0; i < locations.size(); ++i) {
        const auto& a = attributes.at(i);
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a.buffer));
        glVertexAttribPointer(
            locations.at(i), a.size, static_cast<GLenum>(a.type), static_cast<GLboolean>(a.normalized), a.stride,
            a.pointer
        );
        if (a.enabled != 0) {
          glEnableVertexAttribArray(locations.at(i));
        } else {
          glDisableVertexAttribArray(locations.at(i));
        }
      }
      glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(elementBuffer));
      glDepthFunc(static_cast<GLenum>(depthFunc));
      glDepthMask(depthMask);
      setEnabled(GL_DEPTH_TEST, depthTest);
      setEnabled(GL_BLEND, blend);
      setEnabled(GL_CULL_FACE, cull);
      glUseProgram(static_cast<GLuint>(program));
    }
  };

  // The framebuffer, viewport, scissor, clear values and bindings an offscreen render changes.
  struct SavedTarget {
    GLint framebuffer = 0;
    GLint texture = 0;
    GLint renderbuffer = 0;
    std::array<GLint, 4> viewport{};
    std::array<GLfloat, 4> clearColor{};
    GLfloat clearDepth = 1;
    GLboolean scissor = GL_FALSE;

    void save() {
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
      glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
      glGetIntegerv(GL_VIEWPORT, viewport.data());
      glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor.data());
      glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);
      scissor = glIsEnabled(GL_SCISSOR_TEST);
    }

    void restore() const {
      glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
      glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
      glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
      glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
      glClearDepthf(clearDepth);
      SavedDraw::setEnabled(GL_SCISSOR_TEST, scissor);
    }
  };

  std::uint64_t nextSerial() {
    static std::atomic<std::uint64_t> serial{0};
    return ++serial;
  }
} // namespace

std::vector<MeshShortPart> splitShortIndices(const MeshGeometry& geometry, std::size_t maxVertices) {
  const std::size_t cap = std::clamp<std::size_t>(maxVertices, 3, 65536);
  constexpr std::uint32_t kUnmapped = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> local(geometry.vertices.size(), kUnmapped);
  std::vector<MeshShortPart> parts;
  std::vector<std::uint32_t> used; // vertices mapped in the current part
  const auto finish = [&] {
    for (const std::uint32_t vertex : used) {
      local[vertex] = kUnmapped;
    }
    used.clear();
  };
  for (std::size_t first = 0; first + 3 <= geometry.indices.size(); first += 3) {
    const auto triangle = std::span(geometry.indices).subspan(first, 3);
    const auto added = static_cast<std::size_t>(std::ranges::count_if(triangle, [&](std::uint32_t v) {
      return local.at(v) == kUnmapped;
    }));
    if (parts.empty() || parts.back().vertices.size() + added > cap) {
      finish();
      parts.emplace_back();
    }
    auto& part = parts.back();
    for (const std::uint32_t vertex : triangle) {
      if (local[vertex] == kUnmapped) {
        local[vertex] = static_cast<std::uint32_t>(part.vertices.size());
        part.vertices.push_back(geometry.vertices[vertex]);
        used.push_back(vertex);
      }
      part.indices.push_back(static_cast<std::uint16_t>(local[vertex]));
    }
  }
  return parts;
}

std::size_t MeshGpu::bytes() const noexcept {
  const auto pixels = static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height);
  return m_bufferBytes + (m_image != 0 ? pixels * 4 : 0) + (m_depth != 0 ? pixels * 2 : 0);
}

void MeshGpu::release() {
  for (const Part& part : m_parts) {
    const std::array<GLuint, 2> buffers{part.vertices, part.indices};
    glDeleteBuffers(2, buffers.data());
  }
  if (m_image != 0) {
    glDeleteTextures(1, &m_image);
  }
  if (m_depth != 0) {
    glDeleteRenderbuffers(1, &m_depth);
  }
  forget();
}

void MeshGpu::forget() noexcept {
  m_parts.clear();
  m_bufferBytes = 0;
  m_revision = 0;
  m_image = 0;
  m_depth = 0;
  m_width = 0;
  m_height = 0;
  m_drawn.reset();
  m_drawnRevision = 0;
}

MeshProgram::MeshProgram() : m_serial(nextSerial()) {}

void MeshProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }
  const auto key = unit(kMeshKeyLight);
  const auto fill = unit(kMeshFillLight);
  const std::string fragment =
      std::format(kFragmentShader, key[0], key[1], key[2], fill[0], fill[1], fill[2], kAmbient, kFillStrength);
  m_program.create(kVertexShader, fragment.c_str());
  const GLuint id = m_program.id();
  m_position = glGetAttribLocation(id, "a_position");
  m_normal = glGetAttribLocation(id, "a_normal");
  m_modelView = glGetUniformLocation(id, "u_model_view");
  m_projection = glGetUniformLocation(id, "u_projection");
  m_key = glGetUniformLocation(id, "u_key");
  m_fill = glGetUniformLocation(id, "u_fill");
  if (m_position < 0 || m_normal < 0 || m_modelView < 0 || m_projection < 0 || m_key < 0 || m_fill < 0) {
    destroy();
    throw std::runtime_error("failed to query mesh shader locations");
  }
  m_uintIndices = hasExtension("GL_OES_element_index_uint");
  GLint textureSide = 0;
  GLint renderbufferSide = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &textureSide);
  glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &renderbufferSide);
  m_maxSide = std::min({textureSide, renderbufferSide, kMaxImageSide});
}

void MeshProgram::destroy() {
  for (const auto& gpu : m_gpus) {
    gpu->release();
  }
  m_program.destroy();
  m_position = m_normal = m_modelView = m_projection = m_key = m_fill = -1;
}

void MeshProgram::abandon() noexcept {
  for (const auto& gpu : m_gpus) {
    gpu->forget();
  }
  m_program.abandon();
}

std::shared_ptr<MeshGpu> MeshProgram::createGpu() {
  auto gpu = std::make_shared<MeshGpu>();
  gpu->m_program = m_serial;
  m_gpus.push_back(gpu);
  return gpu;
}

void MeshProgram::releaseUnused() {
  std::erase_if(m_gpus, [](const std::shared_ptr<MeshGpu>& gpu) {
    if (gpu.use_count() > 1) {
      return false;
    }
    gpu->release();
    return true;
  });
}

void MeshProgram::upload(MeshGpu& gpu, const MeshGeometry& geometry) {
  for (const auto& part : gpu.m_parts) {
    const std::array<GLuint, 2> buffers{part.vertices, part.indices};
    glDeleteBuffers(2, buffers.data());
  }
  gpu.m_parts.clear();
  gpu.m_bufferBytes = 0;
  const auto add = [&gpu](std::span<const MeshVertex> vertices, const void* indices, std::size_t count, bool wide) {
    MeshGpu::Part part{
        .count = static_cast<GLsizei>(count), .type = wide ? GLenum{GL_UNSIGNED_INT} : GLenum{GL_UNSIGNED_SHORT}
    };
    const std::size_t indexBytes = count * (wide ? 4 : 2);
    glGenBuffers(1, &part.vertices);
    glGenBuffers(1, &part.indices);
    glBindBuffer(GL_ARRAY_BUFFER, part.vertices);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size_bytes()), vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part.indices);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexBytes), indices, GL_STATIC_DRAW);
    gpu.m_bufferBytes += vertices.size_bytes() + indexBytes;
    gpu.m_parts.push_back(part);
  };
  const bool countFits = geometry.indices.size() <= std::size_t{std::numeric_limits<GLsizei>::max()};
  if (m_uintIndices && countFits && !geometry.indices.empty()) {
    add(geometry.vertices, geometry.indices.data(), geometry.indices.size(), true);
  } else if (!m_uintIndices) {
    for (const auto& part : splitShortIndices(geometry)) {
      add(part.vertices, part.indices.data(), part.indices.size(), false);
    }
  }
  ++gpu.m_uploads;
}

bool MeshProgram::prepareImage(MeshGpu& gpu, int width, int height) {
  if (gpu.m_image != 0 && gpu.m_depth != 0 && gpu.m_width == width && gpu.m_height == height) {
    return true;
  }
  glDeleteTextures(1, &gpu.m_image);
  glDeleteRenderbuffers(1, &gpu.m_depth);
  gpu.m_image = 0;
  gpu.m_depth = 0;
  gpu.m_drawn.reset();
  glGenTextures(1, &gpu.m_image);
  glBindTexture(GL_TEXTURE_2D, gpu.m_image);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenRenderbuffers(1, &gpu.m_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, gpu.m_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
  gpu.m_width = width;
  gpu.m_height = height;
  return gpu.m_image != 0 && gpu.m_depth != 0;
}

// Flips y, so the image's first GL row is its top row.
void MeshProgram::drawParts(const MeshGpu& gpu, const MeshLook& look) {
  MeshMatrix projection = look.camera.projection;
  for (const std::size_t row : {1, 5, 9, 13}) {
    projection.at(row) = -projection.at(row);
  }
  glUseProgram(m_program.id());
  glUniformMatrix4fv(m_modelView, 1, GL_FALSE, look.camera.modelView.data());
  glUniformMatrix4fv(m_projection, 1, GL_FALSE, projection.data());
  glUniform3f(m_key, look.keyLight.r, look.keyLight.g, look.keyLight.b);
  glUniform3f(m_fill, look.fillLight.r, look.fillLight.g, look.fillLight.b);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  const auto position = static_cast<GLuint>(m_position);
  const auto normal = static_cast<GLuint>(m_normal);
  glEnableVertexAttribArray(position);
  glEnableVertexAttribArray(normal);
  for (const auto& part : gpu.m_parts) {
    glBindBuffer(GL_ARRAY_BUFFER, part.vertices);
    glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), nullptr);
    glVertexAttribPointer(
        normal, 3, GL_BYTE, GL_TRUE, sizeof(MeshVertex), reinterpret_cast<const void*>(offsetof(MeshVertex, normal))
    );
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part.indices);
    glDrawElements(GL_TRIANGLES, part.count, part.type, nullptr);
  }
}

// Relies on nothing but a current context. Changes the program, buffer bindings, depth, blend and cull state, the
// attribute arrays at this program's locations, the framebuffer binding, viewport, scissor test, clear values and
// the texture and renderbuffer bindings, and restores all of them. The framebuffer object lives only during the
// render, since framebuffers are not shared between contexts.
bool MeshProgram::render(
    MeshGpu& gpu, const MeshGeometry& geometry, std::uint64_t revision, const MeshLook& look, int width, int height
) {
  releaseUnused();
  if (!m_program.isValid() || gpu.m_program != m_serial || width <= 0 || height <= 0) {
    return false;
  }
  width = std::min(width, m_maxSide);
  height = std::min(height, m_maxSide);
  if (gpu.m_image != 0
      && gpu.m_width == width
      && gpu.m_height == height
      && gpu.m_drawn == look
      && gpu.m_drawnRevision == revision
      && gpu.m_revision == revision) {
    return true;
  }
  const std::array locations{static_cast<GLuint>(m_position), static_cast<GLuint>(m_normal)};
  SavedTarget target;
  target.save();
  SavedDraw saved;
  saved.save(locations);
  bool drawn = prepareImage(gpu, width, height);
  GLuint framebuffer = 0;
  if (drawn) {
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu.m_image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gpu.m_depth);
    drawn = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  }
  if (drawn) {
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (gpu.m_revision != revision) {
      upload(gpu, geometry);
      gpu.m_revision = revision;
    }
    drawParts(gpu, look);
    gpu.m_drawn = look;
    gpu.m_drawnRevision = revision;
    ++gpu.m_renders;
  }
  saved.restore(locations);
  target.restore();
  glDeleteFramebuffers(1, &framebuffer);
  if (!drawn) {
    glDeleteTextures(1, &gpu.m_image);
    glDeleteRenderbuffers(1, &gpu.m_depth);
    gpu.m_image = 0;
    gpu.m_depth = 0;
  }
  return drawn;
}
