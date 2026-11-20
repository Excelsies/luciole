#pragma once

#include "workbench/measure.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace luciole::workbench {
  struct Circle {
    double x = 0;
    double y = 0;
    double r = 0;
    bool operator==(const Circle&) const = default;
  };

  enum class PackError : std::uint8_t { BadRadius, NoSpot };

  // Packs circles given largest first: each next one touches two placed circles at the free spot closest to
  // the origin. The result keeps the input order and is scaled into the unit circle. A radius that is not a
  // positive finite number gives BadRadius; a circle left without a free spot gives NoSpot.
  std::expected<std::vector<Circle>, PackError> packCircles(std::span<const double> radii);

  // Entries shown in one folder before the rest share a "+N more" bubble.
  inline constexpr std::size_t kAtlasMaxShown = 60;
  inline constexpr std::size_t kAtlasMaxCached = 3000;

  // One bubble of a folder, in the folder's own frame (its unit circle).
  struct PackedItem {
    NodeId node = kNoNode; // kNoNode for "+N more"
    bool more = false;
    double x = 0;
    double y = 0;
    double r = 0;
  };

  struct FolderPacking {
    std::vector<PackedItem> items; // heaviest first
    std::vector<NodeId> rest;      // entries behind "+N more", heaviest first
    bool failed = false;           // the weights could not be packed; items is empty
    std::optional<PackedItem> placement(NodeId child) const;
  };

  // Weight of the "+N more" bubble: the rest's weight, capped at the median shown weight, and never below the
  // lightest shown weight.
  double moreWeight(std::span<const double> shownWeights, double restWeight);

  // The first 60 visible entries plus `extra` (children of `folder` that must be shown wherever they rank;
  // other ids are ignored), and a "+N more" bubble for the others, packed into a circle of radius 0.9 that
  // sits 0.06 below the center. Empty for an unknown folder.
  FolderPacking
  packFolder(const Tree& tree, NodeId folder, const LayoutSettings& settings, std::span<const NodeId> extra);

  // Packings by folder, dropped when the size measure, emphasis or hidden setting changes. Holds at most
  // 3000 packings, evicting the oldest. Clear it when the tree changes.
  class PackingCache {
  public:
    // Packing of `folder`, with every node of `force` whose parent is `folder` included. An unknown folder
    // gives an empty packing that is not cached.
    std::shared_ptr<const FolderPacking>
    packing(const Tree& tree, NodeId folder, const LayoutSettings& settings, std::span<const NodeId> force);
    void clear();
    std::size_t size() const;

  private:
    using Key = std::pair<NodeId, std::vector<NodeId>>;
    std::shared_ptr<const FolderPacking> remember(Key key, FolderPacking packing);
    std::map<Key, std::shared_ptr<const FolderPacking>> m_packings;
    std::deque<Key> m_order;
    std::optional<LayoutSettings> m_settings;
  };
} // namespace luciole::workbench
