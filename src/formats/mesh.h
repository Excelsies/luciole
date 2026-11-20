#pragma once

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace luciole::formats {
  struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    bool operator==(const Vec3&) const = default;
  };

  // Axis-aligned box around the positions; both corners are zero for an empty mesh.
  struct Bounds {
    Vec3 min;
    Vec3 max;
    bool operator==(const Bounds&) const = default;
  };

  // Caps every reader honors. Geometry past a cap is left out and the mesh is marked truncated.
  struct MeshLimits {
    std::size_t maxVertices = 1'000'000;
    std::size_t maxTriangles = 2'000'000;
  };

  // Parts of a file that are recognized but not read, so a preview can say why geometry is missing.
  struct SkippedParts {
    bool sparse = false;       // sparse accessors: base values are read, the sparse substitutions are not
    bool skins = false;        // skins and joints
    bool morphTargets = false; // morph targets
    bool materials = false;    // materials: the mesh carries no color
    bool extensions = false;   // extensions used or required
    bool compressed = false;   // Draco or meshopt compressed geometry, which is not drawn
    bool otherModes = false;   // primitives that are not triangle lists, which are not drawn
    bool operator==(const SkippedParts&) const = default;
  };

  struct Mesh {
    std::vector<Vec3> positions;        // the vertices the triangles use, all finite
    std::vector<Vec3> normals;          // empty, or one per position
    std::vector<std::uint32_t> indices; // three per triangle, each below positions.size()
    Bounds bounds;
    bool truncated = false;         // a limit left geometry out
    bool normalsMissing = false;    // no normals: flatNormals gives one per triangle
    bool nonFiniteDropped = false;  // NaN or infinite values were dropped with the triangles that use them
    std::size_t unusedVertices = 0; // finite vertices no triangle uses, which are dropped: a point cloud's points
    SkippedParts skipped;
    std::string error; // why the input cannot be read; the geometry is then empty

    std::size_t vertexCount() const { return positions.size(); }
    std::size_t triangleCount() const { return indices.size() / 3; }
  };

  // The box around finite points; zero corners for none.
  inline Bounds boundsOf(std::span<const Vec3> points) {
    if (points.empty()) {
      return {};
    }
    Bounds box{.min = points.front(), .max = points.front()};
    for (const Vec3& point : points) {
      box.min = Vec3{std::fmin(box.min.x, point.x), std::fmin(box.min.y, point.y), std::fmin(box.min.z, point.z)};
      box.max = Vec3{std::fmax(box.max.x, point.x), std::fmax(box.max.y, point.y), std::fmax(box.max.z, point.z)};
    }
    return box;
  }

  // One unit normal per triangle, counterclockwise winding facing the viewer; zero for a degenerate triangle.
  inline std::vector<Vec3> flatNormals(const Mesh& mesh) {
    std::vector<Vec3> normals;
    normals.reserve(mesh.triangleCount());
    for (std::size_t first = 0; first + 3 <= mesh.indices.size(); first += 3) {
      const Vec3& a = mesh.positions.at(mesh.indices[first]);
      const Vec3& b = mesh.positions.at(mesh.indices[first + 1]);
      const Vec3& c = mesh.positions.at(mesh.indices[first + 2]);
      const double ux = double{b.x} - a.x;
      const double uy = double{b.y} - a.y;
      const double uz = double{b.z} - a.z;
      const double vx = double{c.x} - a.x;
      const double vy = double{c.y} - a.y;
      const double vz = double{c.z} - a.z;
      const double nx = (uy * vz) - (uz * vy);
      const double ny = (uz * vx) - (ux * vz);
      const double nz = (ux * vy) - (uy * vx);
      const double length = std::sqrt((nx * nx) + (ny * ny) + (nz * nz));
      if (!(length > 0.0) || !std::isfinite(length)) {
        normals.push_back(Vec3{});
        continue;
      }
      normals.push_back(
          Vec3{static_cast<float>(nx / length), static_cast<float>(ny / length), static_cast<float>(nz / length)}
      );
    }
    return normals;
  }

  inline Mesh meshError(std::string message) {
    Mesh mesh;
    mesh.normalsMissing = true;
    mesh.error = std::move(message);
    return mesh;
  }

  // Readers call this last. It checks the invariants (an error when broken), drops non-finite normals as a whole,
  // drops triangles that use a non-finite position, keeps only used vertices in their order, counts the finite
  // ones it drops in unusedVertices, and sets the bounds.
  inline void finishMesh(Mesh& mesh) {
    const std::size_t count = mesh.positions.size();
    const bool shapeBroken = mesh.indices.size() % 3 != 0
        || (!mesh.normals.empty() && mesh.normals.size() != count)
        || count > std::numeric_limits<std::uint32_t>::max();
    bool indexBroken = false;
    for (const std::uint32_t index : mesh.indices) {
      indexBroken = indexBroken || index >= count;
    }
    if (shapeBroken || indexBroken) {
      Mesh failed = meshError(indexBroken ? "a triangle names a vertex that does not exist" : "inconsistent mesh");
      failed.skipped = mesh.skipped;
      mesh = std::move(failed);
      return;
    }
    const auto finite = [](const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    for (const Vec3& normal : mesh.normals) {
      if (!finite(normal)) {
        mesh.normals.clear();
        mesh.nonFiniteDropped = true;
        break;
      }
    }
    constexpr std::uint32_t kUnused = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint32_t kNonFinite = kUnused - 1;
    std::vector<std::uint32_t> remap(count, kUnused);
    for (std::size_t v = 0; v < count; ++v) {
      if (!finite(mesh.positions[v])) {
        remap[v] = kNonFinite;
        mesh.nonFiniteDropped = true;
      }
    }
    std::size_t kept = 0;
    for (std::size_t first = 0; first < mesh.indices.size(); first += 3) {
      const std::uint32_t a = mesh.indices[first];
      const std::uint32_t b = mesh.indices[first + 1];
      const std::uint32_t c = mesh.indices[first + 2];
      if (remap[a] == kNonFinite || remap[b] == kNonFinite || remap[c] == kNonFinite) {
        continue;
      }
      mesh.indices[kept] = a;
      mesh.indices[kept + 1] = b;
      mesh.indices[kept + 2] = c;
      kept += 3;
      remap[a] = remap[b] = remap[c] = 0;
    }
    mesh.indices.resize(kept);
    std::uint32_t next = 0;
    for (std::size_t v = 0; v < count; ++v) {
      if (remap[v] == 0) {
        remap[v] = next;
        mesh.positions[next] = mesh.positions[v];
        if (!mesh.normals.empty()) {
          mesh.normals[next] = mesh.normals[v];
        }
        ++next;
      } else if (remap[v] == kUnused) {
        ++mesh.unusedVertices;
      }
    }
    mesh.positions.resize(next);
    if (!mesh.normals.empty()) {
      mesh.normals.resize(next);
    }
    for (std::uint32_t& index : mesh.indices) {
      index = remap[index];
    }
    mesh.normalsMissing = mesh.normals.empty();
    mesh.bounds = boundsOf(mesh.positions);
  }
} // namespace luciole::formats

