#pragma once

#include "workbench/flame_row.h"
#include "workbench/measure.h"
#include "workbench/tree.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The folders of the flame graph: the strata ahead of the current folder, the ancestor strata and the far line.
namespace luciole::workbench {
  // Ancestor strata, nearest first, whose folders are named.
  inline constexpr std::size_t kFlameNamedStrata = 2;

  enum class FlameLabel : std::uint8_t { None, Name, NameValue, StackedNameValue, Count };
  enum class FlameLabelStyle : std::uint8_t { FirstAhead, Ahead, Ancestor };

  struct FlameItem {
    NodeId node = kNoNode;    // the folder drawn; kNoNode for a merged piece
    NodeId owner = kNoNode;   // merged piece: the folder whose entries it holds
    std::vector<NodeId> list; // merged piece: the folders it holds
    std::size_t level = 0;    // strata ahead count from 0; ancestor stratum k; far line strata + 1
    double x = 0;
    double w = 0;
    std::size_t parent = kNoFlameItem; // strata ahead: index of the item above it in the previous row
    std::size_t top = kNoFlameItem;    // strata ahead: index of the item it lies under in the first row
    bool more = false;
    bool pinned = false; // the on-path segment of an ancestor stratum
    bool far = false;
    bool bar = false;
    double strength = 1;     // how far the fill moves from the surface toward the node's color
    bool accentFill = false; // a thin pinned segment filled with the accent
    FlameLabel label = FlameLabel::None;
    FlameLabelStyle style = FlameLabelStyle::Ahead;
    double valueWidth = 0; // NameValue: the value's width, drawn whole at the box's right end
  };

  struct FlameAheadOptions {
    std::size_t levels = 3;
    std::optional<double> min = std::nullopt;       // narrowest segment; 28, 18, 18 by stratum when absent
    double cap = kFlameRowCap;                      // 0.15 reads as 0.10 on the first stratum
    std::size_t budget = 60000;                     // child entries read below the first stratum
    std::optional<FlameFonts> fonts = std::nullopt; // label room for the heaviest folders when present
  };

  struct FlameAhead {
    std::array<std::vector<FlameItem>, 3> rows; // each sorted by x
    std::size_t spent = 0;                      // child entries read
    bool truncated = false;                     // the budget left parents without children
  };

  struct FlameStratumOptions {
    std::optional<double> min = std::nullopt; // 28 on the two nearest strata, 18 beyond, when absent
    double cap = kFlameRowCap;
    double pinMin = 120;
    std::optional<FlameFonts> fonts = std::nullopt;
  };

  // The strata of folders below `cwd` over [x0, x1], each folder's segments inside its parent's span. Files are not
  // drawn. Parents are read widest first until the budget runs out.
  FlameAhead flameAhead(
      const Tree& tree, const LayoutSettings& settings, NodeId cwd, double x0, double x1,
      const FlameAheadOptions& options
  );

  // Ancestor stratum k (1 is the parent's folders): the folders of ancestors[k - 1] with the one on the way to `cwd`
  // pinned at least pinMin wide, even when it is hidden. `ancestors` is nearest first.
  std::vector<FlameItem> flameStratum(
      const Tree& tree, const LayoutSettings& settings, NodeId cwd, std::span<const NodeId> ancestors, std::size_t k,
      double x0, double x1, const FlameStratumOptions& options
  );

  // The far line: one equal cell per ancestor from the root (left) to ancestors[strata - 1].
  std::vector<FlameItem> flameFarLine(std::span<const NodeId> ancestors, std::size_t strata, double x0, double x1);

} // namespace luciole::workbench
