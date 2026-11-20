#include "ui/controls/audio_visual.h"
#include "ui/controls/audio_visual_mesh.h"
#include "workbench/bar_scene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {
  using luciole::workbench::BarLayout;
  using luciole::workbench::BarScene;
  using luciole::workbench::kAudioBandCount;
  using luciole::workbench::kWavePoints;

  // Pieces in drawing order: the glow behind, the reflections, the bars, their caps, the waveform's soft stroke and
  // its line. Every vertex is (piece, bar or point, x corner or stroke side, y corner).
  enum class BarPiece : std::uint8_t { Backdrop, Reflection, Bar, Cap, WaveGlow, WaveLine };

  constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec4 a_vertex;
uniform vec2 u_size;
uniform vec3 u_columns;
uniform vec4 u_rows;
uniform vec4 u_wave_line;
uniform vec2 u_backdrop;
uniform vec4 u_levels[16];
uniform vec4 u_caps[16];
uniform vec4 u_wave[32];
uniform mediump vec3 u_accent;
uniform mediump vec3 u_text;
varying vec4 v_color;
varying vec3 v_paint;

const float WAVE_POINTS = 128.0;

float pick(vec4 v, int c) {
    if (c == 0) {
        return v.x;
    }
    if (c == 1) {
        return v.y;
    }
    if (c == 2) {
        return v.z;
    }
    return v.w;
}

float levelAt(int i) {
    int q = i / 4;
    return pick(u_levels[q], i - q * 4);
}

float capAt(int i) {
    int q = i / 4;
    return pick(u_caps[q], i - q * 4);
}

vec2 wavePoint(int i) {
    int p = int(clamp(float(i), 0.0, WAVE_POINTS - 1.0));
    int q = p / 4;
    float y = pick(u_wave[q], p - q * 4);
    return vec2(float(p) / (WAVE_POINTS - 1.0) * u_size.x, u_wave_line.x - y * u_wave_line.y);
}

float snap(float v) {
    return floor(v + 0.5);
}

// Pixels from the top left corner. u_columns: margin, slot, gap. u_rows: base, top of the color ramp, full bar,
// cap height. u_wave_line: center, amplitude, line width, glow width. u_backdrop: room under the base, brightness.
// v_paint: (ramp position or distance across a stroke, half the stroke, paint mode).
void main() {
    float piece = a_vertex.x;
    int index = int(a_vertex.y + 0.5);
    vec2 corner = a_vertex.zw;
    vec2 point = corner * u_size;
    v_color = vec4(1.0);
    v_paint = vec3(0.0);
    if (piece < 0.5) {
        float a = 0.35 * u_backdrop.y;
        vec2 origin = vec2(0.5 * u_size.x, u_size.y);
        v_color = vec4(u_accent * a, a);
        v_paint = vec3((point - origin) / length(origin), 4.0);
    } else if (piece < 3.5) {
        float left = u_columns.x + float(index) * u_columns.y + 0.5 * u_columns.z;
        point.x = snap(left + corner.x * (u_columns.y - u_columns.z));
        float level = levelAt(index);
        if (piece < 1.5) {
            float depth = min(u_backdrop.x, level * u_backdrop.x * 1.2);
            point.y = snap(u_rows.x + u_rows.w + corner.y * depth);
            v_color = vec4(0.14);
            v_paint = vec3(0.0, 0.0, 1.0);
        } else if (piece < 2.5) {
            float height = max(u_rows.w, level * u_rows.z);
            point.y = snap(u_rows.x - (1.0 - corner.y) * height);
            v_paint = vec3((u_rows.x - point.y) / (u_rows.x - u_rows.y), 0.0, 1.0);
        } else {
            float bottom = u_rows.x - capAt(index) * u_rows.z - u_rows.w;
            point.y = snap(bottom - (1.0 - corner.y) * u_rows.w);
            v_color = vec4(mix(u_accent, u_text, 0.8), 1.0);
        }
    } else {
        vec2 tangent = wavePoint(index + 1) - wavePoint(index - 1);
        vec2 normal = normalize(vec2(-tangent.y, tangent.x));
        bool glow = piece < 4.5;
        float halfWidth = 0.5 * (glow ? u_wave_line.w : u_wave_line.z) + 1.0;
        float across = (corner.x * 2.0 - 1.0) * halfWidth;
        point = wavePoint(index) + normal * across;
        if (glow) {
            v_color = vec4(u_accent * 0.25, 0.25);
            v_paint = vec3(across, halfWidth, 3.0);
        } else {
            v_color = vec4(mix(u_accent, u_text, 0.6) * 0.85, 0.85);
            v_paint = vec3(across, halfWidth - 1.0, 2.0);
        }
    }
    gl_Position = vec4(point.x / u_size.x * 2.0 - 1.0, 1.0 - point.y / u_size.y * 2.0, 0.0, 1.0);
}
)";

  // Paint modes: 0 flat, 1 the bar ramp from a quarter of the accent through the accent toward the text color,
  // 2 an antialiased line, 3 a soft stroke, 4 a radial fade.
  constexpr char kFragmentShader[] = R"(
