#include "ui/controls/audio_visual.h"
#include "ui/controls/audio_visual_quad.h"
#include "workbench/sun_horizon.h"

#include <cstdint>
#include <memory>

namespace {
  using luciole::workbench::SunHorizon;

  // The Dark Sun picture in buffer pixels, y down: a sky washed with the haze color above the horizon, a dark disc
  // with a thin rim whose brightest point travels around it, faint corona streaks and darker filaments around the
  // sun, a thin line of light along the horizon, the range hanging below it (the row holds each column's depth;
  // linear filtering joins the columns with straight segments), and the beam below the sun's center.
  constexpr char kDarkSunShader[] = R"(
uniform vec3 u_sun;        // center x, y and radius
uniform float u_horizon;   // the horizon's y
uniform vec2 u_range;      // half width, depth of a full peak
uniform vec2 u_beam;       // half width, brightness
uniform vec4 u_light;      // rim brightness, corona reach in sun radii, diamond angle, corona turn
uniform float u_glow;      // the horizon's glow
uniform float u_cover;     // 1 when the light covers a pale background, 0 when it adds to a dark one
uniform vec3 u_lightColor;
uniform vec3 u_hazeColor;
uniform vec3 u_darkColor;

const float WIDE = 4096.0;
const float COVER = 1.3;   // coverage per unit of light on a pale background

vec2 onCircle(float angle, float radius) {
    return vec2(cos(angle), sin(angle)) * radius;
}

void main() {
    vec2 fc = v_pixel;
    float unit = min(u_size.x, u_size.y);
    float dy = fc.y - u_horizon;
    vec2 ds = (fc - u_sun.xy) / u_sun.z;
    float d = length(ds);
    float angle = atan(-ds.y, ds.x);
    float rim = u_light.x;
    float reach = u_light.y;
    float spot = u_light.z;
    float turn = u_light.w;
    float rr = d - 1.0;

    // The sky, deeper toward the horizon, darkened around the sun by a smoky halo and long filaments.
    vec4 color = vec4(0.0);
    if (dy < 0.0) {
        color = over(color, u_hazeColor, 0.12 + 0.43 * smoothstep(0.2, 1.0, fc.y / u_horizon));
        if (d > 1.0) {
            float filament = pow(valueNoise(onCircle(angle + turn * 0.3, 1.4) + 0.5, WIDE), 40.0);
            filament += 0.5 * pow(valueNoise(onCircle(angle - turn, 7.0) + vec2(4.0 + rr * 0.1), WIDE), 14.0);
            float shade = 0.45 * exp(-rr / (0.55 * reach)) + min(0.6, filament * exp(-rr / (2.2 * reach)));
            color = over(color, u_darkColor, clamp(shade, 0.0, 1.0));
        }
    }

    // The range: coverage from four columns of sub-pixels, darker toward its tips.
    float ux = (fc.x - 0.5 * u_size.x) / u_range.x;
    // Light as tinted strength in rgb and plain strength in alpha.
    vec4 light = vec4(0.0);
    vec4 sunLight = vec4(u_lightColor, 1.0);
    vec4 hazeLight = vec4(u_hazeColor, 1.0);
    if (abs(ux) < 1.0 && dy > -2.0) {
        float x01 = ux * 0.5 + 0.5;
        float quarter = 0.25 / (2.0 * u_range.x);
        float cover = 0.0;
        float depth = 0.0;
        for (int k = 0; k < 4; k++) {
            float dk = texture2D(u_row, vec2(x01 + (float(k) - 1.5) * quarter, 0.5)).r * u_range.y;
            cover += clamp(dk - dy + 0.5, 0.0, 1.0);
            depth += dk;
        }
        depth *= 0.25;
        cover *= 0.25 * step(-1.0, dy);
        if (cover > 0.0) {
            float down = clamp(dy / max(u_range.y, 1.0), 0.0, 1.0);
            float edge = smoothstep(0.0, 0.45 * max(depth, 1.0), depth - dy);
            vec3 body = mix(mix(u_lightColor, u_hazeColor, 0.4), u_lightColor, mix(0.5, 1.0, edge));
            body = mix(body, u_hazeColor, 0.3 * smoothstep(0.2, 1.0, down));
            float mottle = valueNoise(fc / unit * vec2(10.0, 2.5) + 11.0, WIDE);
            color = over(color, body, cover * clamp((0.8 + 0.25 * mottle) * (0.85 + 0.15 * u_glow), 0.0, 1.0));
        }
        // Light spilling from the ridges onto the ground, fading out toward the range's ends.
        if (dy > depth) {
            light += sunLight * exp((depth - dy) / (0.012 * unit)) * 0.16 * u_glow * (1.0 - smoothstep(0.6, 1.0, abs(ux)));
        }
    }

    // The sun: a dark disc, a crescent of light inside its rim on the bright side, a thin rim brightest at the
    // traveling spot, its bloom, and corona streaks that turn slowly and reach further with the bass.
    float aa = 1.5 / u_sun.z;
    float disc = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, d);
    color = over(color, u_darkColor, disc);
    vec2 toward = vec2(cos(spot), -sin(spot));
    float lit = max(0.0, dot(ds, toward));
    float smoke = d < 1.1 ? valueNoise(ds * 2.2 + 3.0, WIDE) : 0.5;
    light += sunLight * rim * (0.1 + 0.3 * smoke) * pow(lit, 4.0) * smoothstep(0.55, 1.0, d) * disc;
    float da = atan(sin(angle - spot), cos(angle - spot));
    float diamond = exp(-da * da / 0.3);
    float rz = rr / 0.03;
    float ring = exp(-rz * rz);
    float bloom = d > 1.0 ? exp(-rr / 0.12) : 0.0;
    light += sunLight * rim * (0.3 + 2.4 * diamond) * (ring * 0.9 + bloom * 0.5);
    light += sunLight * rim * diamond * diamond * exp(-max(rr, 0.0) / 0.22) * 0.8 * step(1.0, d);
    if (d > 0.98 && rr < 2.6 * reach && dy < 0.0) {
        float s1 = valueNoise(onCircle(angle + turn, 6.5) + vec2(rr * 0.6), WIDE);
        float s2 = valueNoise(onCircle(angle - 0.7 * turn, 2.8) + vec2(5.0, rr * 0.4), WIDE);
        float streak = pow(s1, 4.0) * 0.8 + pow(s2, 5.0) * 0.6;
        float corona = streak * exp(-rr / (0.45 * reach)) * 0.45 + exp(-rr / (0.2 * reach)) * 0.2;
        light += hazeLight * corona * rim * (0.5 + 0.8 * diamond);
    }

