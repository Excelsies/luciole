#include "browser/measured_tree.h"

#include "workbench/color.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace {
  using luciole::Location;
  using luciole::MeasureRecord;
  using luciole::MeasureTable;
  using luciole::workbench::kNoNode;
  using luciole::workbench::Node;
  using luciole::workbench::NodeId;

  constexpr std::uint32_t kMaxLevels = 8;
  constexpr std::size_t kMaxAncestors = 256;

  std::uint8_t category(const std::string& name, bool folder) {
    return static_cast<std::uint8_t>(luciole::workbench::fileCategory(name, folder));
  }

  Node folderNode(const MeasureRecord& record) {
    Node node;
    node.name = record.name;
    node.folder = true;
    node.bytes = static_cast<double>(record.totals.apparentBytes);
    node.files = static_cast<double>(record.totals.files);
    node.items = static_cast<double>(record.totals.items);
    node.modified = static_cast<double>(record.totals.newest);
    node.category = category(record.name, true);
    node.hidden = record.flags.hidden;
    node.partial = record.flags.partial;
    node.lowerBound = record.flags.truncated;
    node.unreadable = record.flags.unreadable;
    node.otherFilesystem = record.flags.otherFilesystem;
    return node;
  }

  Node fileNode(const std::string& name, std::uint64_t bytes, std::int64_t modified, bool hidden) {
    Node node;
    node.name = name;
    node.bytes = static_cast<double>(bytes);
    node.modified = static_cast<double>(modified);
    node.category = category(name, false);
    node.hidden = hidden;
    return node;
  }

  // A remainder: `items` entries folded into one node.
  Node remainderNode(const luciole::MeasureRemainder& rest, bool folder, bool partial, bool lowerBound) {
    Node node;
    node.folder = folder;
    node.bytes = static_cast<double>(rest.totals.apparentBytes);
    node.files = static_cast<double>(rest.totals.files);
    node.items = static_cast<double>(rest.count);
    node.modified = static_cast<double>(rest.totals.newest);
    node.category = category({}, folder);
    node.partial = partial;
    node.lowerBound = lowerBound;
    return node;
  }

  // Takes a folder out of the folder remainder it was folded into, clamping the measures at zero.
  void leaveOut(Node& rest, const Node& folder) {
    rest.items = std::max(0.0, rest.items - 1);
    rest.bytes = std::max(0.0, rest.bytes - folder.bytes);
    rest.files = std::max(0.0, rest.files - folder.files);
  }

  class Builder {
  public:
    Builder(const luciole::MeasuredTreeInput& input, const luciole::MeasureView& view)
        : m_input(input), m_view(view), m_levels(std::min(input.levels, kMaxLevels)) {}

    luciole::MeasuredTree build() {
      auto& tree = m_result.tree;
      tree.now = m_input.now;
      tree.root = level(m_input.current, m_input.entries, m_levels, kNoNode);
      auto expected = m_input.current.parent();
      for (const auto& ancestor : m_input.ancestors | std::views::take(kMaxAncestors)) {
        if (!expected || ancestor.location != *expected) {
          break;
        }
        tree.ancestors.push_back(
            level(ancestor.location, ancestor.entries, 1, tree.ancestors.empty() ? tree.root : tree.ancestors.back())
        );
        expected = ancestor.location.parent();
      }
      tree.children.clear();
      for (NodeId id = 0; id < tree.nodes.size(); ++id) {
        tree.nodes[id].firstChild = static_cast<std::uint32_t>(tree.children.size());
        tree.nodes[id].childCount = static_cast<std::uint32_t>(m_children[id].size());
        std::ranges::copy(m_children[id], std::back_inserter(tree.children));
      }
      return std::move(m_result);
    }

  private:
    NodeId add(Node node, NodeId parent, const std::optional<Location>& location) {
      const auto id = static_cast<NodeId>(m_result.tree.nodes.size());
      node.parent = parent;
      node.visits = m_input.visits && location ? m_input.visits(*location) : 0;
      m_result.tree.nodes.push_back(std::move(node));
      m_children.emplace_back();
      if (parent != kNoNode) {
        m_children[parent].push_back(id);
      }
      return id;
    }

    void adopt(NodeId parent, NodeId child) {
      m_result.tree.nodes[child].parent = parent;
      m_children[parent].push_back(child);
    }

    // The location of a child, built only when visits are asked for.
    std::optional<Location> childFor(const std::optional<Location>& folder, const std::string& name) const {
      return m_input.visits && folder ? std::optional(folder->child(name)) : std::nullopt;
    }

    bool fits(NodeId node, std::size_t group) {
      if (m_result.tree.nodes.size() + group <= m_input.maxNodes) {
        return true;
      }
      m_result.tree.nodes[node].truncated = true;
      return false;
    }

    // A folder on the path, measured or listed. Its entries go `levels` deep; `onPath` takes its own entry's place.
    // A record whose folders have no records gives way to the listing.
    NodeId level(
        const Location& location, const std::vector<luciole::ListedEntry>& entries, std::uint32_t levels, NodeId onPath
    ) {
      const auto found = m_view.find(location);
      if (const auto* record = found.entry(); record && (levels == 0 || !record->flags.foldersOmitted)) {
        const auto id = add(folderNode(*record), kNoNode, location);
        m_result.tree.nodes[id].hidden = false;
        expand(*found.table, found.record, id, levels, location, onPath);
        return id;
      }
      return listed(location, entries, levels, onPath);
    }

    // Children of a record: child records, the largest files and the remainders, then their own children. A record
    // whose folders have no records, or that was cut with nothing listed, is flagged truncated instead. `onPath` takes
    // the place of a child record or file of its name, else leaves the folder remainder. Recursion is bounded by
    // kMaxLevels.
    void expand(
        const MeasureTable& table, std::uint32_t index, NodeId node, std::uint32_t levels,
        const std::optional<Location>& location, NodeId onPath
    ) {
      const auto& record = table.records[index];
      const auto files = record.files ? std::span(*record.files) : std::span<const luciole::MeasureFile>();
      const std::size_t group = record.childCount
          + files.size()
          + (record.otherDirectories.count > 0 ? 1 : 0)
          + (record.otherFiles.count > 0 ? 1 : 0);
      const bool omitted = levels > 0 && record.flags.foldersOmitted;
      m_result.tree.nodes[node].truncated |= omitted || (record.flags.truncated && group == 0);
      if (levels == 0 || omitted || !fits(node, group)) {
        if (onPath != kNoNode) {
          adopt(node, onPath);
        }
        return;
      }
      bool adopted = false;
      const auto takesPlace = [&](const std::string& name) {
        if (onPath == kNoNode || adopted || name != m_result.tree.nodes[onPath].name) {
          return false;
        }
        adopt(node, onPath);
        adopted = true;
        return true;
      };
      std::vector<std::tuple<NodeId, std::uint32_t, std::optional<Location>>> folders;
      for (std::uint32_t c = record.firstChild; c < record.firstChild + record.childCount; ++c) {
        const auto& child = table.records[c];
        if (takesPlace(child.name)) {
          continue;
        }
        auto childLocation = childFor(location, child.name);
        const auto id = add(folderNode(child), node, childLocation);
        folders.emplace_back(id, c, std::move(childLocation));
      }
      for (const auto& file : files) {
        if (!takesPlace(file.name)) {
          add(fileNode(file.name, file.apparentBytes, file.modified, file.hidden), node, childFor(location, file.name));
        }
      }
      if (record.otherDirectories.count > 0) {
        auto rest = remainderNode(record.otherDirectories, true, record.flags.partial, record.flags.truncated);
        if (onPath != kNoNode && !adopted) {
          leaveOut(rest, m_result.tree.nodes[onPath]);
        }
        if (rest.items > 0) {
          m_result.remainders.push_back(add(std::move(rest), node, {}));
        }
      }
      if (record.otherFiles.count > 0) {
        m_result.remainders.push_back(
            add(remainderNode(record.otherFiles, false, record.flags.partial, false), node, {})
        );
      }
      if (onPath != kNoNode && !adopted) {
        adopt(node, onPath);
      }
      for (const auto& [id, child, childLocation] : folders) {
        expand(table, child, id, levels - 1, childLocation, kNoNode);
      }
    }

    // A folder known only by its listing: partial, summed from what its entries tell.
    NodeId listed(
        const Location& location, const std::vector<luciole::ListedEntry>& entries, std::uint32_t levels, NodeId onPath
    ) {
      Node node;
      node.name = location.basename().value_or("/");
      node.folder = true;
      node.partial = true;
      node.category = category(node.name, true);
      const auto id = add(std::move(node), kNoNode, location);
      bool adopted = false;
      if (levels > 0 && fits(id, entries.size())) {
        for (const auto& entry : entries) {
          if (onPath != kNoNode && !adopted && entry.name == m_result.tree.nodes[onPath].name) {
            adopt(id, onPath);
            adopted = true;
          } else if (!entry.folder) {
            add(fileNode(entry.name, entry.bytes, entry.modified, entry.hidden), id, childFor(location, entry.name));
          } else {
            listedFolder(entry, id, location, levels - 1);
          }
        }
      }
      if (onPath != kNoNode && !adopted) {
        adopt(id, onPath);
      }
      auto& folder = m_result.tree.nodes[id];
      folder.items = static_cast<double>(std::max(entries.size(), m_children[id].size()));
      for (const auto child : m_children[id]) {
        const auto& item = m_result.tree.nodes[child];
        folder.bytes += item.bytes;
        folder.files += item.folder ? item.files : 1;
        folder.modified = std::max(folder.modified, item.modified);
      }
      return id;
    }

    void listedFolder(const luciole::ListedEntry& entry, NodeId parent, const Location& folder, std::uint32_t levels) {
      const auto location = folder.child(entry.name);
      const auto found = m_view.find(location);
      if (const auto* record = found.entry()) {
        const auto id = add(folderNode(*record), parent, location);
        expand(*found.table, found.record, id, levels, location, kNoNode);
        return;
      }
      Node node;
      node.name = entry.name;
      node.folder = true;
      node.hidden = entry.hidden;
      node.partial = true;
      node.items = static_cast<double>(entry.items.value_or(0));
      node.modified = static_cast<double>(entry.modified);
      node.category = category(entry.name, true);
      add(std::move(node), parent, location);
    }

    const luciole::MeasuredTreeInput& m_input;
    const luciole::MeasureView& m_view;
    std::uint32_t m_levels;
    luciole::MeasuredTree m_result;
    std::vector<std::vector<NodeId>> m_children; // indexed like the nodes
  };
} // namespace

luciole::MeasuredTree luciole::buildMeasuredTree(const MeasuredTreeInput& input, const MeasureView& view) {
  return Builder(input, view).build();
}
