#include "workbench/flame_scene.h"

#include "workbench/nest_labels.h"
#include "workbench/rounding.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr double kLeft = 8;
    constexpr double kRight = 12;
    constexpr double kMinSpan = 40;
    constexpr std::array<double, 5> kAncestorStrength{0.8, 0.68, 0.58, 0.5, 0.44};
    constexpr std::array<double, 3> kAheadStrength{1, 0.8, 0.62};
    constexpr double kFarStrength = 0.6;
    constexpr double kMoreStrength = 0.9;
    constexpr double kPinnedStrength = 0.85;
    constexpr double kAccentBelow = 10;
    constexpr double kNameMin = 40;
    constexpr double kAncestorNameMin = 60;
    constexpr double kValueMin = 90;
    constexpr double kStackedMin = 34;
    constexpr double kLabelMinH = 14;
    constexpr double kLabelMinW = 24;
    constexpr double kTextInset = 12;
    constexpr double kBoxGapFrom = 6;

    double strengthOf(const FlameRow& row) {
      switch (row.kind) {
      case FlameRowKind::Far:
        return kFarStrength;
      case FlameRowKind::Ancestor:
        return kAncestorStrength[std::clamp<std::size_t>(row.level, 1, kAncestorStrength.size()) - 1];
      case FlameRowKind::Ahead:
        return kAheadStrength[std::clamp<std::size_t>(row.level, 1, kAheadStrength.size()) - 1];
      case FlameRowKind::Bar:
        return 1.0;
      }
      return 1.0;
    }

    // The label of a named folder ahead at least kValueMin wide: the value whole under the name, or whole at the
    // right end with the name fitted before it; the name alone when the value or the name has no room.
    FlameLabel valueLabel(
        const Tree& tree, const LayoutSettings& settings, const FlameRow& row, const FlameBox& box,
        const LabelFont& font, const FlameFonts& fonts, FlameItem& item
    ) {
      const double inside = box.w - kTextInset;
      const auto value = measureLabel(tree, item.node, settings.sizeBy);
      const double width = labelTextWidth(value, fonts.value);
      if (value.empty() || !(width <= inside)) {
        return FlameLabel::Name;
      }
      if (row.h >= kStackedMin) {
        return FlameLabel::StackedNameValue;
      }
      const auto measure = [&font](std::string_view text) { return labelTextWidth(text, font); };
      if (fitNestText(tree.nodes[item.node].name, inside - width - kFlameValueGap, measure).empty()) {
        return FlameLabel::Name;
      }
      item.valueWidth = width;
      return FlameLabel::NameValue;
    }

    // Name, name and value, count, or nothing, by the drawn box and whether the name fits whole.
    void labelItem(
        const Tree& tree, const LayoutSettings& settings, const FlameRow& row, const FlameFonts& fonts, FlameItem& item
    ) {
      const bool ahead = row.kind == FlameRowKind::Ahead;
      const bool first = ahead && row.level == 1;
      item.style = first ? FlameLabelStyle::FirstAhead : ahead ? FlameLabelStyle::Ahead : FlameLabelStyle::Ancestor;
      item.label = FlameLabel::None;
      if (item.far || item.bar) {
        return;
      }
      if (item.more) {
        item.style = FlameLabelStyle::Ahead;
      }
      const auto box = flameBox(item.x, item.w, row.h, false);
      const double nameMin = ahead         ? kNameMin
          : row.level <= kFlameNamedStrata ? kAncestorNameMin
                                           : std::numeric_limits<double>::infinity();
      const LabelFont& font = first ? fonts.firstAhead : ahead ? fonts.ahead : fonts.ancestor;
      const bool fits = ahead
          && !item.more
          && hasNode(tree, item.node)
          && labelTextWidth(tree.nodes[item.node].name, font) <= box.w - kTextInset;
      if (row.h < kLabelMinH || box.w < kLabelMinW || (box.w < nameMin && !fits)) {
        return;
      }
      if (item.more) {
        item.label = FlameLabel::Count;
        return;
      }
      item.label = ahead && box.w >= kValueMin && hasNode(tree, item.node)
          ? valueLabel(tree, settings, row, box, font, fonts, item)
          : FlameLabel::Name;
    }

    void decorate(
        const Tree& tree, const LayoutSettings& settings, const FlameRow& row, const FlameFonts& fonts, FlameItem& item
    ) {
      item.strength = strengthOf(row);
      if (item.more) {
        item.strength = kMoreStrength;
      } else if (item.pinned) {
        item.strength = std::max(item.strength, kPinnedStrength);
        item.accentFill = row.h < kAccentBelow;
      }
      labelItem(tree, settings, row, fonts, item);
    }

    // From the top of the first stratum ahead to the bottom of the last, across the scene's span.
    std::optional<Rect> aheadBox(const FlameScene& scene) {
      std::optional<Rect> box;
      for (const auto& row : scene.bands.rows) {
        if (row.kind != FlameRowKind::Ahead) {
          continue;
        }
        const double top = box ? std::min(box->y, row.y) : row.y;
        const double bottom = box ? std::max(box->y + box->height, row.y + row.h) : row.y + row.h;
        box = Rect{.x = scene.x0, .y = top, .width = scene.x1 - scene.x0, .height = bottom - top};
      }
      return box;
    }
  } // namespace

  std::optional<std::size_t> flameItemAt(std::span<const FlameItem> items, double x) {
    std::size_t low = 0;
    std::size_t high = items.size();
    // At most log2(n) + 1 passes.
    while (low < high) {
      const std::size_t mid = low + (high - low) / 2;
      if (x < items[mid].x) {
        high = mid;
      } else if (x >= items[mid].x + items[mid].w) {
        low = mid + 1;
      } else if (x >= items[mid].x) {
        return mid;
      } else {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  NodeId flameFirstLevel(const Tree& tree, NodeId node, NodeId cwd) {
    NodeId current = node;
    for (std::size_t step = 0; step < tree.nodes.size() && hasNode(tree, current); ++step) {
      const NodeId parent = tree.nodes[current].parent;
      if (!hasNode(tree, parent)) {
        return kNoNode;
      }
      if (parent == cwd) {
        return current;
      }
      current = parent;
    }
    return kNoNode;
  }

  FlameBox flameBox(double x, double w, double h, bool bar) {
    const double left = roundHalfUp(x);
    const double right = roundHalfUp(x + w);
    const double gap = right - left >= kBoxGapFrom && !bar ? 1.0 : 0.0;
    const double radius = h >= 16 ? 3.0 : h >= 10 ? 2.0 : 1.0;
    return {.x = left, .w = std::max(1.0, right - left - gap), .r = radius};
  }

  FlameScene flameScene(
      const Tree& tree, const LayoutSettings& settings, double width, double bandHeight, const FlameFonts& fonts
  ) {
    FlameScene scene;
    const NodeId cwd = tree.root;
    const std::span<const NodeId> ancestors = tree.ancestors;
    scene.bands = flameBands(bandHeight, ancestors.size());
    scene.x0 = kLeft;
    scene.x1 = std::max(scene.x0 + kMinSpan, (std::isfinite(width) ? width : 0.0) - kRight);
    auto ahead = flameAhead(tree, settings, cwd, scene.x0, scene.x1, {.levels = scene.bands.ahead, .fonts = fonts});
    scene.empty = ahead.rows[0].empty();
    scene.truncated = ahead.truncated;
    scene.rows.reserve(scene.bands.rows.size());
    for (const auto& row : scene.bands.rows) {
      FlameSceneRow sceneRow;
      sceneRow.row = row;
      switch (row.kind) {
      case FlameRowKind::Far:
        sceneRow.items = flameFarLine(ancestors, scene.bands.strata, scene.x0, scene.x1);
        break;
      case FlameRowKind::Ancestor:
        sceneRow.items = flameStratum(tree, settings, cwd, ancestors, row.level, scene.x0, scene.x1, {.fonts = fonts});
        break;
      case FlameRowKind::Bar: {
        FlameItem bar;
        bar.bar = true;
        bar.node = cwd;
        bar.x = scene.x0;
        bar.w = scene.x1 - scene.x0;
        sceneRow.items.push_back(std::move(bar));
        break;
      }
      case FlameRowKind::Ahead:
        sceneRow.items = std::move(ahead.rows[row.level - 1]);
        break;
      }
      for (auto& item : sceneRow.items) {
        decorate(tree, settings, row, fonts, item);
      }
      scene.rows.push_back(std::move(sceneRow));
    }
    scene.aheadBox = aheadBox(scene);
    return scene;
  }

  std::optional<FlameHit> flameHitAt(const FlameScene& scene, double x, double y) {
    const auto row = flameRowAt(scene.bands.rows, y);
    if (!row || *row >= scene.rows.size()) {
      return std::nullopt;
    }
    const auto item = flameItemAt(scene.rows[*row].items, x);
    if (!item) {
      return std::nullopt;
    }
    return FlameHit{.row = *row, .item = *item};
  }
} // namespace luciole::workbench
