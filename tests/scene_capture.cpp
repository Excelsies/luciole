#include "scene_capture.h"

#include "core/files/resource_paths.h"
#include "core/ui_phase.h"
#include "render/core/mat3.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/custom_draw_node.h"
#include "render/scene/glyph_node.h"
#include "render/scene/image_node.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "render/scene/spinner_node.h"
#include "render/scene/text_node.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

namespace test {
  // Text and icon measurement at the capture's scale, converted as RenderContext converts it for ScaledRenderer.
  class CaptureRenderer final : public Renderer {
  public:
    CaptureRenderer(CairoTextRenderer& text, CairoGlyphRenderer& glyphs, TextureManager& textures)
        : m_text(text), m_glyphs(glyphs), m_textures(textures) {}

    void setScale(float scale) noexcept { m_scale = scale; }
    // The multiplier of every text size, as RenderContext applies the system font size.
    void setTextScale(float scale) noexcept { m_textScale = scale; }
    float textScale() const noexcept { return m_textScale; }

    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines, TextAlign align,
        TextEllipsize ellipsize, bool useMarkup
    ) override {
      const auto m =
          m_text.measure(m_scale, text, fontSize * m_textScale, font, maxWidth, maxLines, align, ellipsize, useMarkup);
      return TextMetrics{
          .width = m.width,
          .left = m.left,
          .right = m.right,
          .top = m.top,
          .bottom = m.bottom,
          .inkTop = m.inkTop,
          .inkBottom = m.inkBottom,
          .inkLeft = m.inkLeft,
          .inkRight = m.inkRight,
          .lineCount = m.lineCount,
          .alignOffsetX = m.alignOffsetX
      };
    }

    TextMetrics measureFont(float fontSize, const TextFont& font) override {
      const auto m = m_text.measureFont(m_scale, fontSize * m_textScale, font);
      return TextMetrics{
          .width = m.width,
          .left = m.left,
          .right = m.right,
          .top = m.top,
          .bottom = m.bottom,
          .inkTop = m.inkTop,
          .inkBottom = m.inkBottom,
          .inkLeft = m.inkLeft,
          .inkRight = m.inkRight,
          .capHeight = m.capHeight
      };
    }

    void measureTextCursorStops(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<float>& outStops, const TextFont& font
    ) override {
      m_text.measureCursorStops(m_scale, text, fontSize * m_textScale, byteOffsets, outStops, font);
    }

    void measureTextCursorStopsWrapped(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
        std::vector<TextCursorStop>& outStops, const TextFont& font
    ) override {
      m_text.measureCursorStopsWrapped(m_scale, text, fontSize * m_textScale, byteOffsets, maxWidth, outStops, font);
    }

    void measureTextCharSpans(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<TextCharSpan>& outSpans, const TextFont& font
    ) override {
      m_text.measureCharSpans(m_scale, text, fontSize * m_textScale, byteOffsets, outSpans, font);
    }

    TextMetrics measureGlyph(char32_t codepoint, float fontSize) override {
      const auto m = m_glyphs.measureGlyph(m_scale, codepoint, fontSize);
      return TextMetrics{
          .width = m.width,
          .left = m.left,
          .right = m.right,
          .top = m.top,
          .bottom = m.bottom,
          .inkTop = m.top,
          .inkBottom = m.bottom,
          .inkLeft = m.left,
          .inkRight = m.right
      };
    }

    TextureManager& textureManager() override { return m_textures; }
    float renderScale() const noexcept override { return m_scale; }
    std::uint64_t textMetricsGeneration() const noexcept override { return m_generation; }

    void notifyFontConfigChanged() override {
      m_text.notifyFontConfigChanged();
      ++m_generation;
    }

