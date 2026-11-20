#include "workbench/nest_rings.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr double kMinSegment = 6;
    constexpr double kPinMinimum = 10;

    struct RingInput {
      const Tree& tree;
      const LayoutSettings& settings;
      const ColorPalette& palette;
      std::span<const NodeId> ancestors;
    };

    PartitionOptions minimumOnly(double minLength) {
      PartitionOptions options;
      options.minLength = minLength;
      return options;
    }

    std::optional<NestRingLabel>
    ringLabel(const Tree& tree, const LayoutSettings& settings, const NestRing& ring, const NestPiece& piece) {
      if (piece.kind != NestPieceKind::Segment
          || !hasNode(tree, piece.node)
          || ring.line
          || ring.thickness <= 7
          || ring.level > 2
          || (ring.level == 2 && piece.s1 - piece.s0 < 80)) {
        return std::nullopt;
      }
      const auto& g = ring.band;
      std::optional<std::size_t> best;
      double bestA = 0;
      double bestB = 0;
      for (const std::size_t side : {0U, 2U, 4U, 6U}) {
        const double a = std::max(piece.s0, g.starts[side]);
        const double b = std::min(piece.s1, g.starts[side] + g.lengths[side]);
        if (b - a - 14 >= 36 && (!best || b - a > bestB - bestA)) {
          best = side;
          bestA = a;
          bestB = b;
        }
      }
      const double fontSize = ring.level == 1 ? 12 : 11;
      if (!best || ring.thickness < fontSize + 4) {
        return std::nullopt;
      }
      const auto mid = pointAt(g, (bestA + bestB) / 2);
      NestRingLabel label{
          .name = tree.nodes[piece.node].name,
          .measure = ring.level == 1 ? measureLabel(tree, piece.node, settings.sizeBy) : std::string{},
          .center = {.x = mid.x, .y = mid.y},
          .turn = *best == 6 ? NestTurn::CounterClockwise : (*best == 2 ? NestTurn::Clockwise : NestTurn::None),
          .fontSize = fontSize,
          .maxWidth = bestB - bestA - 14,
      };
      return label;
    }

    // Box of a segment: the outline's bounds when it lies on one straight side, else 4 px around its middle.
    TreemapRect segmentBox(const Band& g, double s0, double s1) {
      const auto mid = pointAt(g, (s0 + s1) / 2);
      const std::size_t first = bandPiece(g, s0);
      if (first != bandPiece(g, s1 - 1e-6) || first % 2 != 0) {
        return {.x = mid.x - 2, .y = mid.y - 2, .w = 4, .h = 4};
      }
      const auto outline = segmentPolygon(g, s0, s1);
      if (outline.empty()) {
        return {.x = mid.x - 2, .y = mid.y - 2, .w = 4, .h = 4};
      }
      const auto [minX, maxX] = std::ranges::minmax(outline | std::views::transform(&TreemapPoint::x));
      const auto [minY, maxY] = std::ranges::minmax(outline | std::views::transform(&TreemapPoint::y));
      return {.x = minX, .y = minY, .w = maxX - minX, .h = maxY - minY};
    }

    // Strip from the inner edge of an outline toward the center, `extension` deep.
    std::vector<TreemapPoint> bridgeOf(const std::vector<TreemapPoint>& outline, double extension) {
      const std::size_t half = outline.size() / 2;
      std::vector<TreemapPoint> inner;
      std::vector<TreemapPoint> reach;
      inner.reserve(half);
      reach.reserve(half);
      for (std::size_t i = 0; i < half; ++i) {
        const auto& outer = outline[i];
        const auto& in = outline[outline.size() - 1 - i];
        const double distance = std::hypot(outer.x - in.x, outer.y - in.y);
        const double d = distance > 0 ? distance : 1.0;
        inner.push_back(in);
        reach.push_back(
            {.x = in.x - ((outer.x - in.x) / d) * extension, .y = in.y - ((outer.y - in.y) / d) * extension}
        );
      }
      inner.insert(inner.end(), reach.rbegin(), reach.rend());
      return inner;
    }

    void addSegmentShape(NestPiece& piece, const NestRing& ring) {
      const double length = piece.s1 - piece.s0;
      const double gap = length > 10 ? 2.0 : 0.8;
      piece.rect = segmentBox(ring.band, piece.s0, piece.s1);
      piece.outline = segmentPolygon(ring.band, piece.s0 + gap / 2, piece.s1 - gap / 2);
      if (piece.onPath && !ring.line) {
        piece.bridge = bridgeOf(piece.outline, ring.extension);
      }
    }

    void addLinePieces(const RingInput& in, NestScene& scene, NestRing& ring, std::size_t from) {
      std::vector<BandItem> items;
      for (const auto id : in.ancestors.subspan(from)) {
        items.push_back({.key = id, .share = 1});
      }
      for (auto& segment : partition(items, ring.band.perimeter, minimumOnly(kMinSegment))) {
        // A merged tail stands for all its ancestors and takes the nearest one's color.
        const auto key = segment.merged ? segment.keys.front() : segment.key;
        NestPiece piece;
        piece.kind = NestPieceKind::LineSegment;
        piece.node = key;
        if (segment.merged) {
          piece.nodes = std::move(segment.keys);
        }
        piece.s0 = segment.s0;
        piece.s1 = segment.s1;
        piece.ring = scene.rings.size();
        piece.onPath = true;
        piece.folder = true;
        piece.color = nodeColor(in.tree, key, in.settings, in.palette);
        addSegmentShape(piece, ring);
        scene.pieces.push_back(std::move(piece));
      }
    }

    void addRingPieces(const RingInput& in, NestScene& scene, NestRing& ring, NodeId onPath) {
      const auto list = entries(in.tree, ring.folder, in.settings);
      std::vector<BandItem> items;
      items.reserve(list.ids.size() + 1);
      for (std::size_t i = 0; i < list.ids.size(); ++i) {
        items.push_back({.key = list.ids[i], .share = list.total > 0 ? list.weights[i] / list.total : 0.0});
      }
      if (!std::ranges::contains(list.ids, onPath)) {
        items.push_back({.key = onPath, .share = 0});
      }
      const auto range = colorRange(in.tree, ring.folder, in.settings.colorBy, in.settings.showHidden);
      const PartitionOptions options{.minLength = kMinSegment, .pinKey = onPath, .pinMinimum = kPinMinimum};
      auto segments = snapToStraights(ring.band, partition(items, ring.band.perimeter, options), kMinSegment);
      for (auto& segment : segments) {
        NestPiece piece;
        piece.s0 = segment.s0;
        piece.s1 = segment.s1;
        piece.ring = scene.rings.size();
        if (segment.merged) {
          piece.kind = NestPieceKind::MergedSegment;
          piece.node = ring.folder;
          piece.nodes = std::move(segment.keys);
        } else {
          piece.kind = NestPieceKind::Segment;
          piece.node = segment.key;
          piece.onPath = segment.key == onPath;
          piece.folder = hasNode(in.tree, segment.key) && in.tree.nodes[segment.key].folder;
          piece.color = nodeColor(in.tree, segment.key, in.settings.colorBy, range, in.palette);
        }
        addSegmentShape(piece, ring);
        if (auto label = ringLabel(in.tree, in.settings, ring, piece)) {
          piece.label = std::move(*label);
        }
        scene.pieces.push_back(std::move(piece));
      }
    }
  } // namespace

  std::vector<NestRingPlan> nestRingPlan(std::size_t depth, const NestProfile& profile) {
    std::vector<NestRingPlan> plan;
    const std::size_t rings = profile.ringThickness.size();
    if (depth > rings) {
      plan.push_back({.level = kNestLineLevel, .thickness = profile.lineThickness, .from = rings});
    }
    for (std::size_t level = std::min(depth, rings); level >= 1; --level) {
      plan.push_back({.level = level, .thickness = profile.ringThickness[level - 1], .from = 0});
    }
    return plan;
  }

  NestFrame nestFrame(std::span<const NestRingPlan> plan, double width, double height, const NestProfile& profile) {
    NestFrame frame;
    TreemapRect rect{.x = profile.pad, .y = profile.pad, .w = width - 2 * profile.pad, .h = height - 2 * profile.pad};
    for (std::size_t i = 0; i < plan.size(); ++i) {
      frame.rects.push_back(rect);
      rect = inset(rect, plan[i].thickness + (i + 1 == plan.size() ? profile.coreGap : profile.gap));
    }
    frame.radii.resize(plan.size());
    double radius = profile.coreRadius;
    for (std::size_t i = plan.size(); i-- > 0;) {
      radius += (i + 1 == plan.size() ? profile.coreGap : profile.gap) + plan[i].thickness;
      frame.radii[i] = radius;
    }
    frame.core = rect;
    return frame;
  }

  void addNestRings(
      const Tree& tree, const LayoutSettings& settings, const ColorPalette& palette, std::span<const NodeId> ancestors,
      std::span<const NestRingPlan> plan, const NestFrame& frame, NestScene& scene
  ) {
    const RingInput in{.tree = tree, .settings = settings, .palette = palette, .ancestors = ancestors};
    const auto& profile = scene.profile;
    const auto count = std::min({plan.size(), frame.rects.size(), frame.radii.size()});
    for (std::size_t i = 0; i < count; ++i) {
      const bool line = plan[i].level == kNestLineLevel;
      if (line ? plan[i].from > ancestors.size() : plan[i].level == 0 || plan[i].level > ancestors.size()) {
        continue;
      }
      const auto g = band(frame.rects[i], plan[i].thickness, frame.radii[i]);
      if (!g) {
        continue;
      }
      NestRing ring;
      ring.level = plan[i].level;
      ring.line = line;
      ring.thickness = plan[i].thickness;
      ring.extension = i + 1 == plan.size() ? profile.coreGap : profile.gap;
      ring.band = *g;
      ring.firstPiece = scene.pieces.size();
      if (ring.line) {
        addLinePieces(in, scene, ring, plan[i].from);
      } else {
        ring.folder = ancestors[ring.level - 1];
        addRingPieces(in, scene, ring, ring.level == 1 ? scene.folder : ancestors[ring.level - 2]);
      }
      ring.pieceCount = scene.pieces.size() - ring.firstPiece;
      scene.rings.push_back(ring);
    }
  }
} // namespace luciole::workbench
