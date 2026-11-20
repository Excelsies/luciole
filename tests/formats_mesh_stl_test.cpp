#include "formats/mesh.h"
#include "formats/mesh_stl.h"
#include "test_check.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats;

  using Triangle = std::array<Vec3, 3>;

  // A tetrahedron with corners at the origin and on the three unit axes.
  const std::vector<Triangle> kTetrahedron = {
      Triangle{Vec3{0, 0, 0}, Vec3{0, 1, 0}, Vec3{1, 0, 0}},
      Triangle{Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}},
      Triangle{Vec3{0, 0, 0}, Vec3{0, 0, 1}, Vec3{0, 1, 0}},
      Triangle{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}},
  };

  void putU16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
  }

  void putU32(std::string& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
  }

  void putF32(std::string& out, float value) { putU32(out, std::bit_cast<std::uint32_t>(value)); }

  void putVec(std::string& out, const Vec3& v) {
    putF32(out, v.x);
    putF32(out, v.y);
    putF32(out, v.z);
  }

  std::string binaryStl(const std::vector<Triangle>& triangles, std::string_view header = "binary") {
    std::string out(header);
    out.resize(80, ' ');
    putU32(out, static_cast<std::uint32_t>(triangles.size()));
    for (const Triangle& triangle : triangles) {
      putVec(out, Vec3{0, 0, 0});
      for (const Vec3& corner : triangle) {
        putVec(out, corner);
      }
      putU16(out, 0);
    }
    return out;
  }

  std::string asciiStl(std::string_view name = "tetra") {
    std::string out = "solid " + std::string(name) + "\n";
    for (const Triangle& triangle : kTetrahedron) {
      out += "  facet normal 0 0 0\n    outer loop\n";
      for (const Vec3& corner : triangle) {
        out += "      vertex "
            + std::to_string(corner.x)
            + " "
            + std::to_string(corner.y)
            + " "
            + std::to_string(corner.z)
            + "\n";
      }
      out += "    endloop\n  endfacet\n";
    }
    out += "endsolid " + std::string(name) + "\n";
    return out;
  }

  void checkUnitBounds(const Mesh& mesh) {
    TEST_CHECK((mesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{1, 1, 1}}));
  }

  // Every index names a stored vertex and triangles come in threes.
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

  void testFlatNormalsAndBounds() {
    Mesh mesh;
    mesh.positions = {Vec3{0, 0, 0}, Vec3{2, 0, 0}, Vec3{0, 3, 0}, Vec3{5, 0, 0}};
    mesh.indices = {0, 1, 2, 0, 1, 3};
    const std::vector<Vec3> normals = flatNormals(mesh);
    TEST_CHECK(normals.size() == 2);
    TEST_CHECK((normals[0] == Vec3{0, 0, 1}));
    TEST_CHECK((normals[1] == Vec3{0, 0, 0})); // degenerate: all three on the x axis
    TEST_CHECK((boundsOf(mesh.positions) == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{5, 3, 0}}));
    TEST_CHECK((boundsOf({}) == Bounds{}));
  }

  void testFinishMeshCompactsAndValidates() {
    Mesh mesh;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    mesh.positions = {Vec3{9, 9, 9}, Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{nan, 0, 0}};
    mesh.indices = {1, 2, 3, 1, 2, 4};
    finishMesh(mesh);
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.vertexCount() == 3); // the unused vertex and the NaN vertex are gone
    TEST_CHECK(mesh.unusedVertices == 1);
    TEST_CHECK(mesh.triangleCount() == 1);
    TEST_CHECK((mesh.indices == std::vector<std::uint32_t>{0, 1, 2}));
    TEST_CHECK(mesh.nonFiniteDropped);
    TEST_CHECK(mesh.normalsMissing);
    TEST_CHECK((mesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{1, 1, 0}}));

    Mesh normals;
    normals.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    normals.normals = {Vec3{0, 0, 1}, Vec3{0, 0, std::numeric_limits<float>::infinity()}, Vec3{0, 0, 1}};
    normals.indices = {0, 1, 2};
    finishMesh(normals);
    TEST_CHECK(normals.normals.empty());
    TEST_CHECK(normals.normalsMissing);
    TEST_CHECK(normals.nonFiniteDropped);
    TEST_CHECK(normals.triangleCount() == 1);

    Mesh outOfRange;
    outOfRange.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    outOfRange.indices = {0, 1, 3};
    finishMesh(outOfRange);
    TEST_CHECK(!outOfRange.error.empty());
    checkConsistent(outOfRange);

    Mesh partial;
    partial.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}};
    partial.indices = {0, 1};
    finishMesh(partial);
    TEST_CHECK(!partial.error.empty());
  }

  void testBinary() {
    const Mesh mesh = readStl(binaryStl(kTetrahedron));
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 4);
    TEST_CHECK(mesh.vertexCount() == 12); // corners stay unshared
    TEST_CHECK(mesh.normalsMissing);
    TEST_CHECK(!mesh.truncated);
    TEST_CHECK(!mesh.nonFiniteDropped);
    TEST_CHECK((mesh.positions[1] == Vec3{0, 1, 0}));
    checkUnitBounds(mesh);
    checkConsistent(mesh);
  }

  void testBinaryThatStartsWithSolid() {
    const Mesh mesh = readStl(binaryStl(kTetrahedron, "solid exported by a binary writer"));
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 4);
    checkUnitBounds(mesh);
  }

  void testBinaryWithTrailingBytes() {
    std::string bytes = binaryStl(kTetrahedron);
    bytes += "padding";
    const Mesh mesh = readStl(bytes);
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 4);
  }

  void testSolidHeaderWithTrailingBytes() {
    std::string bytes = binaryStl(kTetrahedron, "solid exported by a binary writer");
    bytes += "padding";
    const Mesh mesh = readStl(bytes);
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 4);
    checkUnitBounds(mesh);

    // A count that the file cannot hold keeps the ASCII reading.
    bytes[80] = 9;
    const Mesh tooMany = readStl(bytes);
    TEST_CHECK(tooMany.triangleCount() == 0);
    checkConsistent(tooMany);
  }

  void testAsciiWithoutLineFeeds() {
    const std::string oneLine = "solid x facet normal 0 0 1 outer loop vertex 0 0 0 vertex 1 0 0 vertex 0 1 0 "
                                "endloop endfacet endsolid x";
    const Mesh lineMesh = readStl(oneLine);
    TEST_CHECK(lineMesh.error.empty());
    TEST_CHECK(lineMesh.triangleCount() == 1);
    TEST_CHECK((lineMesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{1, 1, 0}}));

    const std::string returns = "solid vertex\rfacet normal 0 0 1\router loop\rvertex 0 0 0\rvertex 1 0 0\r"
                                "vertex 0 1 0\rendloop\rendfacet\rendsolid x\r";
    const Mesh returnMesh = readStl(returns);
    TEST_CHECK(returnMesh.error.empty());
    TEST_CHECK(returnMesh.triangleCount() == 1);
  }

  void testAscii() {
    const Mesh mesh = readStl(asciiStl());
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 4);
    TEST_CHECK(mesh.vertexCount() == 12);
    TEST_CHECK(mesh.normalsMissing);
    checkUnitBounds(mesh);
    checkConsistent(mesh);

    // The name line may hold keywords; signs and exponents are numbers.
    const std::string signedForms = "solid vertex endloop\nfacet normal 0 0 1\nouter loop\nvertex +1e0 0 0\n"
                                    "vertex 0 1.0E+00 -0\nvertex 0 0 0\nendloop\nendfacet\nendsolid\n";
    const Mesh signedMesh = readStl(signedForms);
    TEST_CHECK(signedMesh.error.empty());
    TEST_CHECK(signedMesh.triangleCount() == 1);
    TEST_CHECK((signedMesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{1, 1, 0}}));
  }

  void testLimits() {
    const Mesh byTriangles = readStl(binaryStl(kTetrahedron), MeshLimits{.maxVertices = 100, .maxTriangles = 2});
    TEST_CHECK(byTriangles.error.empty());
    TEST_CHECK(byTriangles.triangleCount() == 2);
    TEST_CHECK(byTriangles.truncated);

    const Mesh byVertices = readStl(asciiStl(), MeshLimits{.maxVertices = 7, .maxTriangles = 100});
    TEST_CHECK(byVertices.triangleCount() == 2);
    TEST_CHECK(byVertices.vertexCount() == 6);
    TEST_CHECK(byVertices.truncated);
  }

  void testNonFinite() {
    std::vector<Triangle> triangles = kTetrahedron;
    triangles[0][1].y = std::numeric_limits<float>::quiet_NaN();
    triangles[1][2].z = std::numeric_limits<float>::infinity();
    const Mesh binary = readStl(binaryStl(triangles));
    TEST_CHECK(binary.error.empty());
    TEST_CHECK(binary.triangleCount() == 2);
    TEST_CHECK(binary.nonFiniteDropped);
    checkConsistent(binary);

    const std::string ascii = "solid x\nfacet normal 0 0 0\nouter loop\nvertex nan 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
                              "endloop\nendfacet\nfacet normal 0 0 0\nouter loop\nvertex 0 0 0\nvertex 1e999 0 0\n"
                              "vertex 0 1 0\nendloop\nendfacet\nfacet normal 0 0 0\nouter loop\nvertex 0 0 0\n"
                              "vertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid x\n";
    const Mesh asciiMesh = readStl(ascii);
    TEST_CHECK(asciiMesh.error.empty());
    TEST_CHECK(asciiMesh.triangleCount() == 1);
    TEST_CHECK(asciiMesh.nonFiniteDropped);
  }

  void testHostileCounts() {
    std::string bytes = binaryStl(kTetrahedron, "binary");
    bytes[80] = '\xFF';
    bytes[81] = '\xFF';
    bytes[82] = '\xFF';
    bytes[83] = '\xFF'; // 4294967295 triangles
    const Mesh huge = readStl(bytes);
    TEST_CHECK(!huge.error.empty());
    checkConsistent(huge);

    bytes = binaryStl(kTetrahedron, "binary");
    bytes[80] = 5; // one more triangle than the file holds
    const Mesh oneMore = readStl(bytes);
    TEST_CHECK(!oneMore.error.empty());
    checkConsistent(oneMore);
  }

  void testMalformed() {
    TEST_CHECK(!readStl("").error.empty());
    TEST_CHECK(!readStl("not a mesh").error.empty());
    const std::string twoCorners = "solid x\nfacet normal 0 0 0\nouter loop\nvertex 0 0 0\nvertex 1 0 0\n"
                                   "endloop\nendfacet\nendsolid x\n";
    TEST_CHECK(!readStl(twoCorners).error.empty());
    const std::string badNumber = "solid x\nfacet normal 0 0 0\nouter loop\nvertex 0 zero 0\nvertex 1 0 0\n"
                                  "vertex 0 1 0\nendloop\nendfacet\nendsolid x\n";
    TEST_CHECK(!readStl(badNumber).error.empty());
  }

  void testMegabyteLines() {
    const Mesh longName = readStl(asciiStl(std::string(1 << 20, 'n')));
    TEST_CHECK(longName.error.empty());
    TEST_CHECK(longName.triangleCount() == 4);

    const std::string longNumber = "solid x\nfacet normal 0 0 0\nouter loop\nvertex 0 "
        + std::string(1 << 20, '7')
        + " 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid x\n";
    const Mesh longMesh = readStl(longNumber);
    TEST_CHECK(longMesh.error.empty());
    TEST_CHECK(longMesh.triangleCount() == 0); // the coordinate overflows to infinity and is dropped
    TEST_CHECK(longMesh.nonFiniteDropped);
  }

  void testEveryTruncation() {
    for (const std::string& full : {binaryStl(kTetrahedron), binaryStl(kTetrahedron, "solid x")}) {
      for (std::size_t length = 0; length < full.size(); ++length) {
        const Mesh mesh = readStl(std::string_view(full).substr(0, length));
        checkConsistent(mesh);
        TEST_CHECK(mesh.triangleCount() == 0);
      }
    }
    const std::string ascii = asciiStl();
    const std::size_t lastLoopEnd = ascii.rfind("endloop") + std::string_view("endloop").size();
    for (std::size_t length = 0; length < ascii.size(); ++length) {
      const Mesh mesh = readStl(std::string_view(ascii).substr(0, length));
      checkConsistent(mesh);
      // A prefix that ends after the last "endloop" holds every facet; a shorter one loses at least one.
      TEST_CHECK(length >= lastLoopEnd ? mesh.triangleCount() == 4 : mesh.triangleCount() < 4);
    }
  }
} // namespace

int main() {
  testFlatNormalsAndBounds();
  testFinishMeshCompactsAndValidates();
  testBinary();
  testBinaryThatStartsWithSolid();
  testBinaryWithTrailingBytes();
  testSolidHeaderWithTrailingBytes();
  testAsciiWithoutLineFeeds();
  testAscii();
  testLimits();
  testNonFinite();
  testHostileCounts();
  testMalformed();
  testMegabyteLines();
  testEveryTruncation();
  return 0;
}
