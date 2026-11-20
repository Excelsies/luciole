#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// An OpenGL ES 2 context created inside the test process with no display server, drawing into an offscreen
// framebuffer: a color texture and, when asked, a depth renderbuffer. Tests that cannot get one exit with
// kSkipExitCode and print why. captureGlState reads back the state a draw must leave as it found it.
namespace test {
  inline constexpr int kSkipExitCode = 77;

  // RGBA8 pixels, top row first.
  struct GlPixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    std::array<std::uint8_t, 4> at(int x, int y) const {
      const auto offset =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
      return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
    }
  };

  // Vertex attribute array state of one index.
  struct GlAttributeState {
    GLint enabled = 0;
    GLint size = 0;
    GLint type = 0;
    GLint normalized = 0;
    GLint stride = 0;
    GLint buffer = 0;
    void* pointer = nullptr;
  };

  // The shared state a draw could leave changed for whoever draws next.
  struct GlState {
    GLint program = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = 0;
    GLint texture = 0; // 2D texture bound to the active unit
    GLboolean blend = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean depthTest = GL_FALSE;
    std::array<GLint, 4> blendFunc{};
    std::array<GLint, 4> scissorBox{};
    std::array<GLint, 4> viewport{};
    std::vector<GlAttributeState> attributes; // every index up to GL_MAX_VERTEX_ATTRIBS, at most 64
  };

  inline GlState captureGlState() {
    GlState s;
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture);
    s.blend = glIsEnabled(GL_BLEND);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.depthTest = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_BLEND_SRC_RGB, s.blendFunc.data());
    glGetIntegerv(GL_BLEND_DST_RGB, &s.blendFunc[1]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendFunc[2]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendFunc[3]);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissorBox.data());
    glGetIntegerv(GL_VIEWPORT, s.viewport.data());
    GLint count = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &count);
    s.attributes.resize(static_cast<std::size_t>(std::clamp(count, 0, 64)));
    for (std::size_t i = 0; i < s.attributes.size(); ++i) {
      auto& a = s.attributes[i];
      const auto index = static_cast<GLuint>(i);
      glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
      glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
      glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
      glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
      glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
      glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
      glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
    }
    return s;
  }

  class GlTestContext {
  public:
    GlTestContext(const GlTestContext&) = delete;
    GlTestContext& operator=(const GlTestContext&) = delete;

    ~GlTestContext() {
      if (m_display == EGL_NO_DISPLAY) {
        return;
      }
      if (m_context != EGL_NO_CONTEXT && eglMakeCurrent(m_display, m_surface, m_surface, m_context) == EGL_TRUE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &m_framebuffer);
        glDeleteTextures(1, &m_color);
        glDeleteRenderbuffers(1, &m_depth);
      }
      eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (m_context != EGL_NO_CONTEXT) {
        eglDestroyContext(m_display, m_context);
      }
      if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_display, m_surface);
      }
      eglTerminate(m_display);
      eglReleaseThread();
    }

    // The context, current on this thread with its framebuffer bound and the viewport set; empty with `reason`
    // filled when the machine offers no offscreen OpenGL ES 2 context.
    static std::unique_ptr<GlTestContext> create(int width, int height, bool depth, std::string& reason) {
      if (width <= 0 || height <= 0) {
        reason = "the framebuffer size must be positive";
        return nullptr;
      }
      std::unique_ptr<GlTestContext> context(new GlTestContext(width, height));
      if (!context->openDisplay(reason)
          || !context->createContext(reason)
          || !context->createFramebuffer(depth, reason)) {
        return nullptr;
      }
      return context;
    }

    int width() const noexcept { return m_width; }
    int height() const noexcept { return m_height; }

    void clear(float r, float g, float b, float a) const {
      glClearColor(r, g, b, a);
      glClear(GL_COLOR_BUFFER_BIT | (m_depth != 0 ? GL_DEPTH_BUFFER_BIT : 0));
    }

    GlPixels readPixels() const {
      GlPixels pixels{.width = m_width, .height = m_height, .rgba = {}};
      const auto row = static_cast<std::size_t>(m_width) * 4;
      std::vector<std::uint8_t> bottomUp(row * static_cast<std::size_t>(m_height));
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
      pixels.rgba.resize(bottomUp.size());
      for (std::size_t y = 0; y < static_cast<std::size_t>(m_height); ++y) {
        std::memcpy(
            pixels.rgba.data() + y * row, bottomUp.data() + (static_cast<std::size_t>(m_height) - 1 - y) * row, row
        );
      }
      return pixels;
    }

  private:
    GlTestContext(int width, int height) : m_width(width), m_height(height) {}

    static bool hasExtension(const char* list, std::string_view name) {
      if (list == nullptr) {
        return false;
      }
      const std::string_view all(list);
      for (std::size_t start = 0; start < all.size();) {
        const auto end = std::min(all.find(' ', start), all.size());
        if (all.substr(start, end - start) == name) {
          return true;
        }
        start = end + 1;
      }
      return false;
    }

    // A surfaceless display, else the first EGL device; never a window system.
    bool openDisplay(std::string& reason) {
      const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
      const auto getPlatformDisplay =
          reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
      if (getPlatformDisplay == nullptr || !hasExtension(client, "EGL_EXT_platform_base")) {
        reason = "EGL has no platform display support";
        return false;
      }
      if (hasExtension(client, "EGL_MESA_platform_surfaceless")) {
        m_display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
      }
      if (m_display == EGL_NO_DISPLAY && hasExtension(client, "EGL_EXT_platform_device")) {
        const auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        EGLDeviceEXT device = nullptr;
        EGLint count = 0;
        if (queryDevices != nullptr && queryDevices(1, &device, &count) == EGL_TRUE && count > 0) {
          m_display = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, device, nullptr);
        }
      }
      if (m_display == EGL_NO_DISPLAY) {
        reason = "EGL offers neither a surfaceless platform nor a device";
        return false;
      }
      if (eglInitialize(m_display, nullptr, nullptr) != EGL_TRUE) {
        m_display = EGL_NO_DISPLAY;
        reason = "eglInitialize failed on the offscreen display";
        return false;
      }
      return true;
    }

    bool createContext(std::string& reason) {
      if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        reason = "EGL cannot bind OpenGL ES";
        return false;
      }
      const std::array<EGLint, 13> configAttributes{
          EGL_SURFACE_TYPE,
          EGL_PBUFFER_BIT,
          EGL_RENDERABLE_TYPE,
          EGL_OPENGL_ES2_BIT,
          EGL_RED_SIZE,
          8,
          EGL_GREEN_SIZE,
          8,
          EGL_BLUE_SIZE,
          8,
          EGL_ALPHA_SIZE,
          8,
          EGL_NONE,
      };
      EGLConfig config = nullptr;
      EGLint configs = 0;
      if (eglChooseConfig(m_display, configAttributes.data(), &config, 1, &configs) != EGL_TRUE || configs < 1) {
        reason = "no RGBA8 OpenGL ES 2 config on the offscreen display";
        return false;
      }
      const std::array<EGLint, 3> contextAttributes{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
      m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, contextAttributes.data());
      if (m_context == EGL_NO_CONTEXT) {
        reason = "eglCreateContext failed for OpenGL ES 2";
        return false;
      }
      if (!hasExtension(eglQueryString(m_display, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context")) {
        const std::array<EGLint, 5> pbufferAttributes{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        m_surface = eglCreatePbufferSurface(m_display, config, pbufferAttributes.data());
        if (m_surface == EGL_NO_SURFACE) {
          reason = "the display has neither surfaceless contexts nor pbuffers";
          return false;
        }
      }
      if (eglMakeCurrent(m_display, m_surface, m_surface, m_context) != EGL_TRUE) {
        reason = "eglMakeCurrent failed for the offscreen context";
        return false;
      }
      return true;
    }

    bool createFramebuffer(bool depth, std::string& reason) {
      glGenFramebuffers(1, &m_framebuffer);
      glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
      glGenTextures(1, &m_color);
      glBindTexture(GL_TEXTURE_2D, m_color);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glBindTexture(GL_TEXTURE_2D, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color, 0);
      if (depth) {
        glGenRenderbuffers(1, &m_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, m_width, m_height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth);
      }
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        reason = "the offscreen framebuffer is incomplete";
        return false;
      }
      glViewport(0, 0, m_width, m_height);
      if (glGetError() != GL_NO_ERROR) {
        reason = "OpenGL ES reported an error while creating the framebuffer";
        return false;
      }
      return true;
    }

    int m_width = 0;
    int m_height = 0;
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
    GLuint m_framebuffer = 0;
    GLuint m_color = 0;
    GLuint m_depth = 0;
  };
} // namespace test
