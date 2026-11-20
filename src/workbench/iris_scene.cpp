#include "workbench/iris_scene.h"

#include "workbench/format.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <utility>

namespace luciole::workbench::iris {
  namespace {
    constexpr double kFullRowWidth = 360.0;
    constexpr double kCompactRowWidth = 230.0;
    constexpr double kMetricWidth = 72.0;
    constexpr double kCompactMetricWidth = 56.0;
    constexpr double kInnerBandInset = 1.5;    // degrees kept clear at each end of the inner band
    constexpr double kInnerBandMinPiece = 1.5; // smallest inner band piece, in degrees

    std::vector<NodeId> idsFrom(const std::vector<NodeId>& ids, std::size_t first) {
      return {ids.begin() + static_cast<std::ptrdiff_t>(std::min(first, ids.size())), ids.end()};
    }

    SectorRing mainRing(
        const Tree& tree, const Scene& scene, const Entries& list, const ring::ArcFit& fit, const SceneInput& input
    ) {
      SectorRing main{
          .level = 0, .folder = scene.folder, .band = {.r0 = scene.dial.rIn, .r1 = scene.dial.rOut}, .sectors = {}
      };
      const auto range = colorRange(tree, scene.folder, input.settings.colorBy, input.settings.showHidden);
      const auto starts = ring::arcStarts(fit.angles);
      main.sectors.reserve(fit.angles.size());
      for (std::size_t i = 0; i < fit.angles.size(); ++i) {
        const ring::Arc arc{.a0 = starts[i], .a1 = starts[i] + fit.angles[i]};
        if (i < fit.shown) {
          const auto id = list.ids[i];
          main.sectors.push_back(
              {.kind = SectorKind::Entry,
               .node = id,
               .merged = {},
               .arc = arc,
               .onPath = false,
               .color = nodeColor(tree, id, input.settings.colorBy, range, input.palette)}
          );
        } else {
          main.sectors.push_back(
              {.kind = SectorKind::More,
               .node = kNoNode,
               .merged = idsFrom(list.ids, fit.shown),
               .arc = arc,
               .onPath = false,
               .color = {}}
          );
        }
      }
      return main;
    }

    // One ring of `folder`'s entries; `onPath` is added when it is not a visible entry.
    SectorRing contextRing(
        const Tree& tree, const ring::Dial& dial, std::size_t level, NodeId folder, NodeId onPath, ring::Band band,
        const SceneInput& input
    ) {
      auto list = entries(tree, folder, input.settings);
      const auto found = std::ranges::find(list.ids, onPath);
      auto pathIndex = static_cast<std::size_t>(found - list.ids.begin());
      if (found == list.ids.end()) {
        list.ids.push_back(onPath);
        list.weights.push_back(weight(tree, onPath, input.settings));
        pathIndex = list.ids.size() - 1;
      }
      const double middle = (band.r0 + band.r1) / 2;
      const double merge = dial.compact ? std::max(kContextMergeDegrees, kCompactSegmentLength / middle / ring::kRadian)
                                        : kContextMergeDegrees;
      const auto split = ring::ringSegments(list.weights, pathIndex, merge);
      const auto range = colorRange(tree, folder, input.settings.colorBy, input.settings.showHidden);
      SectorRing result{.level = level, .folder = folder, .band = band, .sectors = {}};
      result.sectors.reserve(split.segments.size());
      for (const auto& segment : split.segments) {
        if (segment.merged) {
          std::vector<NodeId> merged;
          merged.reserve(split.merged.size());
          for (const auto index : split.merged) {
            merged.push_back(list.ids[index]);
          }
          result.sectors.push_back(
              {.kind = SectorKind::Rest,
               .node = kNoNode,
               .merged = std::move(merged),
               .arc = segment.arc,
               .onPath = false,
               .color = {}}
          );
          continue;
        }
        const auto id = list.ids[segment.entry];
        result.sectors.push_back(
            {.kind = SectorKind::Entry,
             .node = id,
             .merged = {},
             .arc = segment.arc,
             .onPath = id == onPath,
             .color = nodeColor(tree, id, input.settings.colorBy, range, input.palette)}
        );
      }
      return result;
    }