    // The horizon: a thin line of light with a glow that breathes above it.
    float lz = dy / (0.0022 * unit);
    float above = dy < 0.0 ? exp(dy / (0.035 * unit)) : exp(-dy / (0.006 * unit));
    light += sunLight * (exp(-lz * lz) * (0.7 + 0.4 * u_glow) + above * 0.45 * u_glow);

    // The beam: below the horizon a bright core with a wide glow; above it a narrower column down from the sun.
    float bx = abs(fc.x - u_sun.x);
    if (dy > 0.0) {
        float core = exp(-pow(bx / u_beam.x, 2.0));
        float halo = exp(-bx / (u_beam.x * 5.0));
        float fall = smoothstep(0.3, 1.0, dy / max(u_size.y - u_horizon, 1.0));
        light += mix(sunLight, mix(sunLight, hazeLight, 0.4), fall) * u_beam.y * (core * 1.2 + halo * 0.3);
    } else if (fc.y > u_sun.y) {
        float column = exp(-pow(bx / (u_beam.x * 2.0), 2.0));
        float rise = smoothstep(u_sun.y, u_horizon, fc.y);
        light += mix(hazeLight, sunLight, 0.5) * column * 0.35 * u_beam.y * (0.3 + 0.7 * rise) * (1.0 - disc);
    }