// Bounds-checked byte and text reading shared by the mesh readers.
namespace luciole::formats::mesh_io {
  inline std::optional<std::size_t> checkedAdd(std::size_t a, std::size_t b) {
    if (a > std::numeric_limits<std::size_t>::max() - b) {
      return std::nullopt;
    }
    return a + b;
  }

  inline std::optional<std::size_t> checkedMul(std::size_t a, std::size_t b) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
      return std::nullopt;
    }
    return a * b;
  }

  // Whether `length` bytes from `offset` lie inside `size` bytes.
  inline bool fits(std::size_t size, std::size_t offset, std::size_t length) {
    return offset <= size && length <= size - offset;
  }

  template <typename T>
    requires(std::is_arithmetic_v<T> && !std::same_as<T, bool>)
  std::optional<T> readScalar(std::string_view bytes, std::size_t offset, std::endian order = std::endian::little) {
    using Bits = std::conditional_t<
        sizeof(T) == 1, std::uint8_t,
        std::conditional_t<
            sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
    static_assert(sizeof(Bits) == sizeof(T));
    if (!fits(bytes.size(), offset, sizeof(T))) {
      return std::nullopt;
    }
    Bits bits{};
    std::memcpy(&bits, bytes.data() + offset, sizeof(T));
    if (order != std::endian::native) {
      bits = std::byteswap(bits);
    }
    return std::bit_cast<T>(bits);
  }

  inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

  // The whitespace-separated token at or after `pos`, empty at the end; `pos` moves past it.
  inline std::string_view nextToken(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && isSpace(text[pos])) {
      ++pos;
    }
    const std::size_t begin = pos;
    while (pos < text.size() && !isSpace(text[pos])) {
      ++pos;
    }
    return text.substr(begin, pos - begin);
  }

  // A whole-token decimal number with an optional sign, "inf" or "nan". A value beyond the double range reads as
  // NaN, so the vertex that holds it is dropped.
  inline std::optional<double> parseNumber(std::string_view token) {
    if (token.starts_with('+')) {
      token.remove_prefix(1);
      if (token.starts_with('-') || token.starts_with('+')) {
        return std::nullopt;
      }
    }
    double value = 0.0;
    const char* end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(token.data(), end, value);
    if (token.empty() || ptr != end || (ec != std::errc{} && ec != std::errc::result_out_of_range)) {
      return std::nullopt;
    }
    return ec == std::errc{} ? value : std::numeric_limits<double>::quiet_NaN();
  }

  // The nearest float; values beyond the float range become infinite.
  inline float toFloat(double value) {
    if (std::isfinite(value) && std::fabs(value) > double{std::numeric_limits<float>::max()}) {
      return value > 0.0 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    }
    return static_cast<float>(value);
  }
} // namespace luciole::formats::mesh_io