    std::vector<SectorRing> contextRings(const Tree& tree, const ring::Dial& dial, const SceneInput& input) {
      const auto bands = ring::ringBands(dial.rOut, dial.thickness, dial.gap);
      const auto count = std::min({ring::kContextRings, tree.ancestors.size(), bands.size()});
      std::vector<SectorRing> rings;
      rings.reserve(count);
      for (std::size_t k = 0; k < count; ++k) {
        const auto folder = tree.ancestors[k];
        const auto onPath = k == 0 ? tree.root : tree.ancestors[k - 1];
        if (!hasNode(tree, folder) || !hasNode(tree, onPath)) {
          break;
        }
        rings.push_back(contextRing(tree, dial, k + 1, folder, onPath, bands[k], input));
      }
      return rings;
    }

    LabelRow
    labelRow(const Tree& tree, const Scene& scene, std::size_t index, ring::Side side, const SceneInput& input) {
      const auto& dial = scene.dial;
      const auto& sector = scene.main.sectors[index];
      LabelRow row;
      row.sector = index;
      row.side = side;
      row.degrees = (sector.arc.a0 + sector.arc.a1) / 2;
      const auto& column = side == ring::Side::Right ? dial.right : dial.left;
      const double width = std::min(column.x1 - column.x0, dial.compact ? kCompactRowWidth : kFullRowWidth);
      row.box = {
          .x = side == ring::Side::Right ? column.x0 : column.x1 - width,
          .y = 0,
          .width = width,
          .height = ring::kRowHeight
      };
      row.nameWidth =
          dial.compact ? width - 8 - kCompactMetricWidth - 2 * 8 - 14 : width - 8 - 18 - kMetricWidth - 3 * 8 - 22;
      if (sector.kind == SectorKind::More) {
        row.name = std::format("+{} more", countText(static_cast<double>(sector.merged.size())));
      } else {
        row.name = tree.nodes[sector.node].name;
        row.metric = measureLabel(tree, sector.node, input.settings.sizeBy);
      }
      return row;
    }

