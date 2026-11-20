#include "ui/controls/audio_visual.h"
#include "ui/controls/audio_visual_quad.h"
#include "workbench/corona_field.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {
  using luciole::workbench::CoronaField;

  // The Eclipse picture in units of the fitted circle, around its center: the disc, the ring, and outside the ring a
  // corona of tapering, forking filaments in a wandering space, one tree per cell of TRUNKS around the ring with a
  // finer fringe of hairs between them. Each angle's filaments reach as far as the band at that height is loud
  // (bottom: lowest band, top: highest, the same on both sides); flares are rings of light traveling outward.
  constexpr char kEclipseShader[] = R"(
uniform vec3 u_circle;     // center x, y and radius in buffer pixels, y down
uniform vec4 u_ring;       // disc radius, outer radius, swell, turn
uniform float u_clock;     // seconds of drift
uniform vec2 u_flares[4];  // distance past the ring, amplitude
uniform vec3 u_ringColor;
uniform vec3 u_tipColor;
uniform vec3 u_flareColor;
uniform vec3 u_discColor;

const float TAU = 6.2831853;
const float PI = 3.14159265;
const float TRUNKS = 96.0;
const float REACH = 0.5;

vec4 hash4(float cell, float salt) {
    return vec4(hash(vec2(cell, salt)), hash(vec2(cell, salt + 17.0)), hash(vec2(cell, salt + 31.0)),
                hash(vec2(cell, salt + 47.0)));
}

// A tapering limb in (cell, distance) space that starts at (x0, r0), leans `lean` cells per unit outward, is `len`
// long and `w0` wide at its root; `ku` is units per cell here and `px` units per pixel. Returns its smoothed
// coverage and a soft sheath around it.
vec2 limb(vec2 q, float x0, float r0, float lean, float len, float w0, float ku, float px) {
    float t = (q.y - r0) / len;
    if (t < 0.0 || t > 1.0) {
        return vec2(0.0);
    }
    float slope = lean * ku;
    float d = abs(q.x - x0 - lean * (q.y - r0)) * ku * inversesqrt(1.0 + slope * slope);
    float w = w0 * pow(1.0 - t, 0.75);
    float core = clamp((w - d) / px + 0.5, 0.0, 1.0) * clamp(2.0 * w / px, 0.0, 1.0);
    float sheathWidth = 4.0 * w0 + 2.0 * px;
    float sheath = exp(-d * d / (sheathWidth * sheathWidth)) * (1.0 - t) * min(1.0, t * 8.0 + 0.3);
    return vec2(core, sheath);
}

// The tree rooted in `cell` of `count` around the ring: a trunk with two branches, one on each side. Neighboring
// trees grow long together in clumps that drift slowly.
vec2 tree(vec2 q, float cell, float count, float reach, float width, float ku, float px) {
    float c = mod(cell, count);
    vec4 h = hash4(c, count);
    float len = reach * (0.3 + 0.7 * h.y);
    if (q.y >= 1.2 * len) {
        return vec2(0.0);
    }
    float clump = 0.4 + 0.8 * smoothstep(0.25, 0.7, valueNoise(vec2(c / 6.0, 0.5 + u_clock * 0.02), count / 6.0));
    len *= clump;
    if (q.y >= len) {
        return vec2(0.0);
    }
    float x0 = cell + 0.5 + 0.5 * (h.x - 0.5);
    float lean = (h.w - 0.5) * 1.5;
    float w0 = width * (0.6 + 0.8 * h.z) * (0.7 + 0.3 * clump);
    vec2 v = limb(q, x0, 0.0, lean, len, w0, ku, px);
    float side = h.x > 0.5 ? 1.0 : -1.0;
    for (int b = 0; b < 2; b++) {
        vec4 g = hash4(c + 0.37 * float(b + 1), count + 1.0);
        float dir = b == 0 ? side : -side;
        float f = (float(b) + 0.3 + 0.5 * g.x) * 0.375;
        float rb = f * len;
        float lb = (1.0 - f) * len * (0.4 + 0.5 * g.z);
        float wb = w0 * pow(1.0 - f, 0.75) * 0.75;
        v = max(v, vec2(0.88, 0.8) * limb(q, x0 + lean * rb, rb, lean + dir * (2.5 + 5.0 * g.y), lb, wb, ku, px));
    }
    return v;
}

