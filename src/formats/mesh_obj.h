#pragma once

#include "formats/mesh.h"

#include <string_view>

namespace luciole::formats {
  // Reads Wavefront OBJ: "v" (x y z, extra values ignored), "vn" and "f" lines; every other line is ignored, as
  // is text after '#'. Face corners take the forms v, v/vt, v//vn and v/vt/vn; a negative index counts back from
  // the last element read before its line, a positive one may name an element defined later. Polygons are fanned
  // from their first corner. Normals are kept only when every corner names one; corners that pair one position
  // with different normals become separate vertices. Positions and normals past maxVertices are not stored and
  // faces that use them are left out. Time is linear in the input size.
  Mesh readObj(std::string_view bytes, const MeshLimits& limits = {});
} // namespace luciole::formats
