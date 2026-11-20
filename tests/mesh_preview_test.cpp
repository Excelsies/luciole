#include "formats/mesh.h"
#include "formats/mesh_ply.h"
#include "fs/cancel_token.h"
#include "fs/gio_ptr.h"
#include "fs/location.h"
#include "fs/posix_fd.h"
#include "machine_slowdown.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/provider_process.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::MeshFormat;
  using luciole::formats::Mesh;
  using luciole::formats::MeshLimits;
  using luciole::formats::Vec3;
  using Bytes = std::vector<std::uint8_t>;

  constexpr std::size_t kHeader = 40;
  constexpr MeshLimits kLimits{.maxVertices = 100, .maxTriangles = 100};

  // A tetrahedron with one normal per vertex, as a reader leaves it.
  Mesh tetra(bool normals) {
    Mesh mesh;
    mesh.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    if (normals) {
      mesh.normals = {Vec3{0, 0, -1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    }
    mesh.indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    luciole::formats::finishMesh(mesh);
    return mesh;
  }

  void putU32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      bytes.at(offset + i) = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
    }
  }

  void putF32(Bytes& bytes, std::size_t offset, float value) {
    putU32(bytes, offset, std::bit_cast<std::uint32_t>(value));
  }

  std::uint32_t u32(const Bytes& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(bytes.at(offset + i)) << (i * 8);
    }
    return value;
  }

  bool refused(const Bytes& bytes, const MeshLimits& limits = kLimits) {
    return !luciole::readMeshWorkerResult(bytes, limits).has_value();
  }

  void formatsByName() {
    TEST_CHECK(luciole::meshFormat("part.stl") == MeshFormat::Stl);
    TEST_CHECK(luciole::meshFormat("PART.STL") == MeshFormat::Stl);
    TEST_CHECK(luciole::meshFormat("a.b.obj") == MeshFormat::Obj);
    TEST_CHECK(luciole::meshFormat("scan.Ply") == MeshFormat::Ply);
    TEST_CHECK(luciole::meshFormat("scene.gltf") == MeshFormat::Gltf);
    TEST_CHECK(luciole::meshFormat("scene.glb") == MeshFormat::Gltf);
    TEST_CHECK(!luciole::meshFormat("notes.txt"));
    TEST_CHECK(!luciole::meshFormat("stl"));
    TEST_CHECK(!luciole::meshFormat(".stl"));
    TEST_CHECK(!luciole::meshFormat("part.stl.bak"));
    TEST_CHECK(!luciole::meshFormat(""));
  }

  void roundTripKeepsGeometry() {
    Mesh mesh = tetra(true);
    mesh.truncated = true;
    mesh.skipped.materials = true;
    mesh.skipped.otherModes = true;
    const Bytes bytes = luciole::encodeMeshResult(mesh);
    TEST_CHECK(bytes.size() == kHeader + (4 * 12 * 2) + (12 * 4));
    const auto geometry = luciole::readMeshWorkerResult(bytes, kLimits);
    TEST_CHECK(geometry.has_value());
    TEST_CHECK(geometry->vertices.size() == 4);
    TEST_CHECK((geometry->indices == std::vector<std::uint32_t>{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3}));
    TEST_CHECK(geometry->triangleCount() == 4);
    TEST_CHECK(!geometry->flatNormals);
    TEST_CHECK(geometry->truncated);
    TEST_CHECK(geometry->skipped.materials && geometry->skipped.otherModes);
    TEST_CHECK(!geometry->skipped.sparse && !geometry->skipped.skins && !geometry->skipped.compressed);
    TEST_CHECK((geometry->bounds.min == Vec3{0, 0, 0}));
    TEST_CHECK((geometry->bounds.max == Vec3{1, 1, 1}));
    const auto& second = geometry->vertices[1];
    TEST_CHECK(second.position[0] == 1.0F && second.position[1] == 0.0F && second.position[2] == 0.0F);
    TEST_CHECK(second.normal[0] == 127 && second.normal[1] == 0 && second.normal[2] == 0);
    TEST_CHECK(geometry->vertices[0].normal[2] == -127);
    TEST_CHECK(geometry->bytes() >= (4 * 16) + (12 * 4));
  }

  void flatNormalsWithoutNormals() {
    // Two triangles sharing an edge, in the z = 0 plane, the second facing down.
    Mesh mesh;
    mesh.positions = {Vec3{0, 0, 0}, Vec3{2, 0, 0}, Vec3{0, 2, 0}, Vec3{2, 2, 0}};
    mesh.indices = {0, 1, 2, 1, 2, 3};
    luciole::formats::finishMesh(mesh);
    const auto geometry = luciole::readMeshWorkerResult(luciole::encodeMeshResult(mesh), kLimits);
    TEST_CHECK(geometry.has_value());
    TEST_CHECK(geometry->flatNormals);
    TEST_CHECK(geometry->vertices.size() == 6);
    TEST_CHECK((geometry->indices == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5}));
    for (std::size_t v = 0; v < 3; ++v) {
      TEST_CHECK(geometry->vertices[v].normal[2] == 127);
      TEST_CHECK(geometry->vertices[3 + v].normal[2] == -127);
    }
    TEST_CHECK(geometry->vertices[4].position[0] == 0.0F && geometry->vertices[4].position[1] == 2.0F);
  }

  void degenerateTriangleHasZeroNormal() {
    Mesh mesh;
    mesh.positions = {Vec3{0, 0, 0}, Vec3{1, 1, 1}, Vec3{2, 2, 2}};
    mesh.indices = {0, 1, 2};
    luciole::formats::finishMesh(mesh);
    const auto geometry = luciole::readMeshWorkerResult(luciole::encodeMeshResult(mesh), kLimits);
    TEST_CHECK(geometry.has_value());
    TEST_CHECK(geometry->vertices[0].normal == (std::array<std::int8_t, 4>{0, 0, 0, 0}));
  }

  void errorAnswers() {
    const auto error =
        luciole::readMeshWorkerResult(luciole::encodeMeshResult(luciole::formats::meshError("bad")), kLimits);
    TEST_CHECK(!error.has_value() && error.error() == "bad");
    Bytes control = {'N', 'M', 'E', '1', 'a', 0x07, 'b'};
    TEST_CHECK(refused(control));
    TEST_CHECK(luciole::readMeshWorkerResult(control, kLimits).error() != "a\ab");
    Bytes longError(1025, 'x');
    std::memcpy(longError.data(), "NME1", 4);
    TEST_CHECK(refused(longError));
    TEST_CHECK(refused(Bytes{'N', 'M', 'E', '1'}));
    // A reader's message with a control character is sent with that character replaced.
    const Bytes sent = luciole::encodeMeshResult(luciole::formats::meshError("line\nbreak"));
    TEST_CHECK(luciole::readMeshWorkerResult(sent, kLimits).error() == "line?break");
    Mesh empty;
    luciole::formats::finishMesh(empty);
    TEST_CHECK(refused(luciole::encodeMeshResult(empty)));
  }

  void hostileHeaders() {
    const Bytes good = luciole::encodeMeshResult(tetra(true));
    TEST_CHECK(!refused(good));
    TEST_CHECK(refused({}));
    TEST_CHECK(refused(Bytes(good.begin(), good.begin() + kHeader - 1)));
    Bytes magic = good;
    magic[3] = '2';
    TEST_CHECK(refused(magic));
    Bytes flags = good;
    putU32(flags, 4, u32(good, 4) | 0x80000000U);
    TEST_CHECK(refused(flags));
    // Counts far beyond the bytes and the limits are refused before anything is allocated.
    for (const std::uint32_t huge : {0xFFFFFFFFU, 0x40000000U, 101U}) {
      Bytes vertices = good;
      putU32(vertices, 8, huge);
      TEST_CHECK(refused(vertices));
      Bytes indices = good;
      putU32(indices, 12, huge * 3);
      TEST_CHECK(refused(indices));
    }
    Bytes notTriangles = good;
    putU32(notTriangles, 12, 11);
    TEST_CHECK(refused(notTriangles));
    Bytes shortBody = good;
    shortBody.pop_back();
    TEST_CHECK(refused(shortBody));
    Bytes longBody = good;
    longBody.push_back(0);
    TEST_CHECK(refused(longBody));
    // Fewer vertices claimed with the same bytes: the sizes no longer add up.
    Bytes fewer = good;
    putU32(fewer, 8, 3);
    TEST_CHECK(refused(fewer));
    // The tetrahedron's own counts against tighter limits.
    TEST_CHECK(refused(good, MeshLimits{.maxVertices = 3, .maxTriangles = 100}));
    TEST_CHECK(refused(good, MeshLimits{.maxVertices = 100, .maxTriangles = 3}));
  }

  void hostileValues() {
    const Bytes good = luciole::encodeMeshResult(tetra(true));
    const std::size_t positions = kHeader;
    const std::size_t normals = positions + (4 * 12);
    const std::size_t indices = normals + (4 * 12);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Bytes index = good;
    putU32(index, indices + 8, 4);
    TEST_CHECK(refused(index));
    Bytes position = good;
    putF32(position, positions + 4, nan);
    TEST_CHECK(refused(position));
    Bytes outside = good;
    putF32(outside, positions + 12, 1.5F);
    TEST_CHECK(refused(outside));
    Bytes normal = good;
    putF32(normal, normals, inf);
    TEST_CHECK(refused(normal));
    Bytes bounds = good;
    putF32(bounds, 16, -inf);
    TEST_CHECK(refused(bounds));
    Bytes inverted = good;
    putF32(inverted, 16, 2.0F); // min x above max x
    TEST_CHECK(refused(inverted));
  }

  Bytes triangleAt(const Vec3& a, const Vec3& b, const Vec3& c) {
    Mesh mesh;
    mesh.positions = {a, b, c};
    mesh.indices = {0, 1, 2};
    luciole::formats::finishMesh(mesh);
    return luciole::encodeMeshResult(mesh);
  }

  // Finite coordinates past kMaxMeshCoordinate on either side are refused with a sentence of their own.
  void coordinatesBeyondTheLimit() {
    const float most = luciole::kMaxMeshCoordinate;
    TEST_CHECK(!refused(triangleAt(Vec3{-most, 0, 0}, Vec3{most, 0, 0}, Vec3{0, most, -most})));
    const float past = std::nextafter(most, std::numeric_limits<float>::infinity());
    const float largest = std::numeric_limits<float>::max();
    for (const Bytes& answer :
         {triangleAt(Vec3{-3e38F, 0, 0}, Vec3{3e38F, 0, 0}, Vec3{0, 3e38F, 0}),
          triangleAt(Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, past}),
          triangleAt(Vec3{0, -past, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}),
          triangleAt(
              Vec3{largest, largest, largest}, Vec3{largest, largest, largest}, Vec3{largest, largest, largest}
          )}) {
      const auto result = luciole::readMeshWorkerResult(answer, kLimits);
      TEST_CHECK(!result.has_value() && result.error() == "This model is too large to preview.");
    }
  }

  struct Folder {
    luciole::test::TempFolder tempFolder{"luciole-mesh-preview"};
    fs::path path = tempFolder.path();

    fs::path write(std::string_view name, std::string_view bytes) const {
      const fs::path file = path / name;
      fs::create_directories(file.parent_path());
      std::ofstream out(file, std::ios::binary);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      TEST_CHECK(out.good());
      return file;
    }
  };

  luciole::MeshPreview load(const fs::path& file) {
    return luciole::loadMeshPreview(luciole::Location::fromPath(file.string()), luciole::CancelToken{});
  }

  std::string asciiStlCube() {
    std::string out = "solid cube\n";
    const auto corner = [](int bits) { return std::format("{} {} {}", bits & 1, (bits >> 1) & 1, (bits >> 2) & 1); };
    constexpr std::array<std::array<int, 3>, 12> kTriangles{{
        {0, 2, 1},
        {1, 2, 3},
        {4, 5, 6},
        {5, 7, 6},
        {0, 1, 4},
        {1, 5, 4},
        {2, 6, 3},
        {3, 6, 7},
        {0, 4, 2},
        {2, 4, 6},
        {1, 3, 5},
        {3, 7, 5},
    }};
    for (const auto& triangle : kTriangles) {
      out += "facet normal 0 0 0\nouter loop\n";
      for (const int bits : triangle) {
        out += "vertex " + corner(bits) + "\n";
      }
      out += "endloop\nendfacet\n";
    }
    return out + "endsolid cube\n";
  }

  // The tetrahedron as glTF: four float corners and twelve 16-bit indices in one 72-byte buffer at `uri`.
  std::string tetraGltf(std::string_view uri) {
    return std::string(R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)")
        + R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
        + R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":24}],)"
        + R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)"
        + R"({"bufferView":1,"componentType":5123,"count":12,"type":"SCALAR"}],)"
        + R"("buffers":[{"byteLength":72,"uri":")"
        + std::string(uri)
        + "\"}]}";
  }

  std::string tetraBuffer() {
    std::string out;
    for (const float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F}) {
      const auto bits = std::bit_cast<std::uint32_t>(value);
      for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((bits >> shift) & 0xFFU));
      }
    }
    for (const int index : {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3}) {
      out.push_back(static_cast<char>(index));
      out.push_back('\0');
    }
    return out;
  }

  void helperReadsEachFormat(const Folder& folder) {
    const auto stl = load(folder.write("cube.stl", asciiStlCube()));
    TEST_CHECK(stl.has_value());
    TEST_CHECK((*stl)->triangleCount() == 12 && (*stl)->vertices.size() == 36);
    TEST_CHECK((*stl)->flatNormals && (*stl)->up == luciole::MeshUp::Z);
    TEST_CHECK(((*stl)->bounds.max == Vec3{1, 1, 1}));
    const auto obj = load(folder.write("tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n"));
    TEST_CHECK(obj.has_value());
    TEST_CHECK((*obj)->triangleCount() == 1 && !(*obj)->flatNormals && (*obj)->up == luciole::MeshUp::Y);
    TEST_CHECK((*obj)->vertices[2].normal[2] == 127);
    const auto ply = load(folder.write(
        "tri.ply",
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n"
    ));
    TEST_CHECK(ply.has_value());
    TEST_CHECK((*ply)->triangleCount() == 1 && (*ply)->flatNormals);
  }

  void helperReadsBuffersBesideTheFile(const Folder& folder) {
    folder.write("tetra.bin", tetraBuffer());
    folder.write("parts/tetra.bin", tetraBuffer());
    const auto beside = load(folder.write("beside.gltf", tetraGltf("tetra.bin")));
    TEST_CHECK(beside.has_value());
    TEST_CHECK((*beside)->triangleCount() == 4);
    TEST_CHECK(!load(folder.write("parts/up.gltf", tetraGltf("../tetra.bin"))).has_value());
    TEST_CHECK(!load(folder.write("below.gltf", tetraGltf("parts/tetra.bin"))).has_value());
    TEST_CHECK(!load(folder.write("absolute.gltf", tetraGltf((folder.path / "tetra.bin").string()))).has_value());
    fs::create_symlink(folder.path / "tetra.bin", folder.path / "link.bin");
    TEST_CHECK(!load(folder.write("link.gltf", tetraGltf("link.bin"))).has_value());
    const auto missing = load(folder.write("missing.gltf", tetraGltf("gone.bin")));
    TEST_CHECK(!missing.has_value() && missing.error() == "This model needs a file beside it that could not be read.");
    // The byte cap covers the file and its buffers together.
    const fs::path file = folder.path / "beside.gltf";
    const auto fileBytes = fs::file_size(file);
    const auto output = folder.path / "capped.result";
    const luciole::Fd input(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    TEST_CHECK(input.valid());
    const auto run = luciole::runProviderProcess(
        luciole::meshPreviewArguments(MeshFormat::Gltf, fileBytes + 71, folder.path.string(), output.string()),
        input.get(), nullptr
    );
    TEST_CHECK(!run.has_value());
    std::ifstream answer(output, std::ios::binary);
    const Bytes bytes((std::istreambuf_iterator<char>(answer)), std::istreambuf_iterator<char>());
    const auto result = luciole::readMeshWorkerResult(bytes, kLimits);
    TEST_CHECK(!result.has_value() && !result.error().empty());
  }

  // The error of a refused load: a full sentence, the reader's own words left out.
  std::string refusal(const luciole::MeshPreview& preview) {
    TEST_CHECK(!preview.has_value());
    const auto& text = preview.error();
    TEST_CHECK(!text.empty() && text.front() >= 'A' && text.front() <= 'Z' && text.ends_with('.'));
    return text;
  }

  void helperRefusals(const Folder& folder) {
    // Text with no facet in it holds nothing to draw.
    TEST_CHECK(
        refusal(load(folder.write("noise.stl", "solid x\nfacet nonsense\n"))) == "This model has no triangles to show."
    );
    // A binary STL that claims far more triangles than it holds.
    std::string claims(84, ' ');
    const std::uint32_t billion = 1'000'000'000;
    std::memcpy(claims.data() + 80, &billion, 4);
    TEST_CHECK(
        refusal(load(folder.write("claims.stl", claims + std::string(100, '\0'))))
        == "This file is damaged or is not an STL model."
    );
    TEST_CHECK(
        refusal(load(folder.write("bad.obj", "v 0 0\nf 1 2 3\n"))) == "This file is damaged or is not an OBJ model."
    );
    TEST_CHECK(
        refusal(load(folder.write("bad.ply", "ply\nformat\n"))) == "This file is damaged or is not a PLY model."
    );
    TEST_CHECK(
        refusal(load(folder.write("bad.glb", "glTF nonsense"))) == "This file is damaged or is not a glTF model."
    );
    // A model the reader declines for its version or its size says so.
    TEST_CHECK(
        refusal(load(folder.write("old.gltf", R"({"asset":{"version":"1.0"}})")))
        == "This file is not a glTF 2.0 model, the only glTF version Luciole reads."
    );
    std::string elements = "ply\nformat ascii 1.0\n";
    for (std::size_t k = 0; k <= luciole::formats::kMaxPlyElements; ++k) {
      elements += std::format("element e{} 0\n", k);
    }
    TEST_CHECK(
        refusal(load(folder.write("wide.ply", elements + "end_header\n"))) == "This model is too large to preview."
    );
    std::string values = R"({"asset":{"version":"2.0"},"extras":[)";
    for (int k = 0; k < 1'000'001; ++k) {
      values += k == 0 ? "0" : ",0";
    }
    TEST_CHECK(refusal(load(folder.write("many.gltf", values + "]}"))) == "This model is too large to preview.");
    TEST_CHECK(
        refusal(load(folder.write("note.txt", "v 0 0 0\n"))) == "This file is not a model format Luciole reads."
    );
    TEST_CHECK(refusal(load(folder.write("empty.obj", ""))) == "This model file is empty.");
    TEST_CHECK(refusal(load(folder.write("folder.stl/inside", "").parent_path())) == "This model cannot be opened.");
    TEST_CHECK(refusal(load(folder.path / "absent.stl")) == "This model cannot be opened.");
    TEST_CHECK(
        refusal(luciole::loadMeshPreview(luciole::Location::trash(), luciole::CancelToken{}))
        == "Only local files are shown as models."
    );
    const luciole::CancelToken cancelled;
    cancelled.cancel();
    const auto file = folder.write("cancel.stl", asciiStlCube());
    TEST_CHECK(
        refusal(luciole::loadMeshPreview(luciole::Location::fromPath(file.string()), cancelled))
        == "Reading was cancelled."
    );
    TEST_CHECK(luciole::meshPreviewArguments(MeshFormat::Ply, 5, "/f", "/o").size() == 6);
  }

  // A binary STL of `triangles` triangles in a grid, each with its own corners.
  std::string binaryStl(std::uint32_t triangles) {
    std::string out(80, ' ');
    out.resize(84 + (std::size_t{triangles} * 50));
    std::memcpy(out.data() + 80, &triangles, 4);
    char* at = out.data() + 84;
    const std::uint32_t side = 1000;
    for (std::uint32_t t = 0; t < triangles; ++t) {
      const auto cell = static_cast<float>(t / 2);
      const float x = std::fmod(cell, static_cast<float>(side));
      const float y = std::floor(cell / static_cast<float>(side));
      const float z = std::sin(x * 0.05F) * std::cos(y * 0.05F) * 10.0F;
      const bool upper = t % 2 == 1;
      const std::array<float, 12> values{
          0, 0, 0, x, y, z, upper ? x : x + 1, upper ? y + 1 : y, z, x + 1, y + 1, z + 0.5F,
      };
      std::memcpy(at, values.data(), 48);
      at += 50;
    }
    return out;
  }

  double childSeconds() {
    rusage usage{};
    TEST_CHECK(::getrusage(RUSAGE_CHILDREN, &usage) == 0);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
        + (static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6);
  }

  // The largest resident size any finished helper reached, in MiB.
  double childPeakMiB() {
    rusage usage{};
    TEST_CHECK(::getrusage(RUSAGE_CHILDREN, &usage) == 0);
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
  }

  // Processor time of the calling thread, which reads the helper's answer and builds the geometry; other threads of the
  // process and other processes do not count.
  double threadSeconds() {
    timespec now{};
    TEST_CHECK(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return static_cast<double>(now.tv_sec) + (static_cast<double>(now.tv_nsec) * 1.0E-9);
  }

  // The parent's share stays under five seconds of its own processor time, divided by the machine slowdown, about ten
  // times what it takes on a quiet machine, so a superlinear step in taking the answer up fails and a busy machine
  // does not.
  void measureMillionTriangles(const Folder& folder) {
    const auto file = folder.write("million.stl", binaryStl(1'000'000));
    const double slowdown = luciole::test::machineSlowdown();
    const double childBefore = childSeconds();
    const double start = threadSeconds();
    const auto geometry = load(file);
    const double parent = threadSeconds() - start;
    const double child = childSeconds() - childBefore;
    TEST_CHECK(geometry.has_value());
    TEST_CHECK((*geometry)->triangleCount() == 1'000'000 && !(*geometry)->truncated);
    TEST_CHECK((*geometry)->vertices.size() == 3'000'000);
    TEST_CHECK(parent / slowdown < 5.0);
    std::println(
        "mesh preview: 1000000 triangles loaded, helper {:.3f} s, parent {:.3f} s of processor time, {:.1f} MiB held, "
        "helper peak {:.1f} MiB resident",
        child, parent, static_cast<double>((*geometry)->bytes()) / (1024.0 * 1024.0), childPeakMiB()
    );
    fs::remove(file);
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--mesh-preview-worker") {
    return luciole::runMeshPreviewWorker(std::span(args).subspan(1));
  }
  formatsByName();
  roundTripKeepsGeometry();
  flatNormalsWithoutNormals();
  degenerateTriangleHasZeroNormal();
  errorAnswers();
  hostileHeaders();
  hostileValues();
  coordinatesBeyondTheLimit();
  static const Folder folder;
  helperReadsEachFormat(folder);
  helperReadsBuffersBesideTheFile(folder);
  helperRefusals(folder);
  measureMillionTriangles(folder);
  std::println("mesh preview checks passed");
}
