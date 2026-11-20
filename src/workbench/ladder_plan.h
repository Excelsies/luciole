#pragma once

#include "workbench/measure.h"
#include "workbench/tree.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace luciole::workbench {
  // One bar of the ladder, nearest level first. A merged bar stands for levels firstLevel..level and draws the
  // entries of the farthest one.
  struct LadderBar {
    std::size_t level = 0;
    std::size_t firstLevel = 0;
    double h = 0;
    bool merged = false;
    bool operator==(const LadderBar&) const = default;
  };

  struct LadderLimits {
    double minW = 0;
    double minH = 0;
    double prefH = 0;
    double maxH = 0;
  };

  struct LadderSegment {
    std::size_t index = 0; // entry index
    double x = 0;
    double w = 0;
  };

  struct LadderRest {
    double x = 0;
    double w = 0;
    std::size_t count = 0; // entries it holds
  };

  struct LadderSegments {
    std::vector<LadderSegment> segs;
    std::optional<LadderRest> rest;
  };

  // The entry that must stay visible, with its share of the whole list.
  struct LadderForce {
    std::size_t index = 0;
    double share = 0;
  };

  struct LadderSegmentOptions {
    double min = 3;
    double restCap = 0.15;
    std::size_t maxCount = 40;
    double forceMin = 8; // a value that is not positive reads as 8
    std::optional<LadderForce> force;
  };

  // A drawn entry of one ladder bar, in host coordinates.
  struct LadderPiece {
    NodeId node = kNoNode;
    double x = 0;
    double w = 0;
    bool onPath = false;
    bool labeled = false;
  };

  // One ladder bar in a host, in host coordinates. Bars sit against the bottom edge, nearest lowest.
  struct LadderRow {
    NodeId folder = kNoNode; // whose entries the bar shows
    NodeId onPath = kNoNode; // the entry on the way to the current folder
    std::size_t index = 0;   // 0 is the nearest bar
    std::size_t level = 0;
    std::size_t firstLevel = 0;
    bool merged = false;
    bool named = false; // the folder name is drawn in the gutter
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
    double fade = 1;
    std::vector<LadderPiece> pieces;
    std::optional<LadderRest> rest; // host coordinates
    bool restLabeled = false;
  };

  struct LadderScene {
    std::vector<LadderRow> rows; // nearest first
    bool atTop = false;          // the current folder has no ancestors
  };

  inline constexpr std::size_t kLadderSeparateLevels = 8;
  inline constexpr std::size_t kLadderNearLevels = 4;
  inline constexpr double kLadderGap = 3;
  inline constexpr double kLadderPadLeft = 14;
  inline constexpr double kLadderPadRight = 12;
  inline constexpr double kLadderGutter = 118;
  inline constexpr double kLadderNameRoom = 104; // text room of a gutter name, right aligned before the bar

  // Natural thickness per level: 26, 18, 12, 12, 8, 5, 4, 3, then one merged 3 px bar for the ninth level on.
  std::vector<LadderBar> ladderBars(std::size_t count);

  // Bars plus the gaps between them, without outer padding.
  double ladderHeight(std::span<const LadderBar> bars);

  // Top and bottom padding plus every bar with the gap under it.
  double ladderPlanHeight(std::span<const LadderBar> bars);

  // Bars for `count` ancestors in a host `height` px tall (whole pixels count; NaN or negative reads as 0). The
  // nearest four never go under 12 px and never shrink as the height grows; a short host merges far levels sooner.
  std::vector<LadderBar> ladderPlan(std::size_t count, double height);

  // Limits of the ladder ribbon at a depth: minW 360, minH 76, prefH the natural height, maxH the tallest plan.
  LadderLimits ladderLimits(std::size_t count);

  // Segments of a bar `width` px wide. `shares` are the first entries' fractions of the whole list, heaviest first;
  // `entryCount` is the full list length. Edges are whole pixels and tile the bar; entries under `min` merge into a
  // remainder of at most `restCap`. At most the first 64 entries show, whatever `maxCount` says. A forced entry stays
  // at least `forceMin` wide; one beyond `entryCount` is ignored.
  LadderSegments ladderSegments(
      std::span<const double> shares, std::size_t entryCount, double width, const LadderSegmentOptions& options
  );

  // The ladder ribbon for the tree's current folder in a host of `width` by `height` px. A size that is not finite
  // reads as 0.
  LadderScene ladderScene(const Tree& tree, const LayoutSettings& settings, double width, double height);
} // namespace luciole::workbench