precision mediump float;

uniform vec3 u_accent;
uniform vec3 u_text;
uniform float u_opacity;
varying vec4 v_color;
varying vec3 v_paint;

vec4 ramp(float t) {
    vec4 low = vec4(u_accent * 0.25, 0.25);
    vec4 middle = vec4(u_accent, 1.0);
    vec4 high = vec4(mix(u_accent, u_text, 0.7), 1.0);
    return t < 0.55 ? mix(low, middle, t / 0.55) : mix(middle, high, (t - 0.55) / 0.45);
}

void main() {
    vec4 color = v_color;
    if (v_paint.z > 3.5) {
        color *= clamp(1.0 - length(v_paint.xy), 0.0, 1.0);
    } else if (v_paint.z > 2.5) {
        float fade = clamp(1.0 - abs(v_paint.x) / v_paint.y, 0.0, 1.0);
        color *= fade * fade;
    } else if (v_paint.z > 1.5) {
        color *= clamp(v_paint.y + 0.5 - abs(v_paint.x), 0.0, 1.0);
    } else if (v_paint.z > 0.5) {
        color = ramp(clamp(v_paint.x, 0.0, 1.0)) * v_color.a;
    }
    gl_FragColor = color * u_opacity;
}
)";

  void addQuad(std::vector<AudioVisualVertex>& out, BarPiece piece, std::size_t index) {
    const auto p = static_cast<std::uint8_t>(piece);
    const auto i = static_cast<std::uint8_t>(index);
    for (const auto& [x, y] : {std::array<std::uint8_t, 2>{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}}) {
      out.push_back({p, i, x, y});
    }
  }

  // A strip along the waveform: one quad per segment, its sides at stroke side 0 and 1.
  void addStroke(std::vector<AudioVisualVertex>& out, BarPiece piece) {
    const auto p = static_cast<std::uint8_t>(piece);
    for (std::size_t point = 0; point + 1 < kWavePoints; ++point) {
      const auto a = static_cast<std::uint8_t>(point);
      const auto b = static_cast<std::uint8_t>(point + 1);
      for (const auto& [i, side] : {std::array<std::uint8_t, 2>{a, 0}, {a, 1}, {b, 0}, {b, 0}, {a, 1}, {b, 1}}) {
        out.push_back({p, i, side, 0});
      }
    }
  }

  std::vector<AudioVisualVertex> barVertices(std::size_t count) {
    std::vector<AudioVisualVertex> out;
    out.reserve(6 * (1 + (3 * count) + (2 * (kWavePoints - 1))));
    addQuad(out, BarPiece::Backdrop, 0);
    for (const BarPiece piece : {BarPiece::Reflection, BarPiece::Bar, BarPiece::Cap}) {
      for (std::size_t bar = 0; bar < count; ++bar) {
        addQuad(out, piece, bar);
      }
    }
    addStroke(out, BarPiece::WaveGlow);
    addStroke(out, BarPiece::WaveLine);
    return out;
  }

  struct BarUniforms {
    GLint size = -1;
    GLint columns = -1;
    GLint rows = -1;
    GLint waveLine = -1;
    GLint backdrop = -1;
    GLint levels = -1;
    GLint caps = -1;
    GLint wave = -1;
    GLint accent = -1;
    GLint text = -1;
    GLint opacity = -1;
  };

  class BarsVisual final : public AudioVisual {
  public:
    void reset() override {
      m_scene.reset();
      ++m_revision;
    }

    void advance(const luciole::workbench::AudioFrame& frame) override {
      m_scene.advance(frame);
      ++m_revision;
    }

    void draw(const AudioVisualDraw& draw) override {
      if (draw.rect.width <= 0 || draw.rect.height <= 0) {
        return;
      }
      AudioVisualGlState saved;
      ensureProgram();
      saved.keepAttributes(std::array{m_mesh.vertexLocation()});
      const BarLayout layout = luciole::workbench::barLayout(
          static_cast<float>(draw.rect.width), static_cast<float>(draw.rect.height), draw.scale
      );
      if (layout.count == 0) {
        return;
      }
      if (!m_mesh.holds(layout.count)) {
        m_mesh.fill(layout.count, barVertices(layout.count));
      }
      m_mesh.bind();
      glViewport(draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height);
      setFrameUniforms(draw, layout);
      if (m_uploaded != m_revision || m_uploadedCount != layout.count) {
        uploadScene(layout.count);
      }
      glDrawArrays(GL_TRIANGLES, 0, m_mesh.vertexCount());
    }

    void release() override {
      m_mesh.release();
      forgetUploads();
    }

    void abandon() noexcept override {
      m_mesh.abandon();
      forgetUploads();
    }

  private:
    void ensureProgram() {
      if (m_mesh.hasProgram()) {
        return;
      }
      m_mesh.ensureProgram(kVertexShader, kFragmentShader);
      m_uniforms = {
          .size = m_mesh.uniform("u_size"),
          .columns = m_mesh.uniform("u_columns"),
          .rows = m_mesh.uniform("u_rows"),
          .waveLine = m_mesh.uniform("u_wave_line"),
          .backdrop = m_mesh.uniform("u_backdrop"),
          .levels = m_mesh.uniform("u_levels"),
          .caps = m_mesh.uniform("u_caps"),
          .wave = m_mesh.uniform("u_wave"),
          .accent = m_mesh.uniform("u_accent"),
          .text = m_mesh.uniform("u_text"),
          .opacity = m_mesh.uniform("u_opacity"),
      };
      forgetUploads();
    }

    void setFrameUniforms(const AudioVisualDraw& draw, const BarLayout& layout) const {
      glUniform2f(m_uniforms.size, static_cast<float>(draw.rect.width), static_cast<float>(draw.rect.height));
      glUniform3f(m_uniforms.columns, layout.margin, layout.slot, layout.gap);
      glUniform4f(m_uniforms.rows, layout.base, layout.top, layout.maxHeight, layout.capHeight);
      glUniform4f(m_uniforms.waveLine, layout.waveCenter, layout.waveAmplitude, layout.lineWidth, layout.glowWidth);
      glUniform2f(m_uniforms.backdrop, layout.room, m_scene.backdrop());
      setColorUniform(m_uniforms.accent, draw.palette.accents[0]);
      setColorUniform(m_uniforms.text, draw.palette.text);
      glUniform1f(m_uniforms.opacity, audioVisualOpacity(draw));
    }

    void uploadScene(std::size_t count) {
      std::array<float, kAudioBandCount> levels{};
      std::array<float, kAudioBandCount> caps{};
      m_scene.bars(std::span(levels).first(count), std::span(caps).first(count));
      glUniform4fv(m_uniforms.levels, kVectors, levels.data());
      glUniform4fv(m_uniforms.caps, kVectors, caps.data());
      glUniform4fv(m_uniforms.wave, kWaveVectors, m_scene.wave().data());
      m_uploaded = m_revision;
      m_uploadedCount = count;
    }

    void forgetUploads() {
      m_uploaded = m_revision - 1;
      m_uploadedCount = 0;
    }

    static constexpr GLsizei kVectors = kAudioBandCount / 4;
    static constexpr GLsizei kWaveVectors = kWavePoints / 4;

    BarScene m_scene;
    AudioVisualMesh m_mesh;
    BarUniforms m_uniforms;
    std::uint64_t m_revision = 1;
    std::uint64_t m_uploaded = 0;
    std::size_t m_uploadedCount = 0;
  };
} // namespace

std::unique_ptr<AudioVisual> makeBarsVisual() { return std::make_unique<BarsVisual>(); }
