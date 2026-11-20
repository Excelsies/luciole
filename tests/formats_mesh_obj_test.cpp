#include "formats/mesh.h"
#include "formats/mesh_obj.h"
#include "test_check.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats;

  using Indices = std::vector<std::uint32_t>;

  constexpr std::string_view kCubeVertices = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                             "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n";

  // A unit cube of six quads.
  std::string cube() {
    return std::string(kCubeVertices) + "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n";
  }

  // The cube with one normal per side, written as v//vn.
  std::string cubeWithSideNormals() {
    return std::string(kCubeVertices)
        + "vn 0 0 -1\nvn 0 0 1\nvn 0 -1 0\nvn 1 0 0\nvn 0 1 0\nvn -1 0 0\n"
        + "f 1//1 4//1 3//1 2//1\nf 5//2 6//2 7//2 8//2\nf 1//3 2//3 6//3 5//3\nf 2//4 3//4 7//4 6//4\n"
        + "f 3//5 4//5 8//5 7//5\nf 4//6 1//6 5//6 8//6\n";
  }

  void checkConsistent(const Mesh& mesh) {
    TEST_CHECK(mesh.indices.size() % 3 == 0);
    TEST_CHECK(mesh.normals.empty() || mesh.normals.size() == mesh.positions.size());
    for (const std::uint32_t index : mesh.indices) {
      TEST_CHECK(index < mesh.positions.size());
    }
    if (!mesh.error.empty()) {
      TEST_CHECK(mesh.positions.empty());
      TEST_CHECK(mesh.indices.empty());
    }
  }

  void testCube() {
    const Mesh mesh = readObj(cube());
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.vertexCount() == 8);
    TEST_CHECK(mesh.triangleCount() == 12);
    TEST_CHECK(mesh.normalsMissing);
    TEST_CHECK(!mesh.truncated);
    TEST_CHECK((mesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{1, 1, 1}}));
    TEST_CHECK((Indices(mesh.indices.begin(), mesh.indices.begin() + 6) == Indices{0, 3, 2, 0, 2, 1}));
    checkConsistent(mesh);
  }

  void testSideNormalsSplitCorners() {
    const Mesh mesh = readObj(cubeWithSideNormals());
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.vertexCount() == 24); // each corner appears once per side it touches
    TEST_CHECK(mesh.triangleCount() == 12);
    TEST_CHECK(!mesh.normalsMissing);
    TEST_CHECK(mesh.normals.size() == 24);
    TEST_CHECK((mesh.normals[mesh.indices[0]] == Vec3{0, 0, -1}));
    TEST_CHECK((mesh.normals[mesh.indices[35]] == Vec3{-1, 0, 0}));
    checkConsistent(mesh);
  }

  void testIndexForms() {
    const std::string head = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n";
    for (const std::string_view face : {"f 1 2 3\n", "f 1/1 2/2 3/3\n"}) {
      const Mesh mesh = readObj(head + std::string(face));
      TEST_CHECK(mesh.error.empty());
      TEST_CHECK(mesh.triangleCount() == 1);
      TEST_CHECK(mesh.vertexCount() == 3);
      TEST_CHECK(mesh.normalsMissing);
      TEST_CHECK((mesh.indices == Indices{0, 1, 2}));
    }
    for (const std::string_view face : {"f 1//1 2//1 3//1\n", "f 1/1/1 2/2/1 3/3/1\n", "f -3/-3/-1 -2//-1 -1/1/1\n"}) {
      const Mesh mesh = readObj(head + std::string(face));
      TEST_CHECK(mesh.error.empty());
      TEST_CHECK(mesh.triangleCount() == 1);
      TEST_CHECK(mesh.vertexCount() == 3);
      TEST_CHECK(!mesh.normalsMissing);
      TEST_CHECK((mesh.normals[2] == Vec3{0, 0, 1}));
      TEST_CHECK((mesh.positions[mesh.indices[1]] == Vec3{1, 0, 0}));
    }
  }

  void testNegativeIndicesAreRelativeToTheirLine() {
    const std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\nv 5 0 0\nv 6 0 0\nv 5 1 0\nf -3 -2 -1\n";
    const Mesh mesh = readObj(text);
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 2);
    TEST_CHECK((mesh.indices == Indices{0, 1, 2, 3, 4, 5}));
    TEST_CHECK((mesh.positions[3] == Vec3{5, 0, 0}));
  }

  void testFanAndIgnoredLines() {
    const std::string text = "# a pentagon\r\nmtllib a.mtl\r\no shape\r\ng group\r\ns 1\r\nusemtl red\r\n"
                             "v 0 0 0 1\r\nv 2 0 0\r\nv 3 1 0 0.5 0.5 0.5\r\nv 1 2 0\r\nv -1 1 0\r\nvp 0.5\r\n"
                             "l 1 2\r\np 1\r\n\r\n   \r\nf 1 2 3 4 5 # trailing comment\r\n";
    const Mesh mesh = readObj(text);
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 3);
    TEST_CHECK((mesh.indices == Indices{0, 1, 2, 0, 2, 3, 0, 3, 4}));
    TEST_CHECK((mesh.bounds == Bounds{.min = Vec3{-1, 0, 0}, .max = Vec3{3, 2, 0}}));
  }

  void testForwardReference() {
    const Mesh mesh = readObj("f 1 2 3\nv 0 0 0\nv 1 0 0\nv 0 1 0\n");
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 1);
  }

  void testPointCloudCountsItsPoints() {
    const Mesh points = readObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nv nan 0 0\n");
    TEST_CHECK(points.error.empty());
    TEST_CHECK(points.triangleCount() == 0);
    TEST_CHECK(points.vertexCount() == 0);
    TEST_CHECK(points.unusedVertices == 3); // the NaN point is not counted

    const Mesh oneSpare = readObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 5 5 5\nf 1 2 3\n");
    TEST_CHECK(oneSpare.triangleCount() == 1);
    TEST_CHECK(oneSpare.unusedVertices == 1);
  }

  void testFacesShareCornersWithTheSameNormal() {
    const Mesh mesh = readObj("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\nf 1//1 3//1 4//1\n");
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.vertexCount() == 4);
    TEST_CHECK(mesh.triangleCount() == 2);
    TEST_CHECK((mesh.indices == Indices{0, 1, 2, 0, 2, 3}));
  }

  void testMixedNormalsAreDropped() {
    const Mesh mesh = readObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\nf 2 4 3\n");
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 2);
    TEST_CHECK(mesh.vertexCount() == 4);
    TEST_CHECK(mesh.normalsMissing);
  }

  void testLimits() {
    const Mesh byVertices = readObj(cube(), MeshLimits{.maxVertices = 4, .maxTriangles = 100});
    TEST_CHECK(byVertices.error.empty());
    TEST_CHECK(byVertices.truncated);
    TEST_CHECK(byVertices.triangleCount() == 2); // only the side made of the first four vertices
    TEST_CHECK(byVertices.vertexCount() == 4);

    const Mesh byTriangles = readObj(cube(), MeshLimits{.maxVertices = 100, .maxTriangles = 5});
    TEST_CHECK(byTriangles.truncated);
    TEST_CHECK(byTriangles.triangleCount() == 5);

    const Mesh splitVertices = readObj(cubeWithSideNormals(), MeshLimits{.maxVertices = 10, .maxTriangles = 100});
    TEST_CHECK(splitVertices.truncated);
    TEST_CHECK(splitVertices.vertexCount() <= 10);
    TEST_CHECK(splitVertices.triangleCount() == 4); // two sides of four split corners each
    checkConsistent(splitVertices);
  }

  void testNonFinite() {
    const Mesh mesh = readObj("v nan 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 inf\nv 1 1 1\nf 1 2 3\nf 2 3 5\nf 2 4 5\n");
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.nonFiniteDropped);
    TEST_CHECK(mesh.triangleCount() == 1);
    TEST_CHECK(mesh.vertexCount() == 3);
  }

  void testMalformed() {
    const std::string tri = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
    for (const std::string_view face :
         {"f 0 1 2\n", "f 1 2 4\n", "f -4 -2 -1\n", "f 1//2 2//2 3//2\n", "f a b c\n", "f 1 2\n", "f 1/ 2 3x\n",
          "f 99999999999999999999 1 2\n", "f 1///1 2 3\n", "f 1//x 2 3\n"}) {
      const Mesh mesh = readObj(tri + std::string(face));
      TEST_CHECK(!mesh.error.empty());
      checkConsistent(mesh);
    }
    TEST_CHECK(!readObj("v 1 2\n").error.empty());
    TEST_CHECK(!readObj("vn 1 x 2\n").error.empty());
    const Mesh empty = readObj("");
    TEST_CHECK(empty.error.empty());
    TEST_CHECK(empty.triangleCount() == 0);
  }

  void testMegabyteLines() {
    const Mesh comment = readObj("# " + std::string(1 << 20, 'c') + "\n" + cube());
    TEST_CHECK(comment.error.empty());
    TEST_CHECK(comment.triangleCount() == 12);

    // One face with about 200,000 corners fans into one triangle fewer than two per corner.
    std::string face = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf";
    std::size_t corners = 0;
    while (face.size() < (1U << 20)) {
      face += " 1 2 3";
      corners += 3;
    }
    const Mesh fan = readObj(face + "\n");
    TEST_CHECK(fan.error.empty());
    TEST_CHECK(fan.triangleCount() == corners - 2);

    const Mesh capped = readObj(face + "\n", MeshLimits{.maxVertices = 10, .maxTriangles = 7});
    TEST_CHECK(capped.truncated);
    TEST_CHECK(capped.triangleCount() == 7);

    const Mesh longIndex = readObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 " + std::string(1 << 20, '3') + "\n");
    TEST_CHECK(!longIndex.error.empty());
  }

  void testEveryTruncation() {
    const std::string full = cubeWithSideNormals();
    for (std::size_t length = 0; length < full.size(); ++length) {
      const Mesh mesh = readObj(std::string_view(full).substr(0, length));
      checkConsistent(mesh);
      TEST_CHECK(mesh.triangleCount() <= 12);
    }
  }
} // namespace

int main() {
  testCube();
  testSideNormalsSplitCorners();
  testIndexForms();
  testNegativeIndicesAreRelativeToTheirLine();
  testFanAndIgnoredLines();
  testForwardReference();
  testPointCloudCountsItsPoints();
  testFacesShareCornersWithTheSameNormal();
  testMixedNormalsAreDropped();
  testLimits();
  testNonFinite();
  testMalformed();
  testMegabyteLines();
  testEveryTruncation();
  return 0;
}
