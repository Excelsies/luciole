#pragma once

#include "formats/mesh.h"

#include <string_view>

namespace luciole::formats {
  // Reads binary or ASCII STL. The file is binary when its size is exactly 84 + 50 times the triangle count in
  // the header. Otherwise a file that starts with "solid" is ASCII when it holds a facet or the count does not
  // fit in the file, and any other file is binary when the count fits. Each triangle keeps its own three
  // vertices. Stored facet normals are ignored, so normalsMissing is always set. The ASCII solid name ends at
  // the first line end or before a "facet" token; a facet ends at "endloop" and needs exactly three vertices,
  // and an unfinished last facet is left out.
  Mesh readStl(std::string_view bytes, const MeshLimits& limits = {});
} // namespace luciole::formats
