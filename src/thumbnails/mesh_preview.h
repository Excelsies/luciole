#pragma once

#include "formats/mesh.h"
#include "fs/cancel_token.h"
#include "fs/location.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  enum class MeshFormat : std::uint8_t { Stl, Obj, Ply, Gltf };

  // The reader for a file name's extension, in any case: .stl, .obj, .ply, .gltf and .glb; none for any other.
  std::optional<MeshFormat> meshFormat(std::string_view fileName);

  // Bytes the helper reads for one preview: the file and, for glTF, its outside buffers together.
  inline constexpr std::uint64_t kMaxMeshSourceBytes = 256ULL * 1024U * 1024U;
  // Geometry one preview holds: one million triangles, each with its own three vertices if need be.
  inline constexpr formats::MeshLimits kMeshPreviewLimits{.maxVertices = 3'000'000, .maxTriangles = 1'000'000};
  // The largest coordinate magnitude one preview shows: far beyond any real model, and small enough that the
  // camera's single-precision matrices stay finite.
  inline constexpr float kMaxMeshCoordinate = 1e15F;

  // The axis that points up in the format's usual convention: Z for STL, Y for the others.
  enum class MeshUp : std::uint8_t { Y, Z };

  // One vertex as the mesh view uploads it: the position, then the unit normal times 127 and a padding byte. The
  // normal is zero for a degenerate triangle.
  struct MeshVertex {
    std::array<float, 3> position{};
    std::array<std::int8_t, 4> normal{};
  };
  static_assert(sizeof(MeshVertex) == 16);

  struct MeshGeometry {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices; // three per triangle, each below vertices.size()
    formats::Bounds bounds;             // finite, around every position
    MeshUp up = MeshUp::Y;
    bool truncated = false;   // a limit left geometry out
    bool flatNormals = false; // the file has no normals: every triangle has its own three vertices and normal
    formats::SkippedParts skipped;

    std::size_t triangleCount() const { return indices.size() / 3; }
    // Bytes the vectors hold.
    std::size_t bytes() const;
  };

  using MeshPreview = std::expected<std::shared_ptr<const MeshGeometry>, std::string>;

  // Blocking; run on a worker. Reads a local regular file of at most kMaxMeshSourceBytes in the resource-limited
  // helper, with the reader its name selects and kMeshPreviewLimits, and checks the answer. The error is a sentence
  // fit to show, without the file name; a reader's refusal says the model is too large, that a glTF file is not
  // version 2.0, that a glTF buffer beside it could not be read, or else that the file is damaged or not of its
  // format.
  MeshPreview loadMeshPreview(const Location& location, const CancelToken& token);

  // The helper's answer for `mesh`: its geometry, or its error when mesh.error is set. The format is private to
  // this process and its helper: "NMR1", then little-endian u32 flags, vertex count and index count, six f32
  // bounds, the positions, the normals when the flags say so and the u32 indices; or "NME1" and printable ASCII.
  std::vector<std::uint8_t> encodeMeshResult(const formats::Mesh& mesh);
  // Checks every count and length of an answer against its size and `limits` before allocating, then every
  // value; builds flat normals when it has none. An error answer, an empty mesh and anything malformed are errors.
  std::expected<MeshGeometry, std::string>
  readMeshWorkerResult(std::span<const std::uint8_t> bytes, const formats::MeshLimits& limits);

  // Helper command line; the file is descriptor 3. A glTF file's outside buffers are read from `folder`.
  std::vector<std::string>
  meshPreviewArguments(MeshFormat format, std::uint64_t maxBytes, std::string_view folder, std::string_view output);
  // Private helper protocol: format, byte cap, folder, output path. Outside buffers are read only from the folder
  // itself, by a name with no slash, without following symlinks, and count against the byte cap.
  int runMeshPreviewWorker(std::span<const std::string_view> arguments);
} // namespace luciole