  private:
    CairoTextRenderer& m_text;
    CairoGlyphRenderer& m_glyphs;
    TextureManager& m_textures;
    float m_scale = 1;
    float m_textScale = 1;
    std::uint64_t m_generation = 1;
  };

  namespace {
    constexpr float kPaintCullSlack = 16.0F;

    // Every rig's context terminates the display they all share, so one at a time may exist.
    bool rigAlive = false;

    // What every node of one frame draws with.
    struct Frame {
      RenderBackend& backend;
      CairoTextRenderer& text;
      CairoGlyphRenderer& glyphs;
      float scale = 1;
      float textScale = 1;
      float surfaceWidth = 0; // logical
      float surfaceHeight = 0;
      float bufferWidth = 0;
      float bufferHeight = 0;
    };

    // The clip in logical surface coordinates.
    struct Clip {
      float left = 0;
      float top = 0;
      float right = 0;
      float bottom = 0;
      bool active = false;
    };

    // The parts of a node's paint that come from its ancestors.
    struct Inherited {
      Mat3 transform = Mat3::identity();
      float opacity = 1;
      Clip clip;
      bool ignoreNodeOpacity = false;
      bool paintContained = false;
    };

    RenderScissor scissorForClip(const Frame& f, const Clip& clip) {
      const float scaleX = f.surfaceWidth > 0.0F ? f.bufferWidth / f.surfaceWidth : 1.0F;
      const float scaleY = f.surfaceHeight > 0.0F ? f.bufferHeight / f.surfaceHeight : 1.0F;
      const auto clampX = [&f](std::int32_t value) {
        return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(f.bufferWidth)));
      };
      const auto clampY = [&f](std::int32_t value) {
        return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(f.bufferHeight)));
      };
      const std::int32_t x0 = clampX(static_cast<std::int32_t>(std::floor(clip.left * scaleX)));
      const std::int32_t x1 = clampX(static_cast<std::int32_t>(std::ceil(clip.right * scaleX)));
      const std::int32_t y0 = clampY(static_cast<std::int32_t>(std::floor((f.surfaceHeight - clip.bottom) * scaleY)));
      const std::int32_t y1 = clampY(static_cast<std::int32_t>(std::ceil((f.surfaceHeight - clip.top) * scaleY)));
      return RenderScissor{
          .x = x0,
          .y = y0,
          .width = std::max(std::int32_t{0}, x1 - x0),
          .height = std::max(std::int32_t{0}, y1 - y0),
      };
    }

    Mat3 nodeLocalTransform(const Node& node) {
      const float cx = node.transformOriginX();
      const float cy = node.transformOriginY();
      return Mat3::translation(node.x() + node.translateX(), node.y() + node.translateY())
          * Mat3::translation(cx, cy)
          * Mat3::rotation(node.rotation())
          * Mat3::scale(node.scaleX(), node.scaleY())
          * Mat3::translation(-cx, -cy);
    }

    void drawText(const Frame& f, const TextNode& text, const Mat3& world, float opacity) {
      if (text.text().empty()) {
        return;
      }
      const auto draw = [&](Color color, const Mat3& transform) {
        color.a *= opacity;
        f.text.draw(
            f.scale, f.surfaceWidth, f.surfaceHeight, 0.0F, 0.0F, text.text(), text.fontSize() * f.textScale, color,
            transform, text.font(), text.maxWidth(), text.maxLines(), text.textAlign(), text.ellipsize(),
            text.useMarkup()
        );
      };
      if (text.hasShadow()) {
        draw(text.shadowColor(), world * Mat3::translation(text.shadowOffsetX(), text.shadowOffsetY()));
      }
      draw(text.color(), world);
    }

    void drawGlyph(const Frame& f, const GlyphNode& icon, const Mat3& world, float opacity) {
      if (icon.codepoint() == 0) {
        return;
      }
      const auto draw = [&](Color color, const Mat3& transform) {
        color.a *= opacity;
        f.glyphs.drawGlyph(
            f.scale, f.surfaceWidth, f.surfaceHeight, 0.0F, 0.0F, icon.codepoint(), icon.fontSize(), color, transform
        );
      };
      if (icon.hasShadow()) {
        draw(icon.shadowColor(), world * Mat3::translation(icon.shadowOffsetX(), icon.shadowOffsetY()));
      }
      draw(icon.color(), world);
    }

    void drawImage(const Frame& f, const ImageNode& image, const Mat3& world, float opacity) {
      if (image.textureId() == 0) {
        return;
      }
      f.backend.drawImage(
          RenderImageDraw{
              .texture = image.textureId(),
              .surfaceWidth = f.surfaceWidth,
              .surfaceHeight = f.surfaceHeight,
              .width = image.width(),
              .height = image.height(),
              .tint = image.tint(),
              .saturation = image.saturation(),
              .monochromeTint = image.monochromeTint(),
              .alphaMaskTint = image.alphaMaskTint(),
              .opacity = opacity,
              .radius = image.radius(),
              .borderColor = image.borderColor(),
              .borderWidth = image.borderWidth(),
              .fitMode = static_cast<RenderImageFitMode>(image.fitMode()),
              .textureWidth = static_cast<float>(image.textureWidth()),
              .textureHeight = static_cast<float>(image.textureHeight()),
              .transform = world,
              .scrim = image.scrim(),
          }
      );
    }

    // The node's own paint, before its children.
    void drawContent(const Frame& f, const Node& node, const Mat3& world, float opacity) {
      switch (node.type()) {
      case NodeType::Rect: {
        auto style = static_cast<const RectNode&>(node).style();
        style.fill.a *= opacity;
        style.border.a *= opacity;
        for (auto& stop : style.gradientStops) {
          stop.color.a *= opacity;
        }
        f.backend.drawRect(f.surfaceWidth, f.surfaceHeight, node.width(), node.height(), style, world);
        break;
      }
      case NodeType::Text:
        drawText(f, static_cast<const TextNode&>(node), world, opacity);
        break;
      case NodeType::Image:
        drawImage(f, static_cast<const ImageNode&>(node), world, opacity);
        break;
      case NodeType::Glyph:
        drawGlyph(f, static_cast<const GlyphNode&>(node), world, opacity);
        break;
      case NodeType::Spinner: {
        auto style = static_cast<const SpinnerNode&>(node).style();
        style.color.a *= opacity;
        f.backend.drawSpinner(f.surfaceWidth, f.surfaceHeight, node.width(), node.height(), style, world);
        break;
      }
      case NodeType::CustomDraw:
        static_cast<const CustomDrawNode&>(node).draw(
            f.backend,
            CustomDrawContext{
                .surfaceWidth = f.surfaceWidth,
                .surfaceHeight = f.surfaceHeight,
                .pixelScaleX = f.surfaceWidth > 0.0F ? f.bufferWidth / f.surfaceWidth : 1.0F,
                .pixelScaleY = f.surfaceHeight > 0.0F ? f.bufferHeight / f.surfaceHeight : 1.0F,
                .transform = world,
                .opacity = opacity,
                .width = node.width(),
                .height = node.height(),
            }
        );
        break;
      case NodeType::RenderProxy:
      case NodeType::Base:
        break;
      }
    }

    bool culled(const Node& node, const Clip& clip, bool paintContained, const Clip& bounds) {
      return paintContained
          && clip.active
          && node.type() != NodeType::RenderProxy
          && node.width() > 0.0F
          && node.height() > 0.0F
          && (bounds.right + kPaintCullSlack <= clip.left
              || bounds.left - kPaintCullSlack >= clip.right
              || bounds.bottom + kPaintCullSlack <= clip.top
              || bounds.top - kPaintCullSlack >= clip.bottom);
    }

    // Draws `node` and its subtree in the order, with the transforms, opacity, clipping and culling of
    // RenderContext::renderNode. Recursion is bounded by the depth of the tree, as it is there.
    void drawNode(const Frame& f, const Node& node, const Inherited& from) {
      if (!node.visible()) {
        return;
      }
      const Mat3 world = from.transform * nodeLocalTransform(node);
      const float opacity = from.ignoreNodeOpacity ? from.opacity : from.opacity * node.opacity();
      Clip bounds;
      Node::transformedBounds(&node, world, bounds.left, bounds.top, bounds.right, bounds.bottom);
      const bool paintContained = from.paintContained || node.paintContained();
      if (culled(node, from.clip, paintContained, bounds)) {
        return;
      }
      if (from.clip.active) {
        f.backend.setScissor(scissorForClip(f, from.clip));
      } else {
        f.backend.disableScissor();
      }

      if (node.type() == NodeType::RenderProxy) {
        const Node* source = static_cast<const RenderProxyNode&>(node).source();
        bool sourceContainsProxy = false;
        for (const Node* current = &node; current != nullptr; current = current->parent()) {
          sourceContainsProxy = sourceContainsProxy || current == source;
        }
        if (source != nullptr && !sourceContainsProxy) {
          const Mat3 sourceParent =
              world * Mat3::translation(-(source->x() + source->translateX()), -(source->y() + source->translateY()));
          drawNode(
              f, *source, {.transform = sourceParent, .opacity = opacity, .clip = from.clip, .ignoreNodeOpacity = true}
          );
        }
        return;
      }
      drawContent(f, node, world, opacity);

      Clip clip = from.clip;
      if (node.clipChildren()) {
        clip.left = clip.active ? std::max(clip.left, bounds.left) : bounds.left;
        clip.top = clip.active ? std::max(clip.top, bounds.top) : bounds.top;
        clip.right = clip.active ? std::min(clip.right, bounds.right) : bounds.right;
        clip.bottom = clip.active ? std::min(clip.bottom, bounds.bottom) : bounds.bottom;
        clip.active = true;
      }
      if (clip.active && (clip.right <= clip.left || clip.bottom <= clip.top)) {
        return;
      }
      std::vector<const Node*> ordered;
      ordered.reserve(node.children().size());
      for (const auto& child : node.children()) {
        ordered.push_back(child.get());
      }
      std::ranges::stable_sort(ordered, [](const Node* a, const Node* b) { return a->zIndex() < b->zIndex(); });
      for (const Node* child : ordered) {
        drawNode(f, *child, {.transform = world, .opacity = opacity, .clip = clip, .paintContained = paintContained});
      }
    }

    std::uint32_t bufferExtent(int logical, float scale) {
      return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<double>(logical) * scale)));
    }

    // The bound framebuffer's pixels, top row first.
    GlPixels readBoundPixels(int width, int height) {
      GlPixels pixels{.width = width, .height = height, .rgba = {}};
      const auto row = static_cast<std::size_t>(width) * 4;
      std::vector<std::uint8_t> bottomUp(row * static_cast<std::size_t>(height));
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
      pixels.rgba.resize(bottomUp.size());
      for (std::size_t y = 0; y < static_cast<std::size_t>(height); ++y) {
        std::memcpy(
            pixels.rgba.data() + y * row, bottomUp.data() + (static_cast<std::size_t>(height) - 1 - y) * row, row
        );
      }
      return pixels;
    }
  } // namespace

  std::unique_ptr<SceneCapture> SceneCapture::create(std::string& reason) {
    if (rigAlive) {
      reason = "another SceneCapture is alive";
      return nullptr;
    }
    auto gl = GlTestContext::create(1, 1, false, reason);
    if (!gl) {
      return nullptr;
    }
    return std::unique_ptr<SceneCapture>(new SceneCapture(std::move(gl)));
  }

  SceneCapture::SceneCapture(std::unique_ptr<GlTestContext> gl)
      : m_gl(std::move(gl)), m_backend(createDefaultRenderBackend()) {
    auto& textures = m_backend->textureManager();
    textures.probeExtensions();
    m_text.initialize(m_backend.get(), &textures);
    m_glyphs.initialize(paths::libraryAssetPath("fonts/noctalia-tabler.ttf").string(), m_backend.get(), &textures);
    m_renderer = std::make_unique<CaptureRenderer>(m_text, m_glyphs, textures);
    rigAlive = true;
  }

  // Members go in reverse order: the renderers release their textures through the backend while the context lives.
  SceneCapture::~SceneCapture() { rigAlive = false; }

  void SceneCapture::setTextScale(float scale) { m_renderer->setTextScale(scale); }

  float SceneCapture::textScale() const { return m_renderer->textScale(); }

  GlPixels SceneCapture::capture(Node& root, int width, int height, float scale) {
    if (width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0.0F) {
      return {};
    }
    const auto limit = static_cast<double>(m_backend->maxTextureSize());
    if (std::round(static_cast<double>(width) * scale) > limit
        || std::round(static_cast<double>(height) * scale) > limit) {
      return {};
    }
    const std::uint32_t bufferWidth = bufferExtent(width, scale);
    const std::uint32_t bufferHeight = bufferExtent(height, scale);

    m_renderer->setScale(scale);
    root.arrange(
        *m_renderer, {.x = 0, .y = 0, .width = static_cast<float>(width), .height = static_cast<float>(height)}
    );

    const auto framebuffer = m_backend->createFramebuffer(bufferWidth, bufferHeight);
    if (framebuffer == nullptr) {
      throw std::runtime_error(std::format("could not create a {}x{} framebuffer", bufferWidth, bufferHeight));
    }
    m_backend->bindFramebuffer(*framebuffer);
    m_backend->setViewport(bufferWidth, bufferHeight);
    m_backend->setBlendMode(RenderBlendMode::PremultipliedAlpha);
    m_backend->disableScissor();
    m_backend->clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));
    {
      UiPhaseScope render(UiPhase::Render);
      const Frame frame{
          .backend = *m_backend,
          .text = m_text,
          .glyphs = m_glyphs,
          .scale = scale,
          .textScale = m_renderer->textScale(),
          .surfaceWidth = static_cast<float>(width),
          .surfaceHeight = static_cast<float>(height),
          .bufferWidth = static_cast<float>(bufferWidth),
          .bufferHeight = static_cast<float>(bufferHeight),
      };
      drawNode(frame, root, {});
    }
    auto pixels = readBoundPixels(static_cast<int>(bufferWidth), static_cast<int>(bufferHeight));
    m_backend->disableScissor();
    m_backend->bindDefaultFramebuffer();
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
      throw std::runtime_error(std::format("OpenGL ES error 0x{:04x} while drawing the scene", error));
    }
    return pixels;
  }

  TextureManager& SceneCapture::textureManager() { return m_backend->textureManager(); }
} // namespace test