// A short hair rooted in `cell` of `count`.
vec2 hair(vec2 q, float cell, float count, float reach, float width, float ku, float px) {
    vec4 h = hash4(mod(cell, count), count);
    float len = reach * (0.3 + 0.7 * h.y);
    float x0 = cell + 0.5 + 0.5 * (h.x - 0.5);
    return limb(q, x0, 0.0, (h.w - 0.5) * 1.5, len, width * (0.6 + 0.8 * h.z), ku, px);
}

void main() {
    vec2 p = (v_pixel - u_circle.xy) / u_circle.z;
    float r = length(p);
    float px = 1.0 / u_circle.z;
    float angle = atan(p.x, p.y);
    float height = abs(angle) / PI;
    float level = texture2D(u_row, vec2((height * 63.0 + 0.5) / 64.0, 0.5)).r;
    float disc = u_ring.x;
    float outer = u_ring.y;
    float swell = u_ring.z;
    float rho = max(r - outer, 0.0);

    float flare = 0.0;
    for (int i = 0; i < 4; i++) {
        float d = (rho - u_flares[i].x) / 0.06;
        flare += u_flares[i].y * exp(-d * d);
    }

    float corona = 0.0;
    float t = 1.0;
    if (r > outer - 0.02) {
        float reach = 0.055 + REACH * pow(level, 0.8) + 0.22 * flare * (0.4 + level);
        t = rho / reach;
        if (t < 1.05) {
            float u = (angle + u_ring.w) / TAU * TRUNKS;
            vec2 wq = vec2(u * 0.25, rho * 5.0 - u_clock * 0.05);
            vec2 warp = vec2(valueNoise(wq, TRUNKS * 0.25), valueNoise(wq + vec2(7.3, 2.9), TRUNKS * 0.25)) * 2.0 - 1.0;
            float jitter = valueNoise(vec2(u, rho * 28.0 + u_clock * 0.1), TRUNKS) * 2.0 - 1.0;
            vec2 q = vec2(u + (0.9 + 0.8 * t) * warp.x + 0.25 * t * jitter, rho);
            float ku = TAU * r / TRUNKS;
            float width = max(0.0045 + 0.003 * level, 1.5 * px);
            float c0 = floor(q.x);
            vec2 filament = vec2(0.0);
            for (int j = -1; j <= 1; j++) {
                filament = max(filament, tree(q, c0 + float(j), TRUNKS, reach, width, ku, px));
            }
            vec2 q1 = vec2(q.x * 3.0, q.y);
            float c1 = floor(q1.x);
            for (int j = -1; j <= 1; j++) {
                filament = max(filament, vec2(0.85, 0.5) * hair(q1, c1 + float(j), 3.0 * TRUNKS, 0.02 + 0.3 * reach,
                                                                  0.6 * width, ku / 3.0, px));
            }
            corona = max(filament.x, 0.3 * filament.y) * mix(1.0, 0.62, smoothstep(0.0, 1.0, t));
            float fused = 1.0 - smoothstep(0.0, 0.003 + 0.004 * level,
                                           rho - 0.004 - 0.015 * level - 0.006 * warp.x - 0.003 * jitter);
            corona = max(corona, fused) * (1.0 + 0.8 * flare);
        }
    }
    float glow = exp(-rho / (0.004 + 0.012 * swell)) * (0.45 + 0.4 * swell);
    float haze = 0.25 * flare * (1.0 - smoothstep(0.0, 0.6, rho));

    vec4 color = over(vec4(0.0), u_flareColor, clamp(haze, 0.0, 1.0));
    color = over(color, u_ringColor, clamp(glow, 0.0, 1.0));
    color = over(color, mix(u_ringColor, u_tipColor, smoothstep(0.1, 0.9, t)), clamp(corona, 0.0, 1.0));
    color *= step(outer - 0.02, r);
    float ringCover = clamp((r - disc) / px + 0.5, 0.0, 1.0) * clamp((outer - r) / px + 0.5, 0.0, 1.0);
    color = over(color, u_ringColor, ringCover);
    color = over(color, u_discColor, clamp((disc - r) / px + 0.5, 0.0, 1.0));
    gl_FragColor = color * u_opacity;
}
)";

  class EclipseVisual final : public AudioVisual {
  public:
    EclipseVisual() : m_quad(kEclipseShader) {}

    void reset() override {
      m_field.reset();
      ++m_revision;
    }

    void advance(const luciole::workbench::AudioFrame& frame) override {
      m_field.advance(frame);
      ++m_revision;
    }

    void draw(const AudioVisualDraw& draw) override {
      if (draw.rect.width <= 0 || draw.rect.height <= 0) {
        return;
      }
      AudioVisualGlState saved;
      if (m_quad.begin(draw, saved)) {
        findUniforms();
      }
      m_quad.writeRow(m_field.levels(), m_revision);
      const auto circle =
          luciole::workbench::fitCorona(static_cast<float>(draw.rect.width), static_cast<float>(draw.rect.height));
      glUniform3f(m_uniforms.circle, circle.centerX, circle.centerY, circle.radius);
      glUniform4f(m_uniforms.ring, m_field.discRadius(), m_field.outerRadius(), m_field.swell(), m_field.turn());
      glUniform1f(m_uniforms.clock, m_field.clock());
      std::array<float, 2 * luciole::workbench::kCoronaFlareCount> flares{};
      for (std::size_t i = 0; i < luciole::workbench::kCoronaFlareCount; ++i) {
        flares[2 * i] = m_field.flares()[i].distance;
        flares[(2 * i) + 1] = m_field.flares()[i].amplitude;
      }
      glUniform2fv(m_uniforms.flares, static_cast<GLsizei>(luciole::workbench::kCoronaFlareCount), flares.data());
      const auto& palette = draw.palette;
      AudioVisualQuad::setColor(m_uniforms.ringColor, palette.accents[0]);
      AudioVisualQuad::setColor(m_uniforms.tipColor, palette.accents[1]);
      AudioVisualQuad::setColor(m_uniforms.flareColor, palette.accents[2]);
      AudioVisualQuad::setColor(m_uniforms.discColor, AudioVisualQuad::darker(palette.background, palette.text));
      m_quad.finish();
    }

    void release() override { m_quad.release(); }

    void abandon() noexcept override { m_quad.abandon(); }

  private:
    struct Uniforms {
      GLint circle = -1;
      GLint ring = -1;
      GLint clock = -1;
      GLint flares = -1;
      GLint ringColor = -1;
      GLint tipColor = -1;
      GLint flareColor = -1;
      GLint discColor = -1;
    };

    void findUniforms() {
      m_uniforms = {
          .circle = m_quad.uniform("u_circle"),
          .ring = m_quad.uniform("u_ring"),
          .clock = m_quad.uniform("u_clock"),
          .flares = m_quad.uniform("u_flares"),
          .ringColor = m_quad.uniform("u_ringColor"),
          .tipColor = m_quad.uniform("u_tipColor"),
          .flareColor = m_quad.uniform("u_flareColor"),
          .discColor = m_quad.uniform("u_discColor"),
      };
    }

    CoronaField m_field;
    AudioVisualQuad m_quad;
    Uniforms m_uniforms;
    // Advances since the visual was made; the row is written again when it differs.
    std::uint64_t m_revision = 1;
  };
} // namespace

std::unique_ptr<AudioVisual> makeEclipseVisual() { return std::make_unique<EclipseVisual>(); }
