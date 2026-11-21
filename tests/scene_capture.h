#pragma once

#include "gl_test_context.h"
#include "render/backend/render_backend.h"
#include "render/core/renderer.h"
#include "render/text/cairo_glyph_renderer.h"
#include "render/text/cairo_text_renderer.h"

#include <memory>
#include <string>
#include <string_view>

class Node;
class TextureManager;

namespace test {
  class CaptureRenderer;

  // Lays out whole scenes with the toolkit's text shaping and draws them with the toolkit's backend, text and icon
  // renderers into an offscreen framebuffer, inside the test process and without a display server. The walk over
  // the scene follows RenderContext::renderScene, which needs a Wayland surface and so cannot run here.
  class SceneCapture {
  public:
    // The rig, its context current on this thread; empty with `reason` filled when no offscreen context exists or
    // another rig is alive. Rigs share one display that each terminates when it goes, so one exists at a time, and
    // no GlTestContext may be created or destroyed beside it.
    static std::unique_ptr<SceneCapture> create(std::string& reason);
    ~SceneCapture();

    SceneCapture(const SceneCapture&) = delete;
    SceneCapture& operator=(const SceneCapture&) = delete;

    // Arranges `root` at `width` by `height` logical pixels with text measured at `scale`, draws it into a cleared
    // buffer of round(width * scale) by round(height * scale) pixels, and returns those pixels. Empty for a size
    // that is not positive, a scale that is not positive and finite, or a buffer larger than the GPU allows.
    GlPixels capture(Node& root, int width, int height, float scale);

    // Multiplies every text size from now on, as the system font size does in the application; 1 by default.
    void setTextScale(float scale);
    float textScale() const;

    // Advance of `text` at `fontSize` as the rig draws it: the text scale applied, one line, no wrapping.
    float textWidth(std::string_view text, float fontSize, FontWeight weight = FontWeight::Normal);

    // The height of one line of text at `fontSize` as the rig measures it: the text scale applied.
    float lineHeight(float fontSize);

    // Uploads textures for the image nodes of scenes this rig draws; the caller unloads them.
    TextureManager& textureManager();

  private:
    explicit SceneCapture(std::unique_ptr<GlTestContext> gl);

    std::unique_ptr<GlTestContext> m_gl;
    std::unique_ptr<RenderBackend> m_backend;
    CairoTextRenderer m_text;
    CairoGlyphRenderer m_glyphs;
    std::unique_ptr<CaptureRenderer> m_renderer;
  };
} // namespace test
