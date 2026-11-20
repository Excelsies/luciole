#include "formats/mesh_obj.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>
#include <unordered_map>

namespace luciole::formats {
  namespace {
    constexpr std::int64_t kNoNormal = -1;

    // Zero-based indices into every position and normal in the file.
    struct Corner {
      std::int64_t position = 0;
      std::int64_t normal = kNoNormal;
    };

    struct ObjData {
      std::vector<Vec3> positions; // at most the vertex cap
      std::vector<Vec3> normals;   // at most the vertex cap
      std::size_t positionTotal = 0;
      std::size_t normalTotal = 0;
      std::vector<Corner> corners;
      std::vector<std::size_t> faceSizes; // corners per face, each at least three
      std::size_t triangleTotal = 0;
      bool truncated = false;
    };

    using Status = std::expected<void, std::string_view>;

    std::size_t vertexCap(const MeshLimits& limits) {
      return std::min<std::size_t>(limits.maxVertices, std::numeric_limits<std::uint32_t>::max());
    }

    // A one-based or negative OBJ index as a zero-based one, given the elements defined before its line.
    std::expected<std::int64_t, std::string_view> resolveIndex(std::string_view text, std::size_t defined) {
      std::int64_t value = 0;
      const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size() || value == 0) {
        return std::unexpected("an OBJ face has an index that is not a valid number");
      }
      if (value > 0) {
        return value - 1;
      }
      const auto relative = static_cast<std::int64_t>(defined) + value;
      if (relative < 0) {
        return std::unexpected("an OBJ face counts back past the first element");
      }
      return relative;
    }

    std::expected<Corner, std::string_view> parseCorner(std::string_view token, const ObjData& data) {
      const std::size_t slash1 = token.find('/');
      const std::size_t slash2 = slash1 == std::string_view::npos ? slash1 : token.find('/', slash1 + 1);
      if (slash2 != std::string_view::npos && token.find('/', slash2 + 1) != std::string_view::npos) {
        return std::unexpected("an OBJ face corner has more than three parts");
      }
      const auto position = resolveIndex(token.substr(0, slash1), data.positionTotal);
      if (!position) {
        return std::unexpected(position.error());
      }
      Corner corner{.position = *position};
      const std::string_view normalText =
          slash2 == std::string_view::npos ? std::string_view{} : token.substr(slash2 + 1);
      if (!normalText.empty()) {
        const auto normal = resolveIndex(normalText, data.normalTotal);
        if (!normal) {
          return std::unexpected(normal.error());
        }
        corner.normal = *normal;
      }
      return corner;
    }

    std::expected<Vec3, std::string_view> parseVector(std::string_view line, std::size_t pos) {
      std::array<float, 3> values{};
      for (float& value : values) {
        const std::optional<double> number = mesh_io::parseNumber(mesh_io::nextToken(line, pos));
        if (!number) {
          return std::unexpected("an OBJ vertex needs three numbers");
        }
        value = mesh_io::toFloat(*number);
      }
      return Vec3{values[0], values[1], values[2]};
    }

    // Stores the face's corners, cut so the triangle total stays within the cap.
    Status parseFace(std::string_view line, std::size_t pos, ObjData& data, const MeshLimits& limits) {
      const std::size_t remaining = limits.maxTriangles - std::min(limits.maxTriangles, data.triangleTotal);
      if (remaining == 0) {
        data.truncated = true;
        return {};
      }
      const std::size_t first = data.corners.size();
      // Bounded by the tokens on the line and by the triangle cap.
      for (std::string_view token = mesh_io::nextToken(line, pos); !token.empty();
           token = mesh_io::nextToken(line, pos)) {
        if (data.corners.size() - first == remaining + 2) {
          data.truncated = true;
          break;
        }
        const auto corner = parseCorner(token, data);
        if (!corner) {
          return std::unexpected(corner.error());
        }
        data.corners.push_back(*corner);
      }
      const std::size_t count = data.corners.size() - first;
      if (count < 3) {
        return std::unexpected("an OBJ face has fewer than three corners");
      }
      data.faceSizes.push_back(count);
      data.triangleTotal += count - 2;
      return {};
    }

