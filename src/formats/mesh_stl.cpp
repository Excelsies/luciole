#include "formats/mesh_stl.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace luciole::formats {
  namespace {
    constexpr std::size_t kHeaderBytes = 84;
    constexpr std::size_t kTriangleBytes = 50;
    constexpr std::size_t kCountOffset = 80;

    // Triangles that fit both caps; each triangle has three vertices of its own.
    std::size_t triangleCap(const MeshLimits& limits) { return std::min(limits.maxTriangles, limits.maxVertices / 3); }

    void addTriangle(Mesh& mesh, const std::array<Vec3, 3>& corners) {
      const auto first = static_cast<std::uint32_t>(mesh.positions.size());
      mesh.positions.insert(mesh.positions.end(), corners.begin(), corners.end());
      mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2});
    }

    Mesh readBinary(std::string_view bytes, std::size_t count, const MeshLimits& limits) {
      Mesh mesh;
      const std::size_t stored = std::min(count, triangleCap(limits));
      mesh.truncated = stored < count;
      mesh.positions.reserve(stored * 3);
      mesh.indices.reserve(stored * 3);
      // Bounded by the stored count, which the caller checked against the file size.
      for (std::size_t t = 0; t < stored; ++t) {
        const std::size_t corner0 = kHeaderBytes + (t * kTriangleBytes) + 12;
        std::array<Vec3, 3> corners{};
        for (std::size_t c = 0; c < 3; ++c) {
          const std::size_t at = corner0 + (c * 12);
          const auto x = mesh_io::readScalar<float>(bytes, at);
          const auto y = mesh_io::readScalar<float>(bytes, at + 4);
          const auto z = mesh_io::readScalar<float>(bytes, at + 8);
          if (!x || !y || !z) {
            return meshError("the STL file ends inside a triangle");
          }
          corners.at(c) = Vec3{*x, *y, *z};
        }
        addTriangle(mesh, corners);
      }
      finishMesh(mesh);
      return mesh;
    }

    // Where the facets start: after the "solid" line's name, which ends at the first line end or before a
    // "facet" token on that line.
    std::size_t facetsStart(std::string_view text) {
      std::size_t pos = 0;
      mesh_io::nextToken(text, pos); // "solid"
      const std::size_t lineEnd = std::min(text.find_first_of("\r\n", pos), text.size());
      // Every iteration consumes one token of the first line or stops.
      while (pos < lineEnd) {
        const std::string_view token = mesh_io::nextToken(text, pos);
        const std::size_t start = pos - token.size();
        if (token.empty() || start >= lineEnd) {
          break;
        }
        if (token == "facet") {
          return start;
        }
      }
      return lineEnd;
    }

    Mesh readAscii(std::string_view text, const MeshLimits& limits) {
      Mesh mesh;
      const std::size_t cap = triangleCap(limits);
      std::size_t pos = facetsStart(text);
      std::array<Vec3, 3> corners{};
      std::size_t cornerCount = 0;
      // Every iteration consumes one token or stops at the end.
      for (std::string_view token = mesh_io::nextToken(text, pos); !token.empty();
           token = mesh_io::nextToken(text, pos)) {
        if (token == "vertex") {
          std::array<float, 3> values{};
          for (float& value : values) {
            const std::optional<double> number = mesh_io::parseNumber(mesh_io::nextToken(text, pos));
            if (!number) {
              return meshError("an STL vertex has a coordinate that is not a number");
            }
            value = mesh_io::toFloat(*number);
          }
          if (cornerCount < corners.size()) {
            corners.at(cornerCount) = Vec3{values[0], values[1], values[2]};
          }
          ++cornerCount;
        } else if (token == "endloop") {
          if (cornerCount != 3) {
            return meshError("an STL facet does not have three vertices");
          }
          cornerCount = 0;
          if (mesh.triangleCount() >= cap) {
            mesh.truncated = true;
            break;
          }
          addTriangle(mesh, corners);
        }
      }
      finishMesh(mesh);
      return mesh;
    }

    bool startsWithSolid(std::string_view bytes) {
      std::size_t pos = 0;
      return mesh_io::nextToken(bytes.substr(0, std::min<std::size_t>(bytes.size(), 256)), pos) == "solid";
    }
  } // namespace

  Mesh readStl(std::string_view bytes, const MeshLimits& limits) {
    const std::optional<std::uint32_t> count = mesh_io::readScalar<std::uint32_t>(bytes, kCountOffset);
    const std::uint64_t binarySize = count ? kHeaderBytes + (std::uint64_t{*count} * kTriangleBytes) : std::uint64_t{0};
    if (count && binarySize == bytes.size()) {
      return readBinary(bytes, *count, limits);
    }
    const bool countFits = count && binarySize <= bytes.size();
    if (startsWithSolid(bytes)) {
      // Binary writers may start the header with "solid" too: binary data holds no ASCII facet.
      Mesh ascii = readAscii(bytes, limits);
      if (ascii.triangleCount() > 0 || !countFits) {
        return ascii;
      }
    }
    if (!count) {
      return meshError("not an STL file");
    }
    if (!countFits) {
      return meshError("the STL triangle count is larger than the file");
    }
    return readBinary(bytes, *count, limits);
  }
} // namespace luciole::formats
