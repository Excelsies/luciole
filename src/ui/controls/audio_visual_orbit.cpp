#include "ui/controls/audio_visual.h"
#include "ui/controls/audio_visual_mesh.h"
#include "workbench/pillar_ring.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {
  using luciole::workbench::kPillarCount;
  using luciole::workbench::OrbitView;
  using luciole::workbench::PillarRing;

  // Pieces in painting order. Every vertex is (piece, star or segment or draw slot, corner u or face * 2 + u,
  // corner v); a slot names a pillar through u_order, farthest first. The boxes come twice, around the core: the
  // first run keeps the slots behind it and the second the slots in front.
  enum class OrbitPiece : std::uint8_t { Star, Reflection, Floor, FarPillar, Core, NearPillar, Glow };
  constexpr std::size_t kStars = 96;
  constexpr std::size_t kFaces = 5; // four sides and the top
  constexpr std::size_t kFloorSegments = 64;

  constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec4 a_vertex;
uniform mat4 u_view_projection;
uniform vec3 u_eye;
uniform vec3 u_right;
uniform vec3 u_up;
uniform vec4 u_order[16];
uniform vec4 u_levels[16];
uniform vec4 u_motion;
uniform vec2 u_star_size;
uniform mediump vec3 u_low;
uniform mediump vec3 u_high;
uniform mediump vec3 u_accent;
uniform mediump vec3 u_background;
uniform mediump vec3 u_text;
varying vec4 v_color;
varying vec4 v_local;

const float TAU = 6.2831853;
const float COUNT = 64.0;
const float RADIUS = 4.0;
const float HALF_WIDTH = 0.11;
const float REST = 0.08;
const float RISE = 3.2;
const float GLOW = 0.35;
const float FOG = 0.045;

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

float pillarAt(int slot) {
    int q = slot / 4;
    return floor(pick(u_order[q], slot - q * 4) + 0.5);
}

float levelAt(float pillar) {
    int i = int(pillar);
    int q = i / 4;
    return pick(u_levels[q], i - q * 4);
}

// The band ramp: the pillar's band, folded over the ring, from the low color to the high one.
vec3 rampAt(float pillar) {
    float band = pillar < COUNT * 0.5 ? pillar * 2.0 : (COUNT - 1.0 - pillar) * 2.0;
    return mix(u_low, u_high, band / (COUNT - 2.0));
}

float fogAt(vec3 world) {
    float d = FOG * distance(world, u_eye);
    return exp(-d * d);
}

vec4 place(vec3 world) {
    return u_view_projection * vec4(world, 1.0);
}

// A face of a pillar's box, mirrored under the floor for a reflection; faces turned away go outside the clip space.
vec4 face(float pillar, float faceAndU, float v, bool mirrored) {
    float side = floor(faceAndU * 0.5);
    float u = faceAndU - side * 2.0;
    float angle = pillar / COUNT * TAU;
    vec3 outward = vec3(cos(angle), 0.0, sin(angle));
    vec3 along = vec3(-outward.z, 0.0, outward.x);
    float level = levelAt(pillar);
    float height = REST + RISE * level;
    vec3 normal;
    vec3 world;
    if (side < 3.5) {
        float turn = side * 0.25 * TAU;
        normal = outward * cos(turn) + along * sin(turn);
        vec3 across = vec3(-normal.z, 0.0, normal.x);
        world = RADIUS * outward + normal * HALF_WIDTH + across * (u * 2.0 - 1.0) * HALF_WIDTH;
        world.y = v * height;
    } else {
        normal = vec3(0.0, 1.0, 0.0);
        world = RADIUS * outward + outward * (u * 2.0 - 1.0) * HALF_WIDTH + along * (v * 2.0 - 1.0) * HALF_WIDTH;
        world.y = height;
    }
    if (mirrored) {
        world.y = -world.y;
        normal.y = -normal.y;
    }
    vec3 toEye = u_eye - world;
    if (dot(normal, toEye) <= 0.0) {
        return vec4(0.0, 0.0, 2.0, 1.0);
    }
    float facing = max(dot(normal, normalize(toEye)), 0.0);
    float shade = side < 3.5 ? 0.6 + 0.4 * facing : 1.0;
    vec3 color = mix(u_background, rampAt(pillar), 0.25 + 0.75 * level) * shade;
    if (side > 3.5) {
        color = mix(color, u_text, 0.35 * level);
    }
    color = mix(u_background, color, fogAt(world));
    float alpha = mirrored ? 0.12 : 1.0;
    v_color = vec4(color * alpha, alpha);
    return place(world);
}