    Status parseLine(std::string_view line, ObjData& data, const MeshLimits& limits) {
      line = line.substr(0, line.find('#'));
      std::size_t pos = 0;
      const std::string_view keyword = mesh_io::nextToken(line, pos);
      if (keyword == "f") {
        return parseFace(line, pos, data, limits);
      }
      if (keyword != "v" && keyword != "vn") {
        return {};
      }
      const auto vector = parseVector(line, pos);
      if (!vector) {
        return std::unexpected(vector.error());
      }
      std::vector<Vec3>& stored = keyword == "v" ? data.positions : data.normals;
      ++(keyword == "v" ? data.positionTotal : data.normalTotal);
      if (stored.size() < vertexCap(limits)) {
        stored.push_back(*vector);
      } else {
        data.truncated = true;
      }
      return {};
    }

    Status checkIndices(const ObjData& data) {
      for (const Corner& corner : data.corners) {
        if (static_cast<std::uint64_t>(corner.position) >= data.positionTotal) {
          return std::unexpected("an OBJ face names a vertex that does not exist");
        }
        if (corner.normal != kNoNormal && static_cast<std::uint64_t>(corner.normal) >= data.normalTotal) {
          return std::unexpected("an OBJ face names a normal that does not exist");
        }
      }
      return {};
    }

    // Builds the triangles; with normals, each distinct position and normal pair becomes one vertex.
    Mesh buildMesh(ObjData& data, const MeshLimits& limits) {
      bool useNormals = !data.corners.empty();
      for (const Corner& corner : data.corners) {
        useNormals = useNormals && corner.normal != kNoNormal;
      }
      Mesh mesh;
      mesh.truncated = data.truncated;
      for (const Corner& corner : data.corners) {
        if (useNormals && static_cast<std::uint64_t>(corner.normal) >= data.normals.size()) {
          useNormals = false;
          mesh.truncated = true;
        }
      }
      const std::size_t storedPositions = data.positions.size();
      if (!useNormals) {
        mesh.positions = std::move(data.positions);
      }
      std::unordered_map<std::uint64_t, std::uint32_t> pairs;
      std::vector<std::uint32_t> faceVertices;
      std::size_t cursor = 0;
      bool full = false;
      // Bounded by the stored faces; each face by its corner count.
      for (const std::size_t size : data.faceSizes) {
        const std::span<const Corner> face(data.corners.data() + cursor, size);
        cursor += size;
        if (full || std::ranges::any_of(face, [&](const Corner& c) {
              return static_cast<std::uint64_t>(c.position) >= storedPositions;
            })) {
          mesh.truncated = true;
          continue;
        }
        faceVertices.clear();
        for (const Corner& corner : face) {
          if (!useNormals) {
            faceVertices.push_back(static_cast<std::uint32_t>(corner.position));
            continue;
          }
          const std::uint64_t key =
              (static_cast<std::uint64_t>(corner.position) << 32U) | static_cast<std::uint64_t>(corner.normal);
          const auto found = pairs.find(key);
          if (found != pairs.end()) {
            faceVertices.push_back(found->second);
            continue;
          }
          if (mesh.positions.size() >= vertexCap(limits)) {
            full = true;
            break;
          }
          const auto vertex = static_cast<std::uint32_t>(mesh.positions.size());
          mesh.positions.push_back(data.positions[static_cast<std::size_t>(corner.position)]);
          mesh.normals.push_back(data.normals[static_cast<std::size_t>(corner.normal)]);
          pairs.emplace(key, vertex);
          faceVertices.push_back(vertex);
        }
        if (full) {
          mesh.truncated = true;
          continue;
        }
        for (std::size_t k = 1; k + 1 < faceVertices.size(); ++k) {
          mesh.indices.insert(mesh.indices.end(), {faceVertices[0], faceVertices[k], faceVertices[k + 1]});
        }
      }
      finishMesh(mesh);
      return mesh;
    }
  } // namespace

  Mesh readObj(std::string_view bytes, const MeshLimits& limits) {
    ObjData data;
    std::size_t pos = 0;
    // Each pass consumes one line, so the loop ends after at most bytes.size() passes.
    while (pos < bytes.size()) {
      const std::size_t end = std::min(bytes.find('\n', pos), bytes.size());
      const Status status = parseLine(bytes.substr(pos, end - pos), data, limits);
      if (!status) {
        return meshError(std::string(status.error()));
      }
      pos = end + 1;
    }
    const Status indices = checkIndices(data);
    if (!indices) {
      return meshError(std::string(indices.error()));
    }
    return buildMesh(data, limits);
  }
} // namespace luciole::formats
