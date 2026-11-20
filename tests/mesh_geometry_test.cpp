#include "formats/mesh.h"
#include "test_check.h"
#include "thumbnails/mesh_preview.h"
#include "ui/controls/mesh_program.h"
#include "ui/controls/mesh_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <print>
#include <vector>

namespace {
  using luciole::MeshGeometry;
  using luciole::formats::Mesh;
  using luciole::formats::Vec3;

  // The geometry the helper would answer for `mesh`, after the parent's checks.
  MeshGeometry geometryOf(Mesh mesh) {
    luciole::formats::finishMesh(mesh);
    auto geometry = luciole::readMeshWorkerResult(luciole::encodeMeshResult(mesh), luciole::kMeshPreviewLimits);
    TEST_CHECK(geometry.has_value());
    return std::move(*geometry);
  }

  // A unit cube with outward triangles and no normals.
  MeshGeometry cube() {
    Mesh mesh;
    for (int bits = 0; bits < 8; ++bits) {
      mesh.positions.push_back(
          Vec3{static_cast<float>(bits & 1), static_cast<float>((bits >> 1) & 1), static_cast<float>((bits >> 2) & 1)}
      );
    }
    mesh.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                    2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    return geometryOf(std::move(mesh));
  }

  // Each part's triangles, as position triples, in order.
  std::vector<std::array<float, 9>> triangles(const MeshGeometry& geometry) {
    std::vector<std::array<float, 9>> out;
    for (std::size_t k = 0; k + 2 < geometry.indices.size(); k += 3) {
      std::array<float, 9> t{};
      for (std::size_t c = 0; c < 3; ++c) {
        const auto& p = geometry.vertices[geometry.indices[k + c]].position;
        std::ranges::copy(p, t.begin() + static_cast<std::ptrdiff_t>(c * 3));
      }
      out.push_back(t);
    }
    return out;
  }

  // A grid of quads sharing their corners, with normals: 2 * cells * cells triangles.
  MeshGeometry grid(std::uint32_t cells) {
    Mesh mesh;
    for (std::uint32_t y = 0; y <= cells; ++y) {
      for (std::uint32_t x = 0; x <= cells; ++x) {
        mesh.positions.push_back(Vec3{static_cast<float>(x), static_cast<float>(y), 0});
        mesh.normals.push_back(Vec3{0, 0, 1});
      }
    }
    for (std::uint32_t y = 0; y < cells; ++y) {
      for (std::uint32_t x = 0; x < cells; ++x) {
        const std::uint32_t a = (y * (cells + 1)) + x;
        const std::uint32_t b = a + cells + 1;
        for (const std::uint32_t index : {a, a + 1, b, a + 1, b + 1, b}) {
          mesh.indices.push_back(index);
        }
      }
    }
    return geometryOf(std::move(mesh));
  }

  void splitKeepsTriangles() {
    const MeshGeometry shared = grid(6);
    const auto whole = triangles(shared);
    for (const std::size_t cap : {std::size_t{0}, std::size_t{3}, std::size_t{7}, std::size_t{65536}}) {
      const auto parts = splitShortIndices(shared, cap);
      std::vector<std::array<float, 9>> joined;
      for (const auto& part : parts) {
        TEST_CHECK(part.vertices.size() <= std::max<std::size_t>(cap, 3));
        TEST_CHECK(!part.indices.empty() && part.indices.size() % 3 == 0);
        MeshGeometry piece;
        piece.vertices = part.vertices;
        piece.indices.assign(part.indices.begin(), part.indices.end());
        const auto pieceTriangles = triangles(piece);
        joined.insert(joined.end(), pieceTriangles.begin(), pieceTriangles.end());
      }
      TEST_CHECK(joined.size() == whole.size());
      TEST_CHECK(std::ranges::equal(joined, whole));
    }
    TEST_CHECK(splitShortIndices(shared, 65536).size() == 1);
    TEST_CHECK(splitShortIndices(shared, 7).size() > 10);
    TEST_CHECK(splitShortIndices(MeshGeometry{}).empty());
  }

