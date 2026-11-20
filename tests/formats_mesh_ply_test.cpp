#include "formats/mesh.h"
#include "formats/mesh_ply.h"
#include "test_check.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
  using namespace luciole::formats;

  using Indices = std::vector<std::uint32_t>;

  const std::array<Vec3, 8> kCorners = {
      Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0},
      Vec3{0, 0, 1}, Vec3{1, 0, 1}, Vec3{1, 1, 1}, Vec3{0, 1, 1},
  };
  const std::array<std::array<std::uint32_t, 4>, 6> kSides = {{
      {0, 3, 2, 1},
      {4, 5, 6, 7},
      {0, 1, 5, 4},
      {1, 2, 6, 5},
      {2, 3, 7, 6},
      {3, 0, 4, 7},
  }};

  template <typename T> void put(std::string& out, T value, bool bigEndian) {
    using Bits = std::conditional_t<
        sizeof(T) == 1, std::uint8_t,
        std::conditional_t<
            sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
    const auto bits = std::bit_cast<Bits>(value);
    for (std::size_t k = 0; k < sizeof(T); ++k) {
      const std::size_t shift = 8 * (bigEndian ? sizeof(T) - 1 - k : k);
      out.push_back(static_cast<char>((bits >> shift) & 0xFFU));
    }
  }

  std::string asciiCube(std::string_view comment = "made by hand") {
    std::string out = "ply\nformat ascii 1.0\ncomment "
        + std::string(comment)
        + "\nobj_info none\n"
          "element vertex 8\nproperty float x\nproperty float y\nproperty float z\n"
          "element face 6\nproperty list uchar int vertex_indices\nend_header\n";
    for (const Vec3& corner : kCorners) {
      out += std::to_string(corner.x) + " " + std::to_string(corner.y) + " " + std::to_string(corner.z) + "\n";
    }
    for (const auto& side : kSides) {
      out += "4 "
          + std::to_string(side[0])
          + " "
          + std::to_string(side[1])
          + " "
          + std::to_string(side[2])
          + " "
          + std::to_string(side[3])
          + "\n";
    }
    return out;
  }

  // A cube whose vertices carry colors, normals equal to their positions and a list; an edge element and a
  // material element with a list come before the faces, whose indices sit between other properties.
  std::string binaryCube(bool bigEndian) {
    std::string out = std::string("ply\nformat ")
        + (bigEndian ? "binary_big_endian" : "binary_little_endian")
        + " 1.0\nelement vertex 8\nproperty double x\nproperty float32 y\nproperty float z\n"
          "property uchar red\nproperty uint8 green\nproperty uchar blue\n"
          "property float nx\nproperty float ny\nproperty float nz\nproperty list uchar int16 tags\n"
          "property int id\nproperty ushort weight\nproperty char offset\n"
          "element edge 2\nproperty int vertex1\nproperty int vertex2\n"
          "element material 1\nproperty list ushort char name\n"
          "element face 6\nproperty uchar flags\nproperty list uint8 uint32 vertex_indices\n"
          "property float quality\nend_header\n";
    for (const Vec3& corner : kCorners) {
      put(out, double{corner.x}, bigEndian);
      put(out, corner.y, bigEndian);
      put(out, corner.z, bigEndian);
      put(out, std::uint8_t{200}, bigEndian);
      put(out, std::uint8_t{100}, bigEndian);
      put(out, std::uint8_t{50}, bigEndian);
      put(out, corner.x, bigEndian);
      put(out, corner.y, bigEndian);
      put(out, corner.z, bigEndian);
      put(out, std::uint8_t{2}, bigEndian);
      put(out, std::int16_t{-7}, bigEndian);
      put(out, std::int16_t{300}, bigEndian);
      put(out, std::int32_t{-70000}, bigEndian);
      put(out, std::uint16_t{60000}, bigEndian);
      put(out, std::int8_t{-3}, bigEndian);
    }
    for (std::int32_t edge = 0; edge < 2; ++edge) {
      put(out, edge, bigEndian);
      put(out, edge + 1, bigEndian);
    }
    put(out, std::uint16_t{3}, bigEndian);
    out += "red";
    for (const auto& side : kSides) {
      put(out, std::uint8_t{1}, bigEndian);
      put(out, std::uint8_t{4}, bigEndian);
      for (const std::uint32_t index : side) {
        put(out, index, bigEndian);
      }
      put(out, 0.5F, bigEndian);
    }
    return out;
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

  void checkCube(const Mesh& mesh) {
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.vertexCount() == 8);
    TEST_CHECK(mesh.triangleCount() == 12);
    TEST_CHECK(!mesh.truncated);
    TEST_CHECK((mesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{1, 1, 1}}));
    TEST_CHECK((Indices(mesh.indices.begin(), mesh.indices.begin() + 6) == Indices{0, 3, 2, 0, 2, 1}));
    TEST_CHECK((mesh.positions[6] == Vec3{1, 1, 1}));
    checkConsistent(mesh);
  }

  void testAscii() {
    const Mesh mesh = readPly(asciiCube());
    checkCube(mesh);
    TEST_CHECK(mesh.normalsMissing);
  }

  void testAsciiWithCrLfHeader() {
    std::string bytes = asciiCube();
    const std::size_t bodyStart = bytes.find("end_header\n") + std::string_view("end_header\n").size();
    std::string crlf;
    for (std::size_t k = 0; k < bytes.size(); ++k) {
      if (bytes[k] == '\n' && k < bodyStart) {
        crlf += '\r';
      }
      crlf += bytes[k];
    }
    checkCube(readPly(crlf));
  }

  void testBinaryBothByteOrders() {
    for (const bool bigEndian : {false, true}) {
      const Mesh mesh = readPly(binaryCube(bigEndian));
      checkCube(mesh);
      TEST_CHECK(!mesh.normalsMissing);
      for (std::size_t k = 0; k < mesh.vertexCount(); ++k) {
        TEST_CHECK((mesh.normals[k] == mesh.positions[k]));
      }
    }
  }

  void testLimits() {
    const Mesh byVertices = readPly(asciiCube(), MeshLimits{.maxVertices = 4, .maxTriangles = 100});
    TEST_CHECK(byVertices.error.empty());
    TEST_CHECK(byVertices.truncated);
    TEST_CHECK(byVertices.triangleCount() == 2);
    TEST_CHECK(byVertices.vertexCount() == 4);

    const Mesh byTriangles = readPly(binaryCube(false), MeshLimits{.maxVertices = 100, .maxTriangles = 5});
    TEST_CHECK(byTriangles.error.empty());
    TEST_CHECK(byTriangles.truncated);
    TEST_CHECK(byTriangles.triangleCount() == 5);
  }

  std::string triangleHeader(std::string_view vertexCount, std::string_view faceCount) {
    return "ply\nformat ascii 1.0\nelement vertex "
        + std::string(vertexCount)
        + "\nproperty float x\nproperty float y\nproperty float z\nelement face "
        + std::string(faceCount)
        + "\nproperty list char int vertex_indices\nend_header\n";
  }

  void testShortFacesAndPointClouds() {
    const Mesh shortFace = readPly(triangleHeader("3", "2") + "0 0 0\n1 0 0\n0 1 0\n2 0 1\n3 0 1 2\n");
    TEST_CHECK(shortFace.error.empty());
    TEST_CHECK(shortFace.triangleCount() == 1);

    const Mesh points = readPly(
        "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\n"
        "element nothing 4000000000\nend_header\n1 2 3\n"
    );
    TEST_CHECK(points.error.empty());
    TEST_CHECK(points.triangleCount() == 0);
    TEST_CHECK(points.unusedVertices == 1);
    TEST_CHECK(shortFace.unusedVertices == 0);
  }

  void testNonFinite() {
    const Mesh ascii = readPly(triangleHeader("4", "2") + "nan 0 0\n1 0 0\n0 1 0\n0 0 1\n3 0 1 2\n3 1 2 3\n");
    TEST_CHECK(ascii.error.empty());
    TEST_CHECK(ascii.nonFiniteDropped);
    TEST_CHECK(ascii.triangleCount() == 1);
    TEST_CHECK(ascii.vertexCount() == 3);

    std::string binary = "ply\nformat binary_little_endian 1.0\nelement vertex 3\nproperty float x\n"
                         "property float y\nproperty float z\nelement face 1\n"
                         "property list uchar uint vertex_indices\nend_header\n";
    for (const float value : {0.0F, std::numeric_limits<float>::infinity(), 1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F}) {
      put(binary, value, false);
    }
    put(binary, std::uint8_t{3}, false);
    for (const std::uint32_t index : {0U, 1U, 2U}) {
      put(binary, index, false);
    }
    const Mesh binaryMesh = readPly(binary);
    TEST_CHECK(binaryMesh.error.empty());
    TEST_CHECK(binaryMesh.nonFiniteDropped);
    TEST_CHECK(binaryMesh.triangleCount() == 0);
  }

  void testHostileCounts() {
    std::string bytes = binaryCube(false);
    const std::size_t at = bytes.find("vertex 8");
    bytes.replace(at, 8, "vertex 4294967295");
    TEST_CHECK(!readPly(bytes).error.empty());

    TEST_CHECK(!readPly(triangleHeader("3", "4294967295") + "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n").error.empty());
    TEST_CHECK(!readPly(triangleHeader("3", "99999999999999999999999") + "0 0 0\n").error.empty());
    TEST_CHECK(!readPly(triangleHeader("3", "1") + "0 0 0\n1 0 0\n0 1 0\n3 0 1 3\n").error.empty());
    TEST_CHECK(!readPly(triangleHeader("3", "1") + "0 0 0\n1 0 0\n0 1 0\n3 0 1 -1\n").error.empty());
    TEST_CHECK(!readPly(triangleHeader("3", "1") + "0 0 0\n1 0 0\n0 1 0\n3 0 1 1.5\n").error.empty());
    TEST_CHECK(!readPly(triangleHeader("3", "1") + "0 0 0\n1 0 0\n0 1 0\n-1 0 1 2\n").error.empty());
    TEST_CHECK(!readPly(triangleHeader("3", "1") + "0 0 0\n1 0 0\n0 1 0\n100 0 1 2\n").error.empty());

    // A binary list that claims four billion items.
    std::string list = "ply\nformat binary_big_endian 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
                       "property float z\nelement face 1\nproperty list uint uint vertex_indices\nend_header\n";
    for (int k = 0; k < 9; ++k) {
      put(list, 0.0F, true);
    }
    put(list, std::uint32_t{0xFFFFFFFFU}, true);
    put(list, std::uint32_t{0}, true);
    TEST_CHECK(!readPly(list).error.empty());
  }

  void testMalformedHeaders() {
    const std::string body = "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";
    const std::string good = triangleHeader("3", "1");
    TEST_CHECK(readPly(good + body).error.empty());
    TEST_CHECK(!readPly("").error.empty());
    TEST_CHECK(!readPly("plx\n" + good.substr(4) + body).error.empty());
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"end_header\n", "end_heade\n"},
             {"format ascii 1.0", "format text 1.0"},
             {"property float x", "property quad x"},
             {"property float x", "property float w"},
             {"property list char int", "property list float int"},
             {"element vertex 3\n", "property float q\nelement vertex 3\n"},
             {"element face 1\n", "element face\n"},
             {"format ascii 1.0\n", ""},
             {"vertex_indices", "corners"},
         }) {
      std::string bytes = good + body;
      bytes.replace(bytes.find(from), from.size(), to);
      const Mesh mesh = readPly(bytes);
      TEST_CHECK(!mesh.error.empty());
      checkConsistent(mesh);
    }
    std::string many = "ply\nformat ascii 1.0\nelement vertex 1\n";
    for (int k = 0; k < 5000; ++k) {
      many += "property float p" + std::to_string(k) + "\n";
    }
    TEST_CHECK(!readPly(many + "end_header\n").error.empty());
    std::string elements = "ply\nformat ascii 1.0\n";
    for (std::size_t k = 0; k <= kMaxPlyElements; ++k) {
      elements += "element e" + std::to_string(k) + " 0\n";
    }
    TEST_CHECK(!readPly(elements + "end_header\n").error.empty());
  }

  void testMegabyteLines() {
    checkCube(readPly(asciiCube(std::string(1 << 20, 'c'))));

    const std::string header = "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
                               "property float z\nproperty double confidence\nelement face 1\n"
                               "property list uchar int vertex_indices\nend_header\n";
    const Mesh mesh = readPly(header + "0 0 0 " + std::string(1 << 20, '7') + "\n1 0 0 1\n0 1 0 1\n3 0 1 2\n");
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 1);
  }

  void testEveryTruncation() {
    for (const std::string& full : {asciiCube(), binaryCube(false), binaryCube(true)}) {
      for (std::size_t length = 0; length < full.size(); ++length) {
        const Mesh mesh = readPly(std::string_view(full).substr(0, length));
        checkConsistent(mesh);
        TEST_CHECK(mesh.triangleCount() <= 12);
      }
    }
    // Binary prefixes always end inside a value.
    const std::string binary = binaryCube(false);
    for (std::size_t length = 0; length < binary.size(); ++length) {
      TEST_CHECK(!readPly(std::string_view(binary).substr(0, length)).error.empty());
    }
  }
} // namespace

int main() {
  testAscii();
  testAsciiWithCrLfHeader();
  testBinaryBothByteOrders();
  testLimits();
  testShortFacesAndPointClouds();
  testNonFinite();
  testHostileCounts();
  testMalformedHeaders();
  testMegabyteLines();
  testEveryTruncation();
  return 0;
}