// u_motion: glow pulse, core radius, star field turn, pillars behind the core.
void main() {
    float piece = a_vertex.x;
    float index = a_vertex.y;
    vec2 corner = a_vertex.zw;
    v_local = vec4(0.0);
    v_color = vec4(0.0);
    if (piece < 0.5) {
        float k = index + 0.5;
        float rise = -0.7 + k / 96.0;
        float turn = index * 2.3999632 + u_motion.z;
        vec3 direction = vec3(sqrt(1.0 - rise * rise) * cos(turn), rise, sqrt(1.0 - rise * rise) * sin(turn));
        vec4 clip = place(u_eye + direction * 60.0);
        clip.xy += (corner * 2.0 - 1.0) * u_star_size * clip.w;
        v_color = vec4(u_low * 0.7, 0.0);
        v_local = vec4(corner * 2.0 - 1.0, 0.0, 2.0);
        gl_Position = clip;
    } else if (piece < 1.5) {
        gl_Position = face(pillarAt(int(index)), corner.x, corner.y, true);
    } else if (piece < 2.5) {
        float angle = (index + corner.y) / COUNT * TAU;
        vec3 world = vec3(cos(angle), 0.0, sin(angle)) * (RADIUS + (corner.x - 0.5) * 0.7);
        float alpha = 0.18 * fogAt(world);
        v_color = vec4(u_low * alpha, alpha);
        gl_Position = place(world);
    } else if (piece < 3.5 || (piece > 4.5 && piece < 5.5)) {
        bool behind = index < u_motion.w;
        gl_Position = behind == (piece < 3.5) ? face(pillarAt(int(index)), corner.x, corner.y, false)
                                              : vec4(0.0, 0.0, 2.0, 1.0);
    } else if (piece > 5.5) {
        float pillar = pillarAt(int(index));
        float angle = pillar / COUNT * TAU;
        float level = levelAt(pillar);
        float height = REST + RISE * level;
        vec2 local = vec2((corner.x * 2.0 - 1.0) * (HALF_WIDTH + GLOW), mix(-GLOW, height + GLOW, corner.y));
        vec3 world = RADIUS * vec3(cos(angle), 0.0, sin(angle)) + u_right * local.x + vec3(0.0, local.y, 0.0);
        float strength = 0.4 * level * (0.5 + 0.5 * u_motion.x) * fogAt(world);
        v_color = vec4(rampAt(pillar) * strength, 0.0);
        v_local = vec4(local, height, 1.0);
        gl_Position = place(world);
    } else {
        vec2 local = (corner * 2.0 - 1.0) * 1.6;
        vec3 world = vec3(0.0, 1.2, 0.0) + (u_right * local.x + u_up * local.y) * u_motion.y;
        v_color = vec4(mix(u_accent, u_text, 0.5), u_motion.x);
        v_local = vec4(local, 0.0, 3.0);
        gl_Position = place(world);
    }
}
)";

  // Modes in v_local.w: 0 a flat face, 1 a pillar's glow, added as light, 2 a star, added as light, 3 the core: a
  // sphere with a turning lattice of lines and a halo. A pillar's glow fades with the distance outside its box.
  constexpr char kFragmentShader[] = R"(
precision mediump float;

uniform vec2 u_core_turn;
uniform mediump vec3 u_text;
uniform float u_opacity;
varying vec4 v_color;
varying vec4 v_local;

const float TAU = 6.2831853;
const float PI = 3.14159265;
const float HALF_WIDTH = 0.11;
const float GLOW = 0.35;

float lattice(vec3 n) {
    float c = cos(u_core_turn.y);
    float s = sin(u_core_turn.y);
    vec3 tilted = vec3(n.x, c * n.y - s * n.z, s * n.y + c * n.z);
    float longitude = atan(tilted.x, tilted.z) + u_core_turn.x;
    float latitude = asin(clamp(tilted.y, -1.0, 1.0));
    float across = abs(fract(longitude * 5.0 / TAU + 0.5) - 0.5);
    float up = abs(fract(latitude * 5.0 / PI + 0.5) - 0.5);
    return 1.0 - smoothstep(0.02, 0.07, min(across, up));
}