    // The light adds to a dark background; a pale one it covers with the light's own color.
    light *= 1.0 - 0.85 * disc;
    if (u_cover > 0.5) {
        color = over(color, light.rgb / max(light.a, 0.0001), 1.0 - exp(-COVER * light.a));
    } else {
        color.rgb = min(color.rgb + light.rgb, vec3(1.0));
    }
    gl_FragColor = color * u_opacity;
}
)";

  // The beam's brightness at rest, and what a full surge adds to its brightness and to its width.
  constexpr float kBeamRest = 0.25F;
  constexpr float kBeamSurge = 0.95F;
  constexpr float kBeamWiden = 2.0F;

  class DarkSunVisual final : public AudioVisual {
  public:
    DarkSunVisual() : m_quad(kDarkSunShader) {}

    void reset() override {
      m_sun.reset();
      ++m_revision;
    }

    void advance(const luciole::workbench::AudioFrame& frame) override {
      m_sun.advance(frame);
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
      m_quad.writeRow(m_sun.profile(), m_revision);
      const auto layout = luciole::workbench::layoutSun(
          static_cast<float>(draw.rect.width), static_cast<float>(draw.rect.height), m_sun.swell()
      );
      const float surge = m_sun.surge();
      glUniform3f(m_uniforms.sun, layout.sunX, layout.sunY, layout.sunRadius);
      glUniform1f(m_uniforms.horizon, layout.horizon);
      glUniform2f(m_uniforms.range, layout.rangeHalf, layout.rangeDepth);
      glUniform2f(m_uniforms.beam, layout.beamWidth * (1 + (kBeamWiden * surge)), kBeamRest + (kBeamSurge * surge));
      glUniform4f(m_uniforms.light, m_sun.rim(), m_sun.reach(), m_sun.diamond(), m_sun.coronaTurn());
      glUniform1f(m_uniforms.glow, m_sun.glow());
      const auto& palette = draw.palette;
      AudioVisualQuad::setColor(m_uniforms.lightColor, palette.accents[0]);
      AudioVisualQuad::setColor(m_uniforms.hazeColor, palette.accents[1]);
      const Color& dark = AudioVisualQuad::darker(palette.background, palette.text);
      AudioVisualQuad::setColor(m_uniforms.darkColor, dark);
      // The text is the darker color only on a pale background, which light cannot brighten.
      glUniform1f(m_uniforms.cover, &dark == &palette.text ? 1.0F : 0.0F);
      m_quad.finish();
    }

    void release() override { m_quad.release(); }

    void abandon() noexcept override { m_quad.abandon(); }

  private:
    struct Uniforms {
      GLint sun = -1;
      GLint horizon = -1;
      GLint range = -1;
      GLint beam = -1;
      GLint light = -1;
      GLint glow = -1;
      GLint cover = -1;
      GLint lightColor = -1;
      GLint hazeColor = -1;
      GLint darkColor = -1;
    };

    void findUniforms() {
      m_uniforms = {
          .sun = m_quad.uniform("u_sun"),
          .horizon = m_quad.uniform("u_horizon"),
          .range = m_quad.uniform("u_range"),
          .beam = m_quad.uniform("u_beam"),
          .light = m_quad.uniform("u_light"),
          .glow = m_quad.uniform("u_glow"),
          .cover = m_quad.uniform("u_cover"),
          .lightColor = m_quad.uniform("u_lightColor"),
          .hazeColor = m_quad.uniform("u_hazeColor"),
          .darkColor = m_quad.uniform("u_darkColor"),
      };
    }

    SunHorizon m_sun;
    AudioVisualQuad m_quad;
    Uniforms m_uniforms;
    // Advances since the visual was made; the row is written again when it differs.
    std::uint64_t m_revision = 1;
  };
} // namespace

std::unique_ptr<AudioVisual> makeDarkSunVisual() { return std::make_unique<DarkSunVisual>(); }
