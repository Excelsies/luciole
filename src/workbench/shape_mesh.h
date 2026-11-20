#pragma once

#include "workbench/shape.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Vertex data that draws a whole shape list with one glDrawArrays(GL_TRIANGLES) call of one GLES2 program.
//
// Every vertex is a ShapeVertex: 24 floats, 96 bytes, tightly packed and interleaved. Attributes, with byte
// offsets into the vertex:
//
//   a_position vec2  0   part-local logical px; the node's transform maps them to the surface
//   a_local    vec2  8   coordinate in the shape's own frame, see below
//   a_params   vec4  16  shape parameters, see below
//   a_fill     vec4  32  fill RGBA in 0..1, straight alpha, already multiplied by the shape's opacity
//   a_stroke   vec4  48  stroke RGBA in 0..1, straight alpha, already multiplied by the shape's opacity
//   a_hatch    vec4  64  hatch RGBA in 0..1, straight alpha, already multiplied by the shape's opacity
//   a_style    vec4  80  x: MeshKind, y: ShapePattern, z: stroke width in px (0: none), w: aa, the width of one
//                        device pixel in logical px (1 / pixel scale)
//
// MeshKind::Box draws Rect, RoundedRect, Circle and Line as one quad per shape, grown by aa on every side.
//   a_local is the offset from the box center in the box's frame (along and across a line), a_params is
//   (half width, half height, corner radius, softness). A circle is a box of half size r with radius r.
//     q = abs(local) - params.xy + params.z
//     d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - params.z
// MeshKind::Sector draws one quad per sector covering its band, grown by aa.
//   a_local is the offset from the center in a frame turned so the middle of the arc points up (-y), a_params is
//   (inner radius, outer radius, half the angular span in radians, softness). A half span of pi or more is a whole
//   ring.
//     rho = length(local)
//     dr = max(params.x - rho, rho - params.y)
//     c = vec2(sin(params.z), cos(params.z)); p = vec2(abs(local.x), -local.y)
//     da = length(p - c * max(dot(p, c), 0.0)) * sign(c.y * p.x - c.x * p.y)
//     d = params.z >= pi ? dr : max(dr, da)
// MeshKind::Strip draws a ribbon tessellated here into columns of three quads: a fringe above, the body, a fringe
//   below. a_local.x is the coverage at the vertex (1 on the body, 0 on the outer fringe edge, the thickness in
//   device pixels on a body thinner than one); a_local.y and a_params are 0. Strips have no stroke and no dashes.
//
// Coverage for Box and Sector: alpha = clamp(0.5 - d / aa, 0.0, 1.0); for Strip: alpha = local.x.
// Stroke for Box and Sector, when z > 0: the band -z <= d <= 0 takes the stroke color, blended over the fill with
//   clamp(0.5 + (d + z) / aa, 0.0, 1.0).
// Softness s = params.w, when above 0, replaces both ramps with smooth steps over [-s, s]: alpha =
//   1.0 - smoothstep(-s, s, d) and the stroke's blend smoothstep(-s, s, d + z). The quad grows by s more.
// ShapePattern::Hatched lays stripes of the hatch color over the fill, kHatchWidth wide along x and
//   centered where position.x + position.y is kHatchWidth / 2 past a whole number of kHatchPeriod. Each stripe
//   fades over one device pixel across it: with t = mod(position.x + position.y - kHatchWidth / 2, kHatchPeriod),
//   across = (min(t, kHatchPeriod - t) - kHatchWidth / 2) * sqrt(0.5), coverage = clamp(0.5 - across / aa, 0, 1).
// ShapePattern::DashedOutline keeps the stroke only where mod(t, kDashLength + kDashGap) < kDashLength, with t
//   the length along the edge: for a box, local.x on the top and bottom sides and local.y on the left and right
//   (the side d is nearest to); for a sector, atan(local.x, -local.y) * rho on the arcs and rho on the straight
//   edges.
// The fragment output is premultiplied: vec4(color.rgb * a, a) with a = alpha * color.a.
namespace luciole::workbench {
  enum class MeshKind : std::uint8_t { Box, Sector, Strip };

  struct ShapeVertex {
    std::array<float, 2> position{};
    std::array<float, 2> local{};
    std::array<float, 4> params{};
    std::array<float, 4> fill{};
    std::array<float, 4> stroke{};
    std::array<float, 4> hatch{};
    std::array<float, 4> style{};
  };
  static_assert(sizeof(ShapeVertex) == 24 * sizeof(float));

  inline constexpr float kHatchPeriod = 6;
  inline constexpr float kHatchWidth = 1.5F;
  inline constexpr float kDashLength = 4;
  inline constexpr float kDashGap = 3;

  inline constexpr std::size_t kQuadVertices = 6;
  // A ribbon takes one segment per 8 device pixels of width, at least 2 and at most kRibbonMaxSegments.
  inline constexpr float kRibbonSegmentPixels = 8;
  inline constexpr std::size_t kRibbonMaxSegments = 32;
  inline constexpr std::size_t kMaxShapeVertices = kRibbonMaxSegments * 3 * kQuadVertices;
  inline constexpr std::size_t kMaxMeshVertices = 65536;

  struct ShapeMesh {
    std::vector<ShapeVertex> vertices; // triangles, shapes in draw order
    std::size_t drawn = 0;             // shapes in the mesh
    std::size_t skipped = 0;           // invalid or invisible shapes left out
    bool truncated = false;            // a shape did not fit under kMaxMeshVertices: it and every later one are out
  };

  // Fills `mesh`, reusing its storage, with the shapes in draw order: ascending order, then list order. Invalid
  // shapes (see shapeValid), shapes with no opacity and shapes with no fill, stroke or stripes to show are skipped; a
  // negative inner radius reads as 0, an opacity over 1 as 1 and a sector span over 360 as a whole ring. A pixel
  // scale that is not positive and finite gives an empty mesh.
  void buildShapeMesh(std::span<const Shape> shapes, float pixelScale, ShapeMesh& mesh);
} // namespace luciole::workbench
