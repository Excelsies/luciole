#include "thumbnails/mesh_preview.h"

#include "formats/mesh_gltf.h"
#include "formats/mesh_obj.h"
#include "formats/mesh_ply.h"
#include "formats/mesh_stl.h"
#include "fs/gio_interop.h"
#include "fs/gio_ptr.h"
#include "fs/posix_fd.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_store.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <numeric>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using luciole::formats::Mesh;
  using luciole::formats::Vec3;
  namespace mesh_io = luciole::formats::mesh_io;

  constexpr std::string_view kResultMagic = "NMR1";
  constexpr std::string_view kErrorMagic = "NME1";
  constexpr std::size_t kHeaderBytes = 40;
  constexpr std::size_t kMaxErrorBytes = 1024;
  constexpr std::uint32_t kHasNormals = 1U;
  constexpr std::uint32_t kTruncated = 2U;
  constexpr std::uint32_t kSkippedShift = 8;
  constexpr std::uint32_t kSkippedCount = 7;
  constexpr std::uint32_t kKnownFlags = kHasNormals | kTruncated | (((1U << kSkippedCount) - 1U) << kSkippedShift);
  constexpr std::string_view kUnreadable = "This model could not be read.";
  constexpr std::string_view kTooLarge = "This model is too large to preview.";
  // The helper's answer when a glTF file's outside buffer could not be read.
  constexpr std::string_view kMissingBuffer = "This model needs a file beside it that could not be read.";

  struct FormatName {
    luciole::MeshFormat format;
    std::string_view token;
    std::string_view model; // the model the format holds, with its article
  };
  constexpr std::array<FormatName, 4> kFormatNames{{
      {luciole::MeshFormat::Stl, "stl", "an STL model"},
      {luciole::MeshFormat::Obj, "obj", "an OBJ model"},
      {luciole::MeshFormat::Ply, "ply", "a PLY model"},
      {luciole::MeshFormat::Gltf, "gltf", "a glTF model"},
  }};

  // The sentence for a reader's refusal, whose own words name a limit, the glTF version or a detail of the format.
  std::string refusalOf(luciole::MeshFormat format, std::string_view reason) {
    if (reason == kMissingBuffer) {
      return std::string(kMissingBuffer);
    }
    if (reason.contains("too many") || reason.contains("the limit") || reason.contains("nested too deeply")) {
      return std::string(kTooLarge);
    }
    if (reason == "not a glTF 2.0 file") {
      return "This file is not a glTF 2.0 model, the only glTF version Luciole reads.";
    }
    const auto* const name = std::ranges::find(kFormatNames, format, &FormatName::format);
    return std::format("This file is damaged or is not {}.", name->model);
  }

  std::array<bool*, kSkippedCount> skippedFields(luciole::formats::SkippedParts& parts) {
    return {&parts.sparse,     &parts.skins,      &parts.morphTargets, &parts.materials,
            &parts.extensions, &parts.compressed, &parts.otherModes};
  }

  void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }

  void appendVec(std::vector<std::uint8_t>& out, const Vec3& v) {
    for (const float value : {v.x, v.y, v.z}) {
      appendU32(out, std::bit_cast<std::uint32_t>(value));
    }
  }

  bool startsWith(std::span<const std::uint8_t> bytes, std::string_view magic) {
    return bytes.size() >= magic.size() && std::ranges::equal(bytes.first(magic.size()), magic, {}, {}, [](char c) {
             return static_cast<std::uint8_t>(c);
           });
  }

  // Printable ASCII only, so a message can never carry control sequences into the interface.
  std::string printable(std::string_view text) {
    std::string out(text.substr(0, kMaxErrorBytes - kErrorMagic.size()));
    std::ranges::replace_if(out, [](unsigned char c) { return c < 32 || c > 126; }, '?');
    return out;
  }

  // Bounds-checked little-endian reads over a validated answer.
  class Reader {
  public:
    explicit Reader(std::span<const std::uint8_t> bytes)
        : m_text(reinterpret_cast<const char*>(bytes.data()), bytes.size()) {}

    std::uint32_t u32(std::size_t offset) const {
      return mesh_io::readScalar<std::uint32_t>(m_text, offset).value_or(0);
    }
    std::optional<Vec3> vec(std::size_t offset) const {
      const auto x = mesh_io::readScalar<float>(m_text, offset);
      const auto y = mesh_io::readScalar<float>(m_text, offset + 4);
      const auto z = mesh_io::readScalar<float>(m_text, offset + 8);
      if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z)) {
        return std::nullopt;
      }
      return Vec3{*x, *y, *z};
    }

  private:
    std::string_view m_text;
  };

  bool inside(const Vec3& p, const luciole::formats::Bounds& box) {
    return p.x >= box.min.x
        && p.y >= box.min.y
        && p.z >= box.min.z
        && p.x <= box.max.x
        && p.y <= box.max.y
        && p.z <= box.max.z;
  }

  std::array<std::int8_t, 4> packNormal(const Vec3& n) {
    const double length = std::sqrt((double{n.x} * n.x) + (double{n.y} * n.y) + (double{n.z} * n.z));
    if (!(length > 1e-12) || !std::isfinite(length)) {
      return {};
    }
    const auto pack = [length](float value) {
      return static_cast<std::int8_t>(std::clamp(std::lround(value / length * 127.0), -127L, 127L));
    };
    return {pack(n.x), pack(n.y), pack(n.z), 0};
  }

  // Vertices with packed normals; a mesh without normals gets one per triangle on its own three vertices.
  luciole::MeshGeometry geometryOf(Mesh mesh) {
    luciole::MeshGeometry out;
    out.bounds = mesh.bounds;
    out.truncated = mesh.truncated;
    out.skipped = mesh.skipped;
    if (!mesh.normals.empty()) {
      out.vertices.resize(mesh.positions.size());
      for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
        const Vec3& p = mesh.positions[v];
        out.vertices[v] = {.position = {p.x, p.y, p.z}, .normal = packNormal(mesh.normals[v])};
      }
      out.indices = std::move(mesh.indices);
      return out;
    }
    const std::vector<Vec3> normals = luciole::formats::flatNormals(mesh);
    out.flatNormals = true;
    out.vertices.resize(mesh.indices.size());
    for (std::size_t k = 0; k < mesh.indices.size(); ++k) {
      const Vec3& p = mesh.positions[mesh.indices[k]];
      out.vertices[k] = {.position = {p.x, p.y, p.z}, .normal = packNormal(normals[k / 3])};
    }
    out.indices.resize(mesh.indices.size());
    std::ranges::iota(out.indices, 0U);
    return out;
  }

  // Reads exactly `size` bytes from `fd`; ends when they are read or a read fails.
  std::optional<std::string> readExactly(int fd, std::size_t size) {
    std::string bytes(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
      const auto count = ::read(fd, bytes.data() + offset, size - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return std::nullopt;
      }
      offset += static_cast<std::size_t>(count);
    }
    return bytes;
  }

  bool writeAnswer(std::string_view path, std::span<const std::uint8_t> bytes) {
    const luciole::Fd output(
        ::open(std::string(path).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)
    );
    for (std::size_t offset = 0; output.valid() && offset < bytes.size();) {
      const auto count = ::write(output.get(), bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      offset += static_cast<std::size_t>(count);
    }
    return output.valid();
  }

  // Outside glTF buffers: names in `folder` itself, never through a symlink, within the bytes `budget` leaves.
  class FolderBuffers {
  public:
    FolderBuffers(std::string_view folder, std::uint64_t budget) : m_budget(budget) {
      if (!folder.empty()) {
        m_folder = luciole::Fd(::open(std::string(folder).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      }
    }

    // A buffer was asked for and could not be given.
    bool missed() const noexcept { return m_missed; }

    std::optional<std::string> load(std::string_view name) {
      auto bytes = read(name);
      m_missed = m_missed || !bytes;
      return bytes;
    }

  private:
    std::optional<std::string> read(std::string_view name) {
      if (!m_folder.valid() || name.empty() || name.contains('/')) {
        return std::nullopt;
      }
      const luciole::Fd file(
          ::openat(m_folder.get(), std::string(name).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)
      );
      struct stat info{};
      if (!file.valid()
          || ::fstat(file.get(), &info) < 0
          || !S_ISREG(info.st_mode)
          || info.st_size < 0
          || static_cast<std::uint64_t>(info.st_size) > m_budget) {
        return std::nullopt;
      }
      m_budget -= static_cast<std::uint64_t>(info.st_size);
      return readExactly(file.get(), static_cast<std::size_t>(info.st_size));
    }

    luciole::Fd m_folder;
    std::uint64_t m_budget = 0;
    bool m_missed = false;
  };

  Mesh readMesh(luciole::MeshFormat format, std::string_view bytes, FolderBuffers& buffers) {
    switch (format) {
    case luciole::MeshFormat::Stl:
      return luciole::formats::readStl(bytes, luciole::kMeshPreviewLimits);
    case luciole::MeshFormat::Obj:
      return luciole::formats::readObj(bytes, luciole::kMeshPreviewLimits);
    case luciole::MeshFormat::Ply:
      return luciole::formats::readPly(bytes, luciole::kMeshPreviewLimits);
    case luciole::MeshFormat::Gltf:
      return luciole::formats::readGltf(
          bytes, [&buffers](std::string_view name) { return buffers.load(name); }, luciole::kMeshPreviewLimits
      );
    }
    return luciole::formats::meshError("unknown model format");
  }

  std::uint64_t answerLimit() {
    const auto& limits = luciole::kMeshPreviewLimits;
    return kHeaderBytes + (std::uint64_t{limits.maxVertices} * 24U) + (std::uint64_t{limits.maxTriangles} * 12U);
  }
} // namespace

std::size_t luciole::MeshGeometry::bytes() const {
  return (vertices.capacity() * sizeof(MeshVertex)) + (indices.capacity() * sizeof(std::uint32_t));
}

std::optional<luciole::MeshFormat> luciole::meshFormat(std::string_view fileName) {
  const std::size_t dot = fileName.rfind('.');
  if (dot == std::string_view::npos || dot == 0) {
    return std::nullopt;
  }
  std::string extension(fileName.substr(dot + 1));
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  if (extension == "glb") {
    return MeshFormat::Gltf;
  }
  const auto* const name = std::ranges::find(kFormatNames, extension, &FormatName::token);
  return name != kFormatNames.end() ? std::optional(name->format) : std::nullopt;
}

std::vector<std::uint8_t> luciole::encodeMeshResult(const formats::Mesh& mesh) {
  std::vector<std::uint8_t> out;
  if (!mesh.error.empty()) {
    const std::string message = printable(mesh.error);
    out.assign(kErrorMagic.begin(), kErrorMagic.end());
    out.insert(out.end(), message.begin(), message.end());
    return out;
  }
  const bool normals = !mesh.normals.empty();
  std::uint32_t flags = (normals ? kHasNormals : 0U) | (mesh.truncated ? kTruncated : 0U);
  formats::SkippedParts skipped = mesh.skipped;
  const auto fields = skippedFields(skipped);
  for (std::uint32_t bit = 0; bit < kSkippedCount; ++bit) {
    flags |= *fields.at(bit) ? 1U << (kSkippedShift + bit) : 0U;
  }
  out.reserve(kHeaderBytes + (mesh.positions.size() * (normals ? 24 : 12)) + (mesh.indices.size() * 4));
  out.assign(kResultMagic.begin(), kResultMagic.end());
  appendU32(out, flags);
  appendU32(out, static_cast<std::uint32_t>(mesh.positions.size()));
  appendU32(out, static_cast<std::uint32_t>(mesh.indices.size()));
  appendVec(out, mesh.bounds.min);
  appendVec(out, mesh.bounds.max);
  for (const Vec3& p : mesh.positions) {
    appendVec(out, p);
  }
  for (const Vec3& n : mesh.normals) {
    appendVec(out, n);
  }
  for (const std::uint32_t index : mesh.indices) {
    appendU32(out, index);
  }
  return out;
}

std::expected<luciole::MeshGeometry, std::string>
luciole::readMeshWorkerResult(std::span<const std::uint8_t> bytes, const formats::MeshLimits& limits) {
  if (startsWith(bytes, kErrorMagic)) {
    const auto text = bytes.subspan(kErrorMagic.size());
    if (text.empty() || bytes.size() > kMaxErrorBytes || !std::ranges::all_of(text, [](std::uint8_t c) {
          return c >= 32 && c <= 126;
        })) {
      return std::unexpected(std::string(kUnreadable));
    }
    return std::unexpected(std::string(text.begin(), text.end()));
  }
  const Reader read(bytes);
  if (bytes.size() < kHeaderBytes || !startsWith(bytes, kResultMagic) || (read.u32(4) & ~kKnownFlags) != 0) {
    return std::unexpected(std::string(kUnreadable));
  }
  const std::uint32_t flags = read.u32(4);
  const bool hasNormals = (flags & kHasNormals) != 0;
  const std::size_t vertexCount = read.u32(8);
  const std::size_t indexCount = read.u32(12);
  if (vertexCount > limits.maxVertices || indexCount % 3 != 0 || indexCount / 3 > limits.maxTriangles) {
    return std::unexpected(std::string(kUnreadable));
  }
  const auto vertexBytes = mesh_io::checkedMul(vertexCount, hasNormals ? 24 : 12);
  const auto indexBytes = mesh_io::checkedMul(indexCount, 4);
  const auto body = vertexBytes && indexBytes ? mesh_io::checkedAdd(*vertexBytes, *indexBytes) : std::nullopt;
  const auto total = body ? mesh_io::checkedAdd(*body, kHeaderBytes) : std::nullopt;
  const auto low = read.vec(16);
  const auto high = read.vec(28);
  if (!total || *total != bytes.size() || !low || !high || low->x > high->x || low->y > high->y || low->z > high->z) {
    return std::unexpected(std::string(kUnreadable));
  }
  if (indexCount == 0) {
    return std::unexpected("This model has no triangles to show.");
  }
  const auto withinReach = [](const Vec3& v) {
    return std::abs(v.x) <= kMaxMeshCoordinate
        && std::abs(v.y) <= kMaxMeshCoordinate
        && std::abs(v.z) <= kMaxMeshCoordinate;
  };
  if (!withinReach(*low) || !withinReach(*high)) {
    return std::unexpected(std::string(kTooLarge));
  }
  Mesh mesh;
  mesh.bounds = {.min = *low, .max = *high};
  mesh.truncated = (flags & kTruncated) != 0;
  const auto fields = skippedFields(mesh.skipped);
  for (std::uint32_t bit = 0; bit < kSkippedCount; ++bit) {
    *fields.at(bit) = (flags & (1U << (kSkippedShift + bit))) != 0;
  }
  mesh.positions.resize(vertexCount);
  mesh.normals.resize(hasNormals ? vertexCount : 0);
  mesh.indices.resize(indexCount);
  std::size_t offset = kHeaderBytes;
  for (Vec3& position : mesh.positions) {
    const auto p = read.vec(offset);
    if (!p || !inside(*p, mesh.bounds)) {
      return std::unexpected(std::string(kUnreadable));
    }
    position = *p;
    offset += 12;
  }
  for (Vec3& normal : mesh.normals) {
    const auto n = read.vec(offset);
    if (!n) {
      return std::unexpected(std::string(kUnreadable));
    }
    normal = *n;
    offset += 12;
  }
  for (std::uint32_t& index : mesh.indices) {
    index = read.u32(offset);
    if (index >= vertexCount) {
      return std::unexpected(std::string(kUnreadable));
    }
    offset += 4;
  }
  return geometryOf(std::move(mesh));
}

std::vector<std::string> luciole::meshPreviewArguments(
    MeshFormat format, std::uint64_t maxBytes, std::string_view folder, std::string_view output
) {
  const auto* const name = std::ranges::find(kFormatNames, format, &FormatName::format);
  return {"/proc/self/exe",         "--mesh-preview-worker", std::string(name->token),
          std::to_string(maxBytes), std::string(folder),     std::string(output)};
}

int luciole::runMeshPreviewWorker(std::span<const std::string_view> arguments) {
  if (arguments.size() != 4) {
    return 2;
  }
  const auto* const name = std::ranges::find(kFormatNames, arguments[0], &FormatName::token);
  std::uint64_t maxBytes = 0;
  const auto parsed = std::from_chars(arguments[1].data(), arguments[1].data() + arguments[1].size(), maxBytes);
  struct stat info{};
  if (name == kFormatNames.end()
      || parsed.ec != std::errc{}
      || parsed.ptr != arguments[1].data() + arguments[1].size()
      || maxBytes > kMaxMeshSourceBytes
      || ::fstat(3, &info) < 0
      || !S_ISREG(info.st_mode)
      || info.st_size <= 0
      || static_cast<std::uint64_t>(info.st_size) > maxBytes) {
    return 2;
  }
  try {
    const auto bytes = readExactly(3, static_cast<std::size_t>(info.st_size));
    if (!bytes) {
      return 1;
    }
    FolderBuffers buffers(arguments[2], maxBytes - static_cast<std::uint64_t>(info.st_size));
    Mesh mesh = readMesh(name->format, *bytes, buffers);
    if (!mesh.error.empty() && buffers.missed()) {
      mesh.error = kMissingBuffer;
    }
    return writeAnswer(arguments[3], encodeMeshResult(mesh)) && mesh.error.empty() ? 0 : 1;
  } catch (const std::exception&) {
    return 1;
  }
}

luciole::MeshPreview luciole::loadMeshPreview(const Location& location, const CancelToken& token) {
  const auto path = location.nativePath();
  if (!path) {
    return std::unexpected("Only local files are shown as models.");
  }
  const std::filesystem::path file(*path);
  const auto format = meshFormat(file.filename().string());
  if (!format) {
    return std::unexpected("This file is not a model format Luciole reads.");
  }
  const Fd input(::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
  struct stat info{};
  if (!input.valid() || ::fstat(input.get(), &info) < 0 || !S_ISREG(info.st_mode)) {
    return std::unexpected("This model cannot be opened.");
  }
  if (info.st_size <= 0 || static_cast<std::uint64_t>(info.st_size) > kMaxMeshSourceBytes) {
    return std::unexpected(info.st_size <= 0 ? std::string("This model file is empty.") : std::string(kTooLarge));
  }
  const GString temporary(g_dir_make_tmp("luciole-mesh-XXXXXX", nullptr));
  if (!temporary) {
    return std::unexpected(std::string(kUnreadable));
  }
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{temporary.get()};
  const auto output = cleanup.path / "result";
  const auto run = runProviderProcess(
      meshPreviewArguments(*format, kMaxMeshSourceBytes, file.parent_path().string(), output.string()), input.get(),
      gioCancellable(token), answerLimit(), 1
  );
  if (token.cancelled()) {
    return std::unexpected("Reading was cancelled.");
  }
  if (!run) {
    // A reader's refusal leaves an error answer; a helper stopped by its limits leaves none.
    const auto report = readThumbnailFile(output, kMaxErrorBytes);
    if (report && startsWith(*report, kErrorMagic)) {
      return std::unexpected(refusalOf(*format, readMeshWorkerResult(*report, kMeshPreviewLimits).error()));
    }
    return std::unexpected(std::string(kUnreadable));
  }
  const auto answer = readThumbnailFile(output, answerLimit());
  if (!answer) {
    return std::unexpected(std::string(kUnreadable));
  }
  auto geometry = readMeshWorkerResult(*answer, kMeshPreviewLimits);
  if (!geometry) {
    return std::unexpected(std::move(geometry.error()));
  }
  geometry->up = *format == MeshFormat::Stl ? MeshUp::Z : MeshUp::Y;
  return std::make_shared<const MeshGeometry>(std::move(*geometry));
}
