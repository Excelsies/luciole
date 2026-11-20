#include "formats/mesh.h"
#include "formats/mesh_gltf.h"
#include "test_check.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace luciole::formats;

  void putU16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>(value >> 8U));
  }

  void putU32(std::string& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
  }

  void putF32(std::string& out, float value) { putU32(out, std::bit_cast<std::uint32_t>(value)); }

  std::string base64(std::string_view bytes) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t k = 0; k < bytes.size(); k += 3) {
      std::uint32_t group = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[k])) << 16U;
      if (k + 1 < bytes.size()) {
        group |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[k + 1])) << 8U;
      }
      if (k + 2 < bytes.size()) {
        group |= static_cast<unsigned char>(bytes[k + 2]);
      }
      out += kAlphabet[(group >> 18U) & 63U];
      out += kAlphabet[(group >> 12U) & 63U];
      out += k + 1 < bytes.size() ? kAlphabet[(group >> 6U) & 63U] : '=';
      out += k + 2 < bytes.size() ? kAlphabet[group & 63U] : '=';
    }
    return out;
  }

  // Four float corners of a tetrahedron, then twelve 16-bit indices: 72 bytes.
  std::string tetraBuffer(float firstX = 0.0F) {
    std::string out;
    for (const float value : {firstX, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F}) {
      putF32(out, value);
    }
    for (const int index : {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3}) {
      putU16(out, static_cast<std::uint16_t>(index));
    }
    return out;
  }

  constexpr std::string_view kTetraLayout =
      R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
      R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":24}],)"
      R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)"
      R"({"bufferView":1,"componentType":5123,"count":12,"type":"SCALAR"}])";

  constexpr std::string_view kOneNode = R"("scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}])";

  std::string dataUri(std::string_view buffer = tetraBuffer()) {
    return R"(,"uri":"data:application/octet-stream;base64,)" + base64(buffer) + "\"";
  }

  // A glTF document around the tetrahedron layout; `buffer` adds members to the one buffer.
  std::string tetraGltf(std::string_view scene = kOneNode, std::string_view buffer = dataUri()) {
    return R"({"asset":{"version":"2.0"},)"
        + std::string(scene)
        + ","
        + std::string(kTetraLayout)
        + R"(,"buffers":[{"byteLength":72)"
        + std::string(buffer)
        + "}]}";
  }

  std::string glb(std::string json, std::string binary, std::uint32_t version = 2) {
    json.resize((json.size() + 3) / 4 * 4, ' ');
    binary.resize((binary.size() + 3) / 4 * 4, '\0');
    std::string out = "glTF";
    putU32(out, version);
    putU32(out, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + binary.size()));
    putU32(out, static_cast<std::uint32_t>(json.size()));
    out += "JSON";
    out += json;
    putU32(out, static_cast<std::uint32_t>(binary.size()));
    out += std::string("BIN\0", 4);
    out += binary;
    return out;
  }

  std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    TEST_CHECK(at != std::string::npos);
    return text.replace(at, from.size(), to);
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

  void checkBounds(const Mesh& mesh, Vec3 min, Vec3 max) {
    TEST_NEAR(mesh.bounds.min.x, min.x, 1e-5F);
    TEST_NEAR(mesh.bounds.min.y, min.y, 1e-5F);
    TEST_NEAR(mesh.bounds.min.z, min.z, 1e-5F);
    TEST_NEAR(mesh.bounds.max.x, max.x, 1e-5F);
    TEST_NEAR(mesh.bounds.max.y, max.y, 1e-5F);
    TEST_NEAR(mesh.bounds.max.z, max.z, 1e-5F);
  }

  Mesh readText(const std::string& text, const MeshLimits& limits = {}) { return readGltf(text, {}, limits); }

  void testGlbTwoNodesAndTransform() {
    const std::string scene = R"("scene":0,"scenes":[{"nodes":[0,1]}],"nodes":[{"mesh":0},)"
                              R"({"mesh":0,"translation":[10,0,0],"children":[2]},{"mesh":0,"scale":[2,2,2]}])";
    const Mesh mesh = readGltf(glb(tetraGltf(scene, ""), tetraBuffer()), {});
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.vertexCount() == 12);
    TEST_CHECK(mesh.triangleCount() == 12);
    TEST_CHECK(mesh.normalsMissing);
    TEST_CHECK(!mesh.truncated);
    TEST_CHECK((mesh.skipped == SkippedParts{}));
    TEST_CHECK((mesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{12, 2, 2}}));
    TEST_CHECK((mesh.positions[5] == Vec3{11, 0, 0}));
    checkConsistent(mesh);
  }

  void testRotationMatrixAndMirror() {
    const Mesh rotated = readText(tetraGltf(R"("nodes":[{"mesh":0,"rotation":[0,0,0.70710678,0.70710678]}])"));
    TEST_CHECK(rotated.error.empty());
    checkBounds(rotated, Vec3{-1, 0, 0}, Vec3{0, 1, 1});

    const Mesh moved = readText(tetraGltf(R"("nodes":[{"mesh":0,"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,5,6,7,1]}])"));
    TEST_CHECK((moved.bounds == Bounds{.min = Vec3{5, 6, 7}, .max = Vec3{6, 7, 8}}));

    // A mirror keeps the outside facing out: the bottom side still faces down.
    const Mesh plain = readText(tetraGltf());
    const Mesh mirrored = readText(tetraGltf(R"("nodes":[{"mesh":0,"scale":[-1,1,1]}])"));
    TEST_CHECK((flatNormals(plain)[0] == Vec3{0, 0, -1}));
    TEST_CHECK((flatNormals(mirrored)[0] == Vec3{0, 0, -1}));
  }

  void testNoScenesUsesParentlessNodes() {
    const Mesh mesh = readText(tetraGltf(R"("nodes":[{"children":[1]},{"mesh":0,"translation":[0,0,3]}])"));
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 4);
    TEST_CHECK((mesh.bounds.max == Vec3{1, 1, 4}));
    const Mesh empty = readText(R"({"asset":{"version":"2.0"}})");
    TEST_CHECK(empty.error.empty());
    TEST_CHECK(empty.triangleCount() == 0);
  }

  void testDataUris() {
    const Mesh octet = readText(tetraGltf());
    TEST_CHECK(octet.error.empty());
    TEST_CHECK(octet.vertexCount() == 4);
    TEST_CHECK(octet.triangleCount() == 4);
    TEST_CHECK((octet.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{1, 1, 1}}));
    const std::string gltfBuffer = R"(,"uri":"data:application/gltf-buffer;base64,)" + base64(tetraBuffer()) + "\"";
    TEST_CHECK(readText(tetraGltf(kOneNode, gltfBuffer)).triangleCount() == 4);
    for (const std::string_view bad :
         {R"(,"uri":"data:application/octet-stream,abc")", R"(,"uri":"data:application/octet-stream;base64,AAA*")",
          R"(,"uri":"data:application/octet-stream;base64,A")"}) {
      TEST_CHECK(!readText(tetraGltf(kOneNode, bad)).error.empty());
    }
  }

  void testExternalBuffers() {
    std::vector<std::string> asked;
    const BufferLoader loader = [&](std::string_view path) -> std::optional<std::string> {
      asked.emplace_back(path);
      return tetraBuffer();
    };
    const std::string twoNodes = R"("scenes":[{"nodes":[0,1]}],"nodes":[{"mesh":0},{"mesh":0}])";
    const Mesh mesh = readGltf(tetraGltf(twoNodes, R"(,"uri":"parts/tetra%20data.bin")"), loader);
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 8);
    TEST_CHECK((asked == std::vector<std::string>{"parts/tetra data.bin"})); // read once, percent-decoded

    for (const std::string_view uri :
         {"/abs/tetra.bin", "../tetra.bin", "parts/../../tetra.bin", "%2e%2e/tetra.bin", "parts/%2E%2E", "..",
          "http://example.com/tetra.bin", "file:///tetra.bin", R"(C:\\tetra.bin)", R"(parts\\..\\tetra.bin)",
          R"(\\\\server\\tetra.bin)", "", "bad%zzescape.bin", "nul%00.bin"}) {
      asked.clear();
      const Mesh refused = readGltf(tetraGltf(kOneNode, R"(,"uri":")" + std::string(uri) + "\""), loader);
      TEST_CHECK(!refused.error.empty());
      TEST_CHECK(asked.empty());
    }

    const BufferLoader missing = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    TEST_CHECK(!readGltf(tetraGltf(kOneNode, R"(,"uri":"tetra.bin")"), missing).error.empty());
    const BufferLoader shortFile = [](std::string_view) -> std::optional<std::string> { return std::string(10, 0); };
    TEST_CHECK(!readGltf(tetraGltf(kOneNode, R"(,"uri":"tetra.bin")"), shortFile).error.empty());
    TEST_CHECK(!readGltf(tetraGltf(kOneNode, R"(,"uri":"tetra.bin")"), {}).error.empty());
  }

  void testNormalsStrideAndComponentTypes() {
    // One triangle, positions and normals interleaved with a 24-byte stride, 8-bit indices.
    std::string buffer;
    const float half = std::sqrt(0.5F);
    for (const Vec3& corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
      for (const float value : {corner.x, corner.y, corner.z, half, half, 0.0F}) {
        putF32(buffer, value);
      }
    }
    buffer += std::string("\0\1\2\0", 4);
    const std::string text =
        R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0,"scale":[2,1,1]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2,"mode":4}]}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":72,"byteStride":24},{"buffer":0,"byteOffset":72,"byteLength":3}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5121,"count":3,"type":"SCALAR"}],)"
        R"("buffers":[{"byteLength":76)"
        + dataUri(buffer)
        + "}]}";
    const Mesh mesh = readText(text);
    TEST_CHECK(mesh.error.empty());
    TEST_CHECK(mesh.triangleCount() == 1);
    TEST_CHECK(!mesh.normalsMissing);
    TEST_CHECK((mesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{2, 1, 0}}));
    TEST_NEAR(mesh.normals[0].x, 0.4472136F, 1e-5F); // the inverse transpose of the scale, normalized
    TEST_NEAR(mesh.normals[0].y, 0.8944272F, 1e-5F);

    // Normalized 8-bit positions padded to a 4-byte stride, 32-bit indices, no normals.
    std::string quantized = std::string("\0\0\0\0\xFF\0\0\0\0\xFF\0\0", 12);
    for (const std::uint32_t index : {0U, 1U, 2U}) {
      putU32(quantized, index);
    }
    const std::string small =
        R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":12,"byteStride":4},{"buffer":0,"byteOffset":12,"byteLength":12}],)"
        R"("accessors":[{"bufferView":0,"componentType":5121,"normalized":true,"count":3,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}],)"
        R"("buffers":[{"byteLength":24)"
        + dataUri(quantized)
        + "}]}";
    const Mesh smallMesh = readText(small);
    TEST_CHECK(smallMesh.error.empty());
    TEST_CHECK((smallMesh.bounds == Bounds{.min = Vec3{0, 0, 0}, .max = Vec3{1, 1, 0}}));
    TEST_CHECK(smallMesh.normalsMissing);
  }

  // One triangle whose positions are signed normalized components of `type` (5120 or 5122), no indices.
  std::string signedTriangle(std::size_t type) {
    std::string buffer;
    const bool bytes = type == 5120;
    for (const int value : {bytes ? -128 : -32768, 0, 0, bytes ? 127 : 32767, 0, 0, 0, bytes ? 127 : 32767, 0}) {
      if (bytes) {
        buffer.push_back(static_cast<char>(value));
      } else {
        putU16(buffer, static_cast<std::uint16_t>(value));
      }
    }
    return R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],)"
           R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
           R"("bufferViews":[{"buffer":0,"byteLength":)"
        + std::to_string(buffer.size())
        + R"(}],"accessors":[{"bufferView":0,"componentType":)"
        + std::to_string(type)
        + R"(,"normalized":true,"count":3,"type":"VEC3"}],"buffers":[{"byteLength":)"
        + std::to_string(buffer.size())
        + dataUri(buffer)
        + "}]}";
  }

  void testSignedNormalizedAndViewlessAccessors() {
    for (const std::size_t type : {5120U, 5122U}) {
      const Mesh mesh = readText(signedTriangle(type));
      TEST_CHECK(mesh.error.empty());
      TEST_CHECK(mesh.triangleCount() == 1);
      TEST_CHECK((mesh.bounds == Bounds{.min = Vec3{-1, 0, 0}, .max = Vec3{1, 1, 0}})); // the lowest value clamps
    }
    // An accessor without a buffer view reads as zeros.
    const std::string viewless = replaced(
        tetraGltf(), R"({"bufferView":0,"componentType":5126,"count":4)",
        R"({"componentType":5126,"count":4,"sparse":{"count":1})"
    );
    const Mesh zeros = readText(viewless);
    TEST_CHECK(zeros.error.empty());
    TEST_CHECK(zeros.skipped.sparse);
    TEST_CHECK((zeros.bounds == Bounds{}));
    TEST_CHECK(zeros.triangleCount() == 4);
  }

  void testSkippedParts() {
    const Mesh meshopt =
        readText(replaced(tetraGltf(), R"("asset")", R"("extensionsRequired":["EXT_meshopt_compression"],"asset")"));
    TEST_CHECK(meshopt.error.empty());
    TEST_CHECK(meshopt.skipped.compressed);
    TEST_CHECK(meshopt.triangleCount() == 0);

    const std::string sparse =
        replaced(tetraGltf(), R"("count":4,"type":"VEC3")", R"("count":4,"type":"VEC3","sparse":{"count":1})");
    const Mesh sparseMesh = readText(sparse);
    TEST_CHECK(sparseMesh.skipped.sparse);
    TEST_CHECK(sparseMesh.triangleCount() == 4);

    const Mesh skins = readText(
        replaced(tetraGltf(), R"("nodes":[{"mesh":0})", R"("skins":[{"joints":[0]}],"nodes":[{"mesh":0,"skin":0})")
    );
    TEST_CHECK(skins.skipped.skins);
    TEST_CHECK(skins.triangleCount() == 4);

    const Mesh morph = readText(replaced(tetraGltf(), R"("indices":1)", R"("indices":1,"targets":[{"POSITION":0}])"));
    TEST_CHECK(morph.skipped.morphTargets);
    TEST_CHECK(morph.triangleCount() == 4);

    const Mesh material = readText(replaced(tetraGltf(), R"("indices":1)", R"("indices":1,"material":0)"));
    TEST_CHECK(material.skipped.materials);

    const Mesh used =
        readText(replaced(tetraGltf(), R"("asset")", R"("extensionsUsed":["KHR_materials_unlit"],"asset")"));
    TEST_CHECK(used.skipped.extensions);
    TEST_CHECK(!used.skipped.compressed);
    TEST_CHECK(used.triangleCount() == 4);

    std::string draco =
        replaced(tetraGltf(), R"("asset")", R"("extensionsRequired":["KHR_draco_mesh_compression"],"asset")");
    draco = replaced(
        draco, R"("indices":1)", R"("indices":1,"extensions":{"KHR_draco_mesh_compression":{"bufferView":0}})"
    );
    const Mesh dracoMesh = readText(draco);
    TEST_CHECK(dracoMesh.error.empty());
    TEST_CHECK(dracoMesh.skipped.compressed);
    TEST_CHECK(dracoMesh.skipped.extensions);
    TEST_CHECK(dracoMesh.triangleCount() == 0);

    const Mesh lines = readText(replaced(tetraGltf(), R"("indices":1)", R"("indices":1,"mode":1)"));
    TEST_CHECK(lines.error.empty());
    TEST_CHECK(lines.skipped.otherModes);
    TEST_CHECK(lines.triangleCount() == 0);
  }

  void testHostileStructure() {
    const std::string base = tetraGltf();
    const std::vector<std::pair<std::string_view, std::string>> cases = {
        {R"("componentType":5126,"count":4)", R"("byteOffset":40,"componentType":5126,"count":4)"},
        {R"("componentType":5126,"count":4)", R"("componentType":5126,"count":4294967295)"},
        {R"("componentType":5126,"count":4)", R"("byteOffset":1e30,"componentType":5126,"count":4)"},
        {R"("componentType":5126,"count":4)", R"("componentType":5126,"count":-4)"},
        {R"("componentType":5126,"count":4)", R"("componentType":5124,"count":4)"},
        {R"("count":4,"type":"VEC3")", R"("count":4,"type":"VEC2")"},
        {R"("byteOffset":0,"byteLength":48)", R"("byteOffset":0,"byteLength":48,"byteStride":0)"},
        {R"("byteOffset":0,"byteLength":48)", R"("byteOffset":0,"byteLength":48,"byteStride":8)"},
        {R"("byteOffset":0,"byteLength":48)", R"("byteOffset":0,"byteLength":4800)"},
        {R"("byteOffset":48,"byteLength":24)", R"("byteOffset":18446744073709551615,"byteLength":24)"},
        {R"("buffer":0,"byteOffset":0)", R"("buffer":2,"byteOffset":0)"},
        {R"("bufferView":1,)", R"("bufferView":7,)"},
        {R"("POSITION":0)", R"("POSITION":9)"},
        {R"("nodes":[{"mesh":0}])", R"("nodes":[{"mesh":3}])"},
        {R"("nodes":[{"mesh":0}])", R"("nodes":[{"mesh":0,"children":[0]}])"},
        {R"("nodes":[{"mesh":0}])", R"("nodes":[{"mesh":0,"children":[5]}])"},
        {R"("nodes":[{"mesh":0}])", R"("nodes":[{"mesh":0,"children":[1]},{"children":[0]}])"},
        {std::string_view(kOneNode), R"("nodes":[{"mesh":0,"children":[1]},{"children":[0]}])"}, // no root
        {R"("scenes":[{"nodes":[0]}])", R"("scene":4,"scenes":[{"nodes":[0]}])"},
        {R"("version":"2.0")", R"("version":"1.0")"},
        {R"("asset":{"version":"2.0"},)", ""},
        {R"("byteLength":72)", R"("byteLength":100)"},
        {R"("asset")", R"("extras":)" + std::string(10'000, '[') + std::string(10'000, ']') + R"(,"asset")"},
        {R"("POSITION":0)", R"("POSITION":0,"NORMAL":1)"},
        {R"("indices":1)", R"("indices":1,"mode":"triangles")"},
        {R"("nodes":[{"mesh":0}])", R"("nodes":[{"mesh":0,"translation":[1,2]}])"},
        {R"("nodes":[{"mesh":0}])", R"("nodes":[{"mesh":0,"matrix":[1,0,0,1]}])"},
    };
    for (const auto& [from, to] : cases) {
      const Mesh mesh = readText(replaced(base, from, to));
      TEST_CHECK(!mesh.error.empty());
      checkConsistent(mesh);
    }
    const std::string otherCount = replaced(
        replaced(base, R"("POSITION":0)", R"("POSITION":0,"NORMAL":2)"), R"("type":"SCALAR"}])",
        R"("type":"SCALAR"},{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}])"
    );
    TEST_CHECK(!readText(otherCount).error.empty());
    TEST_CHECK(
        readText(replaced(otherCount, R"("count":3,"type":"VEC3")", R"("count":4,"type":"VEC3")")).error.empty()
    );
    std::string outOfRange = tetraBuffer();
    outOfRange[48 + 22] = 4; // the last index names a fifth vertex
    TEST_CHECK(!readText(tetraGltf(kOneNode, dataUri(outOfRange))).error.empty());
    TEST_CHECK(!readText("[]").error.empty());
    TEST_CHECK(!readText("").error.empty());
  }

  void testNodeDepth() {
    const auto chain = [](std::size_t length) {
      std::string nodes = R"("nodes":[)";
      for (std::size_t k = 0; k + 1 < length; ++k) {
        nodes += R"({"children":[)" + std::to_string(k + 1) + "]},";
      }
      return tetraGltf(R"("scenes":[{"nodes":[0]}],)" + nodes + R"({"mesh":0}])");
    };
    TEST_CHECK(readText(chain(kMaxGltfNodeDepth)).triangleCount() == 4);
    TEST_CHECK(!readText(chain(kMaxGltfNodeDepth + 1)).error.empty());
    TEST_CHECK(!readText(chain(10'000)).error.empty());
  }

  // `count` copies of `item`, separated by commas.
  std::string joined(std::string_view item, std::size_t count) {
    std::string out;
    for (std::size_t k = 0; k < count; ++k) {
      out += k == 0 ? "" : ",";
      out += item;
    }
    return out;
  }

  Mesh readWithinASecond(const std::string& text) {
    const std::clock_t start = std::clock();
    Mesh mesh = readText(text);
    TEST_CHECK(static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC < 1.0);
    return mesh;
  }

  // Reading time grows with the file, not with nodes times the primitives of the mesh they share, nor with
  // nodes times the root members listed before "nodes".
  void testLinearTime() {
    const std::string nodes = R"("nodes":[)" + joined(R"({"mesh":0})", 10'000) + "]";
    const std::string emptyPrimitives = R"("primitives":[)" + joined("{}", 30'000) + ",";
    const Mesh shared = readWithinASecond(replaced(tetraGltf(nodes), R"("primitives":[)", emptyPrimitives));
    TEST_CHECK(shared.error.empty());
    TEST_CHECK(shared.triangleCount() == 40'000);
    TEST_CHECK(shared.vertexCount() == 40'000);

    std::string padded = R"({"asset":{"version":"2.0"})";
    for (std::size_t k = 0; k < 100'000; ++k) {
      padded += std::format(R"(,"k{}":0)", k);
    }
    TEST_CHECK(readWithinASecond(padded + R"(,"nodes":[)" + joined("{}", 100'000) + "]}").error.empty());
  }

  void testNonFinite() {
    const Mesh nan = readText(tetraGltf(kOneNode, dataUri(tetraBuffer(std::numeric_limits<float>::quiet_NaN()))));
    TEST_CHECK(nan.error.empty());
    TEST_CHECK(nan.nonFiniteDropped);
    TEST_CHECK(nan.triangleCount() == 1); // only the side without the first corner
    const Mesh huge = readText(tetraGltf(R"("nodes":[{"mesh":0,"translation":[1e300,0,0]}])"));
    TEST_CHECK(huge.error.empty());
    TEST_CHECK(huge.nonFiniteDropped);
    TEST_CHECK(huge.triangleCount() == 0);
  }

  void testLimits() {
    const std::string twoNodes = R"("scenes":[{"nodes":[0,1]}],"nodes":[{"mesh":0},{"mesh":0}])";
    const Mesh byVertices = readText(tetraGltf(twoNodes), MeshLimits{.maxVertices = 6, .maxTriangles = 100});
    TEST_CHECK(byVertices.error.empty());
    TEST_CHECK(byVertices.truncated);
    TEST_CHECK(byVertices.triangleCount() == 4);
    const Mesh byTriangles = readText(tetraGltf(twoNodes), MeshLimits{.maxVertices = 100, .maxTriangles = 6});
    TEST_CHECK(byTriangles.truncated);
    TEST_CHECK(byTriangles.triangleCount() == 6);
  }

  void testGlbContainerErrors() {
    const std::string json = tetraGltf(kOneNode, "");
    const std::string good = glb(json, tetraBuffer());
    TEST_CHECK(readGltf(good, {}).triangleCount() == 4);
    TEST_CHECK(!readGltf(glb(json, tetraBuffer(), 1), {}).error.empty());
    std::string longer = good;
    longer[8] = static_cast<char>(longer[8] + 4); // the total length claims four bytes more than the file
    TEST_CHECK(!readGltf(longer, {}).error.empty());
    std::string chunkPastEnd = good;
    chunkPastEnd[15] = '\x7F'; // the JSON chunk claims two gigabytes
    TEST_CHECK(!readGltf(chunkPastEnd, {}).error.empty());
    std::string binFirst = good;
    binFirst.replace(16, 4, std::string("BIN\0", 4));
    TEST_CHECK(!readGltf(binFirst, {}).error.empty());
    std::string binaryPastEnd = good;
    binaryPastEnd[20 + ((json.size() + 3) / 4 * 4)] = '\x7F'; // the binary chunk claims more than is left
    TEST_CHECK(!readGltf(binaryPastEnd, {}).error.empty());
    std::string noBinary = good.substr(0, 20 + ((json.size() + 3) / 4 * 4));
    const auto size = static_cast<std::uint32_t>(noBinary.size());
    for (std::size_t k = 0; k < 4; ++k) {
      noBinary[8 + k] = static_cast<char>((size >> (8 * k)) & 0xFFU);
    }
    TEST_CHECK(!readGltf(noBinary, {}).error.empty());
  }

  void testEveryTruncation() {
    for (const std::string& full : {glb(tetraGltf(kOneNode, ""), tetraBuffer()), tetraGltf()}) {
      for (std::size_t length = 0; length < full.size(); ++length) {
        const Mesh mesh = readGltf(std::string_view(full).substr(0, length), {});
        TEST_CHECK(!mesh.error.empty());
        checkConsistent(mesh);
      }
    }
  }
} // namespace

int main() {
  testGlbTwoNodesAndTransform();
  testRotationMatrixAndMirror();
  testNoScenesUsesParentlessNodes();
  testDataUris();
  testExternalBuffers();
  testNormalsStrideAndComponentTypes();
  testSignedNormalizedAndViewlessAccessors();
  testSkippedParts();
  testHostileStructure();
  testNodeDepth();
  testLinearTime();
  testNonFinite();
  testLimits();
  testGlbContainerErrors();
  testEveryTruncation();
  return 0;
}