vec4 core(vec2 local) {
    float r = length(local);
    float pulse = v_color.a;
    float light;
    float wire = 0.0;
    if (r < 1.0) {
        float z = sqrt(1.0 - r * r);
        wire = max(lattice(vec3(local, z)), 0.5 * lattice(vec3(local, -z)));
        light = 0.3 + 0.7 * wire + 0.4 * r * r * r;
    } else {
        light = 0.6 * exp(-5.0 * (r - 1.0)) * (0.5 + 0.5 * pulse) * (1.0 - smoothstep(1.2, 1.6, r));
    }
    float alpha = clamp(light, 0.0, 1.0) * 0.85;
    return vec4(mix(v_color.rgb, u_text, 0.5 * wire) * alpha, alpha);
}

void main() {
    vec4 color = v_color;
    float mode = v_local.w;
    if (mode > 2.5) {
        color = core(v_local.xy);
    } else if (mode > 1.5) {
        color *= clamp(1.0 - length(v_local.xy), 0.0, 1.0);
    } else if (mode > 0.5) {
        vec2 outside = max(vec2(abs(v_local.x) - HALF_WIDTH, max(-v_local.y, v_local.y - v_local.z)), 0.0);
        float fade = clamp(1.0 - length(outside) / GLOW, 0.0, 1.0);
        color *= fade * fade;
    }
    gl_FragColor = color * u_opacity;
}
)";

  void addQuad(std::vector<AudioVisualVertex>& out, OrbitPiece piece, std::size_t index, std::uint8_t offset) {
    const auto p = static_cast<std::uint8_t>(piece);
    const auto i = static_cast<std::uint8_t>(index);
    for (const auto& [u, v] : {std::array<std::uint8_t, 2>{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}}) {
      out.push_back({p, i, static_cast<std::uint8_t>(offset + u), v});
    }
  }

  void addBoxes(std::vector<AudioVisualVertex>& out, OrbitPiece piece) {
    for (std::size_t slot = 0; slot < kPillarCount; ++slot) {
      for (std::size_t face = 0; face < kFaces; ++face) {
        addQuad(out, piece, slot, static_cast<std::uint8_t>(face * 2));
      }
    }
  }

  std::vector<AudioVisualVertex> orbitVertices() {
    std::vector<AudioVisualVertex> out;
    out.reserve(6 * (kStars + (3 * kFaces * kPillarCount) + kFloorSegments + kPillarCount + 1));
    for (std::size_t star = 0; star < kStars; ++star) {
      addQuad(out, OrbitPiece::Star, star, 0);
    }
    addBoxes(out, OrbitPiece::Reflection);
    for (std::size_t segment = 0; segment < kFloorSegments; ++segment) {
      addQuad(out, OrbitPiece::Floor, segment, 0);
    }
    addBoxes(out, OrbitPiece::FarPillar);
    addQuad(out, OrbitPiece::Core, 0, 0);
    addBoxes(out, OrbitPiece::NearPillar);
    for (std::size_t slot = 0; slot < kPillarCount; ++slot) {
      addQuad(out, OrbitPiece::Glow, slot, 0);
    }
    return out;
  }

  constexpr std::uint64_t kOrbitShape = 1;
  // Logical px from a star's center to its edge.
  constexpr float kStarRadius = 1.6F;

  struct OrbitUniforms {
    GLint viewProjection = -1;
    GLint eye = -1;
    GLint right = -1;
    GLint up = -1;
    GLint order = -1;
    GLint levels = -1;
    GLint motion = -1;
    GLint starSize = -1;
    GLint low = -1;
    GLint high = -1;
    GLint accent = -1;
    GLint background = -1;
    GLint text = -1;
    GLint coreTurn = -1;
    GLint opacity = -1;
  };

  class OrbitVisual final : public AudioVisual {
  public:
    void reset() override {
      m_ring.reset();
      ++m_revision;
    }

    void advance(const luciole::workbench::AudioFrame& frame) override {
      m_ring.advance(frame);
      ++m_revision;
    }

    void draw(const AudioVisualDraw& draw) override {
      if (draw.rect.width <= 0 || draw.rect.height <= 0) {
        return;
      }
      AudioVisualGlState saved;
      ensureProgram();
      saved.keepAttributes(std::array{m_mesh.vertexLocation()});
      if (!m_mesh.holds(kOrbitShape)) {
        m_mesh.fill(kOrbitShape, orbitVertices());
      }
      m_mesh.bind();
      glViewport(draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height);
      const auto width = static_cast<float>(draw.rect.width);
      const auto height = static_cast<float>(draw.rect.height);
      const OrbitView view = m_ring.view(width / height);
      glUniformMatrix4fv(m_uniforms.viewProjection, 1, GL_FALSE, view.viewProjection.data());
      glUniform3fv(m_uniforms.eye, 1, view.eye.data());
      glUniform3fv(m_uniforms.right, 1, view.right.data());
      glUniform3fv(m_uniforms.up, 1, view.up.data());
      const float star = kStarRadius * std::max(draw.scale, 0.0F);
      glUniform2f(m_uniforms.starSize, 2 * star / width, 2 * star / height);
      const auto behind = static_cast<float>(luciole::workbench::pillarsBehindCore(view.eye));
      glUniform4f(m_uniforms.motion, m_ring.pulse(), m_ring.coreRadius(), m_ring.starTurn(), behind);
      glUniform2f(m_uniforms.coreTurn, m_ring.coreTurn()[0], m_ring.coreTurn()[1]);
      setColorUniform(m_uniforms.low, draw.palette.accents[1]);
      setColorUniform(m_uniforms.high, draw.palette.accents[2]);
      setColorUniform(m_uniforms.accent, draw.palette.accents[0]);
      setColorUniform(m_uniforms.background, draw.palette.background);
      setColorUniform(m_uniforms.text, draw.palette.text);
      glUniform1f(m_uniforms.opacity, audioVisualOpacity(draw));
      if (m_uploaded != m_revision) {
        uploadPillars(view);
      }
      glDrawArrays(GL_TRIANGLES, 0, m_mesh.vertexCount());
    }

    void release() override {
      m_mesh.release();
      m_uploaded = m_revision - 1;
    }

    void abandon() noexcept override {
      m_mesh.abandon();
      m_uploaded = m_revision - 1;
    }

  private:
    void ensureProgram() {
      if (m_mesh.hasProgram()) {
        return;
      }
      m_mesh.ensureProgram(kVertexShader, kFragmentShader);
      m_uniforms = {
          .viewProjection = m_mesh.uniform("u_view_projection"),
          .eye = m_mesh.uniform("u_eye"),
          .right = m_mesh.uniform("u_right"),
          .up = m_mesh.uniform("u_up"),
          .order = m_mesh.uniform("u_order"),
          .levels = m_mesh.uniform("u_levels"),
          .motion = m_mesh.uniform("u_motion"),
          .starSize = m_mesh.uniform("u_star_size"),
          .low = m_mesh.uniform("u_low"),
          .high = m_mesh.uniform("u_high"),
          .accent = m_mesh.uniform("u_accent"),
          .background = m_mesh.uniform("u_background"),
          .text = m_mesh.uniform("u_text"),
          .coreTurn = m_mesh.uniform("u_core_turn"),
          .opacity = m_mesh.uniform("u_opacity"),
      };
      m_uploaded = m_revision - 1;
    }

    // The pillars' levels and their painting order, which only an advance changes.
    void uploadPillars(const OrbitView& view) {
      const auto order = luciole::workbench::drawOrder(view.eye);
      std::array<float, kPillarCount> slots{};
      std::ranges::transform(order, slots.begin(), [](std::uint8_t pillar) { return static_cast<float>(pillar); });
      glUniform4fv(m_uniforms.order, kVectors, slots.data());
      glUniform4fv(m_uniforms.levels, kVectors, m_ring.levels().data());
      m_uploaded = m_revision;
    }

    static constexpr GLsizei kVectors = kPillarCount / 4;

    PillarRing m_ring;
    AudioVisualMesh m_mesh;
    OrbitUniforms m_uniforms;
    std::uint64_t m_revision = 1;
    std::uint64_t m_uploaded = 0;
  };
} // namespace

std::unique_ptr<AudioVisual> makeOrbitVisual() { return std::make_unique<OrbitVisual>(); }
