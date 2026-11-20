#pragma once

#include "formats/mesh.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace luciole::formats {
  // Deepest node hierarchy a scene may have; the scene's root nodes are at depth 1.
  inline constexpr std::size_t kMaxGltfNodeDepth = 64;

  // Returns the bytes of an external buffer file, given its percent-decoded path relative to the glTF file, or
  // nothing when it cannot be read. The reader never passes a path that is absolute, has a scheme or drive, holds
  // a backslash, a NUL or a ".." segment. The caller bounds what it reads.
  using BufferLoader = std::function<std::optional<std::string>(std::string_view path)>;

  // Reads glTF 2.0: the GLB container when the bytes start with "glTF", JSON otherwise. Draws the default scene
  // (or the first; without scenes, every node that is no other node's child, and an error when nodes exist but
  // none is a root) with node transforms applied. Each mesh is resolved once, however many nodes place it.
  // Primitives in triangle mode are read from accessors of any component type, normalized or not, through
  // buffer views with or without a byte stride, from the GLB binary chunk, base64 data URIs or `loadBuffer`
  // (which may be empty). Normals are kept when every drawn primitive has them. Parts that are not read are
  // listed in Mesh::skipped. The JSON is parsed with the default JsonLimits.
  Mesh readGltf(std::string_view bytes, const BufferLoader& loadBuffer, const MeshLimits& limits = {});
} // namespace luciole::formats

// GLB container and buffer URI decoding for readGltf.
namespace luciole::formats::gltf_io {
  inline constexpr std::uint32_t kJsonChunk = 0x4E4F534AU;
  inline constexpr std::uint32_t kBinChunk = 0x004E4942U;
  inline constexpr std::string_view kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  // Standard base64 with optional padding; nothing for any other character.
  inline std::optional<std::string> decodeBase64(std::string_view text) {
    for (int pad = 0; pad < 2 && text.ends_with('='); ++pad) {
      text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
      return std::nullopt;
    }
    std::string out;
    out.reserve((text.size() / 4 * 3) + 2);
    std::uint32_t group = 0;
    unsigned bits = 0;
    for (const char c : text) {
      const std::size_t value = kBase64.find(c);
      if (value == std::string_view::npos) {
        return std::nullopt;
      }
      group = (group << 6U) | static_cast<std::uint32_t>(value);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back(static_cast<char>((group >> bits) & 0xFFU));
      }
    }
    return out;
  }

  // The percent-decoded relative path of a buffer URI, or nothing when it may not be passed to the loader.
  inline std::optional<std::string> bufferPath(std::string_view uri) {
    std::string path;
    for (std::size_t k = 0; k < uri.size(); ++k) {
      if (uri[k] != '%') {
        path.push_back(uri[k]);
        continue;
      }
      unsigned byte = 0;
      const char* end = uri.data() + std::min(k + 3, uri.size());
      const auto [ptr, ec] = std::from_chars(uri.data() + k + 1, end, byte, 16);
      if (k + 3 > uri.size() || ec != std::errc{} || ptr != end) {
        return std::nullopt;
      }
      path.push_back(static_cast<char>(byte));
      k += 2;
    }
    if (path.empty() || path.front() == '/' || path.contains(':') || path.contains('\\') || path.contains('\0')) {
      return std::nullopt;
    }
    for (const auto segment : std::views::split(path, '/')) {
      if (std::string_view(segment) == "..") {
        return std::nullopt;
      }
    }
    return path;
  }

  // The JSON text and, for GLB, the binary chunk.
  struct Container {
    std::string_view json;
    std::optional<std::string_view> binary;
  };

  // Splits a GLB file into its chunks; any other bytes are the JSON text.
  inline std::expected<Container, std::string> splitContainer(std::string_view bytes) {
    if (!bytes.starts_with("glTF")) {
      return Container{.json = bytes, .binary = std::nullopt};
    }
    const auto version = mesh_io::readScalar<std::uint32_t>(bytes, 4);
    const auto length = mesh_io::readScalar<std::uint32_t>(bytes, 8);
    if (!version || !length || *version != 2 || *length > bytes.size()) {
      return std::unexpected("the GLB header is not version 2 or claims more bytes than the file holds");
    }
    bytes = bytes.substr(0, *length);
    const auto jsonLength = mesh_io::readScalar<std::uint32_t>(bytes, 12);
    const auto jsonType = mesh_io::readScalar<std::uint32_t>(bytes, 16);
    if (!jsonLength || !jsonType || *jsonType != kJsonChunk || !mesh_io::fits(bytes.size(), 20, *jsonLength)) {
      return std::unexpected("the GLB file has no complete JSON chunk first");
    }
    Container container{.json = bytes.substr(20, *jsonLength), .binary = std::nullopt};
    const std::size_t next = 20 + std::size_t{*jsonLength};
    const auto binLength = mesh_io::readScalar<std::uint32_t>(bytes, next);
    const auto binType = mesh_io::readScalar<std::uint32_t>(bytes, next + 4);
    if (binLength && binType && *binType == kBinChunk) {
      if (!mesh_io::fits(bytes.size(), next + 8, *binLength)) {
        return std::unexpected("the GLB binary chunk is cut short");
      }
      container.binary = bytes.substr(next + 8, *binLength);
    }
    return container;
  }
} // namespace luciole::formats::gltf_io
