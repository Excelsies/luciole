#pragma once

#include "formats/mesh.h"

#include <string_view>

namespace luciole::formats {
  // Most elements, and most properties over all elements, a PLY header may declare.
  inline constexpr std::size_t kMaxPlyElements = 256;
  inline constexpr std::size_t kMaxPlyProperties = 1024;

  // Reads PLY in ASCII or either binary byte order. The "vertex" element needs x, y and z and may carry nx, ny
  // and nz, among other properties of any declared type. The "face" element's "vertex_indices" (or
  // "vertex_index") list is fanned from its first index; a face with fewer than three indices adds nothing.
  // Every other element is skipped. Element counts are checked against the bytes left before anything is read.
  Mesh readPly(std::string_view bytes, const MeshLimits& limits = {});
} // namespace luciole::formats