  bool finite(const MeshCamera& camera) {
    const auto isFinite = [](float v) { return std::isfinite(v); };
    return std::ranges::all_of(camera.modelView, isFinite) && std::ranges::all_of(camera.projection, isFinite);
  }

  void cameraFits() {
    const MeshGeometry box = cube();
    const auto camera = meshCamera(box, MeshOrbit{0, 0, 1}, 1.0F);
    // The box's center lies on the view axis, in front of the camera.
    const auto& m = camera.modelView;
    const float cx = (m[0] * 0.5F) + (m[4] * 0.5F) + (m[8] * 0.5F) + m[12];
    const float cz = (m[2] * 0.5F) + (m[6] * 0.5F) + (m[10] * 0.5F) + m[14];
    TEST_NEAR(cx, 0.0F, 1e-5F);
    TEST_CHECK(cz < -0.87F);
    const auto closer = meshCamera(box, MeshOrbit{0, 0, 2}, 1.0F);
    TEST_CHECK(closer.modelView[14] > m[14]);
    TEST_CHECK(
        (meshCamera(box, MeshOrbit{0, 0, 100}, 1.0F) == meshCamera(box, MeshOrbit{0, 0, MeshOrbit::kMaxZoom}, 1.0F))
    );
    TEST_CHECK(
        (meshCamera(box, MeshOrbit{0, 9, 1}, 1.0F) == meshCamera(box, MeshOrbit{0, MeshOrbit::kMaxPitch, 1}, 1.0F))
    );
    const float nan = std::nanf("");
    TEST_CHECK(finite(meshCamera(box, MeshOrbit{nan, nan, nan}, nan)));
    TEST_CHECK(finite(meshCamera(MeshGeometry{}, MeshOrbit{}, 1.0F)));
  }

  // The largest box the parent accepts, seen from as far as the zoom goes through the narrowest and widest views,
  // and a smaller box through views far past those, all give finite matrices.
  void cameraStaysFinite() {
    const float most = luciole::kMaxMeshCoordinate;
    MeshGeometry largest;
    largest.bounds = {.min = Vec3{-most, -most, -most}, .max = Vec3{most, most, most}};
    MeshGeometry offCenter;
    offCenter.bounds = {.min = Vec3{most / 2, -most, 0}, .max = Vec3{most, most, 1}};
    const float tiny = std::numeric_limits<float>::denorm_min();
    const float huge = std::numeric_limits<float>::max();
    for (const MeshGeometry* geometry : {&largest, &offCenter}) {
      for (const float aspect : {tiny, 1e-30F, 1e-4F, 1.0F, 1e4F, 1e30F, huge}) {
        TEST_CHECK(
            finite(meshCamera(*geometry, MeshOrbit{.yaw = 2, .pitch = -1, .zoom = MeshOrbit::kMinZoom}, aspect))
        );
        TEST_CHECK(finite(meshCamera(*geometry, MeshOrbit{.yaw = 0, .pitch = 1, .zoom = MeshOrbit::kMaxZoom}, aspect)));
      }
    }
    // Views wider or narrower than the clamp draw as the clamp does.
    const MeshGeometry box = cube();
    TEST_CHECK((meshCamera(box, MeshOrbit{}, 1e-30F) == meshCamera(box, MeshOrbit{}, kMinMeshAspect)));
    TEST_CHECK((meshCamera(box, MeshOrbit{}, 1e30F) == meshCamera(box, MeshOrbit{}, 1 / kMinMeshAspect)));
    TEST_CHECK(!(meshCamera(box, MeshOrbit{}, 0.5F) == meshCamera(box, MeshOrbit{}, 2.0F)));
  }
} // namespace

int main() {
  splitKeepsTriangles();
  cameraFits();
  cameraStaysFinite();
  std::println("mesh geometry checks passed");
}
