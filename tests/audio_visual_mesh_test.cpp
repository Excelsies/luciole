#include "gl_test_context.h"
#include "test_check.h"
#include "ui/controls/audio_visual_mesh.h"

#include <GLES2/gl2.h>
#include <array>
#include <print>
#include <stdexcept>
#include <string>

namespace {
  constexpr char kVertexShader[] = R"(
attribute vec4 a_vertex;
uniform float u_scale;
void main() {
    gl_Position = vec4(a_vertex.xy * u_scale, 0.0, 1.0);
}
)";

  constexpr char kFragmentShader[] = R"(
precision mediump float;
void main() {
    gl_FragColor = vec4(1.0);
}
)";

  constexpr std::array<AudioVisualVertex, 3> kTriangle{{{0, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}}};

  // Whether any program or buffer name up to a bound well above what this test creates is live.
  bool anyLiveObject() {
    for (GLuint name = 1; name <= 256; ++name) {
      if (glIsProgram(name) == GL_TRUE || glIsBuffer(name) == GL_TRUE) {
        return true;
      }
    }
    return false;
  }

  bool lookupThrows(AudioVisualMesh& mesh, const char* name) {
    try {
      (void)mesh.uniform(name);
    } catch (const std::runtime_error&) {
      return true;
    }
    return false;
  }

  void aUsedUniformHasALocation() {
    AudioVisualMesh mesh;
    TEST_CHECK(!mesh.hasProgram());
    mesh.ensureProgram(kVertexShader, kFragmentShader);
    TEST_CHECK(mesh.hasProgram());
    TEST_CHECK(mesh.vertexLocation() >= 0);
    TEST_CHECK(mesh.uniform("u_scale") >= 0);
    TEST_CHECK(mesh.hasProgram());
    mesh.release();
    TEST_CHECK(!anyLiveObject());
  }

  // A failed lookup drops the program and the buffer, so the next setup builds the program again and fails again
  // instead of drawing with a location of -1.
  void aMissingUniformDropsTheProgram() {
    AudioVisualMesh mesh;
    mesh.ensureProgram(kVertexShader, kFragmentShader);
    mesh.fill(1, kTriangle);
    TEST_CHECK(mesh.holds(1));
    TEST_CHECK(lookupThrows(mesh, "u_missing"));
    TEST_CHECK(!mesh.hasProgram());
    TEST_CHECK(mesh.vertexLocation() == -1);
    TEST_CHECK(!mesh.holds(1));
    TEST_CHECK(mesh.vertexCount() == 0);
    TEST_CHECK(!anyLiveObject());

    mesh.ensureProgram(kVertexShader, kFragmentShader);
    TEST_CHECK(mesh.hasProgram());
    TEST_CHECK(lookupThrows(mesh, "u_missing"));
    TEST_CHECK(!mesh.hasProgram());
    TEST_CHECK(glGetError() == GL_NO_ERROR);
  }
} // namespace

int main() {
  std::string reason;
  const auto gl = test::GlTestContext::create(16, 16, false, reason);
  if (!gl) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  aUsedUniformHasALocation();
  aMissingUniformDropsTheProgram();
  std::println("sound visual mesh checks passed");
  return 0;
}