    void placeColumn(std::vector<LabelRow>& rows, const ring::Dial& dial) {
      std::vector<double> ideal;
      ideal.reserve(rows.size());
      for (const auto& row : rows) {
        ideal.push_back(ring::rowIdeal(dial, row.degrees));
      }
      const auto placed = ring::placeRows(ideal, ring::kRowHeight, dial.yMin, dial.yMax);
      for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].y = placed[i];
        rows[i].box.y = placed[i] - ring::kRowHeight / 2;
      }
    }

    LabelColumns labelColumns(const Tree& tree, const Scene& scene, std::size_t split, const SceneInput& input) {
      LabelColumns columns;
      for (std::size_t i = 0; i < scene.main.sectors.size(); ++i) {
        const auto side = i < split ? ring::Side::Right : ring::Side::Left;
        (side == ring::Side::Right ? columns.right : columns.left).push_back(labelRow(tree, scene, i, side, input));
      }
      std::ranges::reverse(columns.left);
      placeColumn(columns.right, scene.dial);
      placeColumn(columns.left, scene.dial);
      std::vector<ring::LeaderItem> items;
      items.reserve(columns.right.size() + columns.left.size());
      for (const auto* column : {&columns.right, &columns.left}) {
        for (const auto& row : *column) {
          items.push_back({.degrees = row.degrees, .y = row.y, .side = row.side});
        }
      }
      auto routes = ring::routeLeaders(scene.dial, items);
      std::size_t next = 0;
      for (auto* column : {&columns.right, &columns.left}) {
        for (auto& row : *column) {
          row.leader = std::move(routes.paths[next++]);
        }
      }
      columns.mode = routes.mode == ring::LeaderMode::Straight ? RowMode::Straight : RowMode::Lanes;
      return columns;
    }

    // Every arc goes to the right column. A wide host shows them as a legend centered on the dial; otherwise the
    // rows are not shown and serve the keyboard only.
    LabelColumns legendRows(const Tree& tree, const Scene& scene, const SceneInput& input) {
      const auto& dial = scene.dial;
      LabelColumns columns{.right = {}, .left = {}, .mode = RowMode::Legend};
      const auto count = scene.main.sectors.size();
      columns.right.reserve(count);
      for (std::size_t j = 0; j < count; ++j) {
        auto row = labelRow(tree, scene, j, ring::Side::Right, input);
        row.shown = dial.legend;
        if (dial.legend) {
          row.y = dial.center.y + (static_cast<double>(j) - static_cast<double>(count - 1) / 2) * ring::kRowHeight;
          row.box.y = row.y - ring::kRowHeight / 2;
        } else {
          row.box = {};
          row.nameWidth = 0;
        }
        columns.right.push_back(std::move(row));
      }
      return columns;
    }

    // Names inside the arcs, as long as they fit and never under 3 code points: at most 38 tries per arc.
    std::vector<ArcLabel> arcLabels(const Tree& tree, const Scene& scene, double advance) {
      std::vector<ArcLabel> labels;
      for (std::size_t i = 0; i < scene.main.sectors.size(); ++i) {
        const auto& sector = scene.main.sectors[i];
        if (sector.kind != SectorKind::Entry) {
          continue;
        }
        const auto& name = tree.nodes[sector.node].name;
        const auto full = std::min(codePointCount(name), kArcLabelMaxChars);
        if (full == 0) {
          continue;
        }
        const auto shortest = std::min<std::size_t>(3, full);
        for (std::size_t count = full; count >= shortest; --count) {
          auto text = middleEllipsis(name, count);
          const double width = std::ceil(advance * static_cast<double>(codePointCount(text))) + 2.0;
          const auto spot = ring::labelSpot(scene.dial, sector.arc, width, kArcLabelHeight);
          if (spot.has_value()) {
            labels.push_back(
                {.sector = i,
                 .text = std::move(text),
                 .center = *spot,
                 .width = width,
                 .height = kArcLabelHeight,
                 .ink = sector.color.ink}
            );
            break;
          }
        }
      }
      return labels;
    }
  } // namespace

  std::optional<Scene> buildScene(const Tree& tree, const SceneInput& input) {
    if (!hasNode(tree, tree.root)) {
      return std::nullopt;
    }
    const auto rings = std::min(ring::kContextRings, tree.ancestors.size());
    auto dial =
        input.compact ? ring::compactDial(input.width, input.height, rings) : ring::fullDial(input.width, input.height);
    if (!dial.has_value()) {
      return std::nullopt;
    }
    Scene scene;
    scene.dial = std::move(*dial);
    scene.folder = tree.root;
    const auto list = entries(tree, tree.root, input.settings);
    scene.entryCount = list.ids.size();
    const bool compact = scene.dial.compact;
    const auto fit = compact ? ring::fitCompact(list.weights, compactArcCount(scene.dial), kMinArcDegrees)
                             : ring::fitCount(list.weights, scene.dial.rowsFit, kMinArcDegrees);
    scene.main = mainRing(tree, scene, list, fit, input);
    scene.rings = contextRings(tree, scene.dial, input);
    scene.rows = compact ? legendRows(tree, scene, input) : labelColumns(tree, scene, fit.split, input);
    if (compact && input.labelAdvance > 0 && std::isfinite(input.labelAdvance)) {
      scene.arcLabels = arcLabels(tree, scene, input.labelAdvance);
    }
    TextMeasure measure{.font = input.font, .boldFont = input.boldFont, .advance = TextMeasure{}.advance};
    if (input.labelAdvance > 0 && std::isfinite(input.labelAdvance)) {
      measure.advance = input.labelAdvance;
    }
    scene.hub = hubLayout(
        tree, scene.dial, scene.folder, input.trail, hubNote(tree, scene.folder, scene.entryCount, input.filter),
        measure
    );
    scene.stack = ancestorStack(tree, scene.dial, measure);
    scene.readout = readout(scene.dial);
    scene.settings = input.settings;
    return scene;
  }

  std::string hubNote(const Tree& tree, NodeId folder, std::size_t entryCount, std::string_view filter) {
    if (entryCount > 0 || !hasNode(tree, folder)) {
      return {};
    }
    const auto& node = tree.nodes[folder];
    if (node.partial) {
      return "Loading…";
    }
    switch (folderState(node)) {
    case FolderState::NotMeasured:
      return "Not measured here";
    case FolderState::Unreadable:
      return "This folder could not be read";
    case FolderState::OtherFilesystem:
      return "On another filesystem, not scanned";
    case FolderState::NotExpanded:
      return std::format("Not expanded in this snapshot: {}", itemsLabel(tree, folder));
    case FolderState::Listed:
      break;
    }
    const auto all = children(tree, folder);
    if (node.items > 0 && all.empty()) {
      return std::format("Not listed yet: {}", itemsLabel(tree, folder));
    }
    if (!filter.empty()) {
      return std::format("Nothing matches \u201C{}\u201D", middleEllipsis(filter, kFilterNoteChars));
    }
    const auto hidden = static_cast<std::size_t>(std::ranges::count_if(all, [&tree](NodeId id) {
      return hasNode(tree, id) && tree.nodes[id].hidden;
    }));
    if (hidden > 0) {
      return std::format(
          "Only {} hidden {} (Ctrl+H shows them)", countText(static_cast<double>(hidden)),
          hidden == 1 ? "entry" : "entries"
      );
    }
    return "This folder is empty";
  }

  std::size_t compactArcCount(const ring::Dial& dial) {
    if (dial.legend) {
      return std::min<std::size_t>(8, dial.rowsFit > 1 ? dial.rowsFit - 1 : 1);
    }
    if (dial.rOut >= 95.0) {
      return 7;
    }
    return dial.rOut >= 70.0 ? 6 : 5;
  }

  std::optional<InnerBand> innerBand(
      const Tree& tree, const Scene& scene, NodeId node, const LayoutSettings& settings, const ColorPalette& palette
  ) {
    const auto found = std::ranges::find_if(scene.main.sectors, [node](const Sector& sector) {
      return sector.kind == SectorKind::Entry && sector.node == node;
    });
    if (found == scene.main.sectors.end() || !hasNode(tree, node)) {
      return std::nullopt;
    }
    const auto arc = found->arc;
    if (!tree.nodes[node].folder || arc.a1 - arc.a0 < kInnerBandMinDegrees || children(tree, node).empty()) {
      return std::nullopt;
    }
    const auto list = entries(tree, node, settings);
    if (list.ids.empty()) {
      return std::nullopt;
    }
    const auto pieces = ring::compose(
        list.weights, {.a0 = arc.a0 + kInnerBandInset, .a1 = arc.a1 - kInnerBandInset}, kInnerBandMinPiece
    );
    const auto range = colorRange(tree, node, settings.colorBy, settings.showHidden);
    const double inner = scene.dial.rIn;
    InnerBand band{
        .arc = arc,
        .backing = {.r0 = inner, .r1 = inner + 17.0},
        .band = {.r0 = inner + 4.0, .r1 = inner + 14.0},
        .sectors = {}
    };
    band.sectors.reserve(pieces.size());
    for (const auto& piece : pieces) {
      if (piece.merged) {
        band.sectors.push_back(
            {.kind = SectorKind::Rest, .node = kNoNode, .merged = {}, .arc = piece.arc, .onPath = false, .color = {}}
        );
        continue;
      }
      const auto id = list.ids[piece.entry];
      band.sectors.push_back(
          {.kind = SectorKind::Entry,
           .node = id,
           .merged = {},
           .arc = piece.arc,
           .onPath = false,
           .color = nodeColor(tree, id, settings.colorBy, range, palette)}
      );
    }
    return band;
  }
} // namespace luciole::workbench::iris