// Affine transforms that place mesh parts in a scene.
namespace luciole::formats {
  // A 4x4 matrix in column-major order.
  using Transform = std::array<double, 16>;
  inline constexpr Transform kIdentityTransform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  inline Transform multiply(const Transform& a, const Transform& b) {
    Transform out{};
    for (std::size_t col = 0; col < 4; ++col) {
      for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t k = 0; k < 4; ++k) {
          out.at((col * 4) + row) += a.at((k * 4) + row) * b.at((col * 4) + k);
        }
      }
    }
    return out;
  }

  // Translation times rotation (a unit quaternion x, y, z, w) times scale.
  inline Transform composeTransform(
      const std::array<double, 3>& translation, const std::array<double, 4>& rotation,
      const std::array<double, 3>& scale
  ) {
    const auto [x, y, z, w] = rotation;
    const std::array<double, 9> turn = {
        1 - (2 * ((y * y) + (z * z))), 2 * ((x * y) + (z * w)),       2 * ((x * z) - (y * w)),
        2 * ((x * y) - (z * w)),       1 - (2 * ((x * x) + (z * z))), 2 * ((y * z) + (x * w)),
        2 * ((x * z) + (y * w)),       2 * ((y * z) - (x * w)),       1 - (2 * ((x * x) + (y * y))),
    };
    Transform out = kIdentityTransform;
    for (std::size_t col = 0; col < 3; ++col) {
      for (std::size_t row = 0; row < 3; ++row) {
        out.at((col * 4) + row) = turn.at((col * 3) + row) * scale.at(col);
      }
      out.at(12 + col) = translation.at(col);
    }
    return out;
  }

  // Places points and normals; normals go through the inverse transpose of the linear part.
  class PointMapper {
  public:
    explicit PointMapper(const Transform& m) : m_m(m) {
      const auto a = [&](std::size_t row, std::size_t col) { return m.at((col * 4) + row); };
      for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
          const std::size_t r0 = (row + 1) % 3;
          const std::size_t r1 = (row + 2) % 3;
          const std::size_t c0 = (col + 1) % 3;
          const std::size_t c1 = (col + 2) % 3;
          m_cofactors.at((col * 3) + row) = (a(r0, c0) * a(r1, c1)) - (a(r0, c1) * a(r1, c0));
        }
      }
      m_determinant = (m[0] * m_cofactors[0]) + (m[4] * m_cofactors[3]) + (m[8] * m_cofactors[6]);
    }

    // True when the transform turns the scene inside out, so triangle winding must be reversed.
    bool mirrors() const { return m_determinant < 0.0; }

    Vec3 point(double x, double y, double z) const {
      return Vec3{
          mesh_io::toFloat((m_m[0] * x) + (m_m[4] * y) + (m_m[8] * z) + m_m[12]),
          mesh_io::toFloat((m_m[1] * x) + (m_m[5] * y) + (m_m[9] * z) + m_m[13]),
          mesh_io::toFloat((m_m[2] * x) + (m_m[6] * y) + (m_m[10] * z) + m_m[14]),
      };
    }

    // The placed normal at unit length; zero when the transform or the normal is degenerate.
    Vec3 normal(double x, double y, double z) const {
      const std::array<double, 9>& c = m_cofactors;
      const double nx = (c[0] * x) + (c[3] * y) + (c[6] * z);
      const double ny = (c[1] * x) + (c[4] * y) + (c[7] * z);
      const double nz = (c[2] * x) + (c[5] * y) + (c[8] * z);
      const double length = std::sqrt((nx * nx) + (ny * ny) + (nz * nz));
      const double scale = length > 0.0 && std::isfinite(length) ? (mirrors() ? -1.0 : 1.0) / length : 0.0;
      return Vec3{mesh_io::toFloat(nx * scale), mesh_io::toFloat(ny * scale), mesh_io::toFloat(nz * scale)};
    }

  private:
    Transform m_m;
    std::array<double, 9> m_cofactors{}; // column-major: the inverse transpose times the determinant
    double m_determinant = 0.0;
  };
} // namespace luciole::formats
