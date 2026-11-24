#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace luciole::workbench {
  using NodeId = std::uint32_t;
  inline constexpr NodeId kNoNode = 0xFFFFFFFFU;

  struct Node {
    std::string name;
    NodeId parent = kNoNode;
    std::uint32_t firstChild = 0; // index into Tree::children
    std::uint32_t childCount = 0;
    double bytes = 0;    // own size, or everything below for a folder
    double files = 0;    // files below
    double items = 0;    // direct entries
    double modified = 0; // newest change below, seconds
    double visits = 0;
    std::uint8_t category = 0; // a FileCategory value
    bool folder = false;
    bool hidden = false;
    bool partial = false;         // measure still running
    bool lowerBound = false;      // the walk was cut short and is not running: the totals are a lower bound
    bool unreadable = false;      // the folder could not be listed
    bool truncated = false;       // the folder's entries were not expanded in this snapshot
    bool otherFilesystem = false; // the folder is on another filesystem and was not scanned
    bool notEntered = false;      // listed, and the walk ended before it was entered: nothing is known of it
    bool notMeasured = false;     // the location is not a local folder, so nothing below it is measured
  };

  // One immutable snapshot that every layout reads.
  struct Tree {
    std::vector<Node> nodes;
    std::vector<NodeId> children;  // child ids, grouped by parent
    NodeId root = kNoNode;         // the current folder
    std::vector<NodeId> ancestors; // nearest first
    double now = 0;
  };

  inline bool hasNode(const Tree& tree, NodeId id) { return id < tree.nodes.size(); }

  // Child ids of a node; empty for an unknown id or a range outside Tree::children.
  inline std::span<const NodeId> children(const Tree& tree, NodeId id) {
    if (!hasNode(tree, id)) {
      return {};
    }
    const auto& node = tree.nodes[id];
    const std::size_t first = node.firstChild;
    const std::size_t count = node.childCount;
    if (first > tree.children.size() || count > tree.children.size() - first) {
      return {};
    }
    return std::span(tree.children).subspan(first, count);
  }

  // Parents of a node, nearest first. Stops at an unknown id and after one step per node, so a cycle ends.
  inline std::vector<NodeId> parentChain(const Tree& tree, NodeId id) {
    std::vector<NodeId> chain;
    if (!hasNode(tree, id)) {
      return chain;
    }
    for (auto parent = tree.nodes[id].parent; hasNode(tree, parent) && chain.size() < tree.nodes.size();
         parent = tree.nodes[parent].parent) {
      chain.push_back(parent);
    }
    return chain;
  }

  // The parent of a node; none for an unknown id.
  inline NodeId parentOf(const Tree& tree, NodeId id) { return hasNode(tree, id) ? tree.nodes[id].parent : kNoNode; }

  // The node and its parents, top first; empty for an unknown id. Bounded like parentChain.
  inline std::vector<NodeId> pathFromTop(const Tree& tree, NodeId id) {
    auto path = parentChain(tree, id);
    std::ranges::reverse(path);
    if (hasNode(tree, id)) {
      path.push_back(id);
    }
    return path;
  }

  // Number of parents, with the same bound as parentChain.
  inline std::size_t depth(const Tree& tree, NodeId id) {
    std::size_t levels = 0;
    if (!hasNode(tree, id)) {
      return levels;
    }
    for (auto parent = tree.nodes[id].parent; hasNode(tree, parent) && levels < tree.nodes.size();
         parent = tree.nodes[parent].parent) {
      ++levels;
    }
    return levels;
  }

  inline bool visibleEntry(const Node& node, bool showHidden) { return showHidden || !node.hidden; }

  // What a folder's entries in the snapshot can be trusted for; the first flag set wins, in this order.
  enum class FolderState : std::uint8_t {
    Listed,          // the entries are all there
    NotMeasured,     // the location is not a local folder and is not measured
    Unreadable,      // the folder could not be listed
    OtherFilesystem, // the folder is on another filesystem and was not scanned
    NotExpanded,     // the folder's entries were left out of this snapshot
  };

  inline FolderState folderState(const Node& node) {
    if (node.notMeasured) {
      return FolderState::NotMeasured;
    }
    if (node.unreadable) {
      return FolderState::Unreadable;
    }
    if (node.otherFilesystem) {
      return FolderState::OtherFilesystem;
    }
    return node.truncated ? FolderState::NotExpanded : FolderState::Listed;
  }
} // namespace luciole::workbench
