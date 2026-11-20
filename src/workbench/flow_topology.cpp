#include "workbench/flow_topology.h"

#include <algorithm>
#include <format>
#include <span>
#include <utility>

namespace luciole::workbench {
  namespace {
    // Folders drawn per parent in columns 2 and 3.
    constexpr std::array<std::size_t, kFlowColumns> kQuota{0, 0, 3, 2};

    struct Weighted {
      NodeId id = kNoNode;
      double weight = 0;
    };

    // Visible children of a folder: folders heaviest first, and the totals of its files.
    struct Partition {
      std::vector<Weighted> folders;
      std::size_t fileCount = 0;
      FlowTotals files;
    };

    // The folders of one parent in a later column: `kids` are drawn, `tail` goes into a More record.
    struct Group {
      std::size_t parent = kNoRecord;
      std::vector<Weighted> kids;
      std::vector<Weighted> tail;
      bool collapsed = false;
    };

    FlowTotals totalsOf(const Tree& tree, NodeId id) {
      return {
          .bytes = measureValue(tree, id, WorkbenchMeasure::Bytes),
          .files = measureValue(tree, id, WorkbenchMeasure::Files),
          .items = measureValue(tree, id, WorkbenchMeasure::Items),
          .modified = measureValue(tree, id, WorkbenchMeasure::Modified),
          .visits = measureValue(tree, id, WorkbenchMeasure::Visits),
          .state = measureState(tree.nodes[id]),
      };
    }

    // A one-folder tree holding `totals`, so weights and labels of summed records follow the same rules as nodes.
    Tree totalsTree(const FlowTotals& totals, double now) {
      Tree tree;
      tree.now = now;
      Node node;
      node.bytes = totals.bytes;
      node.files = totals.files;
      node.items = totals.items;
      node.modified = totals.modified;
      node.visits = totals.visits;
      node.partial = totals.state == MeasureState::Measuring;
      node.lowerBound = totals.state == MeasureState::LowerBound;
      node.folder = true;
      tree.nodes.push_back(std::move(node));
      tree.root = 0;
      return tree;
    }

    Partition partition(const Tree& tree, NodeId folder, const LayoutSettings& settings) {
      Partition part;
      const auto list = entries(tree, folder, settings);
      for (std::size_t i = 0; i < list.ids.size(); ++i) {
        const auto id = list.ids[i];
        if (tree.nodes[id].folder) {
          part.folders.push_back({.id = id, .weight = list.weights[i]});
          continue;
        }
        const auto file = totalsOf(tree, id);
        ++part.fileCount;
        part.files.bytes += file.bytes;
        part.files.visits += file.visits;
        part.files.modified = std::max(part.files.modified, file.modified);
      }
      return part;
    }

    FlowRecord makeRecord(FlowRecordKind kind, std::size_t column, NodeId node, std::size_t parent) {
      FlowRecord record;
      record.kind = kind;
      record.column = column;
      record.node = node;
      record.parent = parent;
      return record;
    }

    FlowRecord folderRecord(const Tree& tree, Weighted folder, std::size_t column, std::size_t parent) {
      auto record = makeRecord(FlowRecordKind::Folder, column, folder.id, parent);
      record.weight = folder.weight;
      record.totals = totalsOf(tree, folder.id);
      return record;
    }

    // Weighs the summed folders as they are; their totals add up, with the newest change.
    FlowRecord moreRecord(
        const Tree& tree, NodeId owner, std::span<const Weighted> list, std::size_t column, std::size_t parent,
        bool collapsed
    ) {
      auto record = makeRecord(FlowRecordKind::More, column, owner, parent);
      record.count = list.size();
      record.collapsed = collapsed;
      record.folders.reserve(list.size());
      for (const auto& folder : list) {
        const auto totals = totalsOf(tree, folder.id);
        record.folders.push_back(folder.id);
        record.weight += folder.weight;
        record.totals.bytes += totals.bytes;
        record.totals.files += totals.files;
        record.totals.items += totals.items;
        record.totals.visits += totals.visits;
        record.totals.modified = std::max(record.totals.modified, totals.modified);
        record.totals.state = worseState(record.totals.state, totals.state);
      }
      return record;
    }

    // All files of a column as one entry with the summed values: its weight is that of one entry, so thousands of
    // small files do not outweigh every folder.
    FlowRecord filesRecord(
        const Tree& tree, NodeId folder, std::size_t column, std::size_t fileCount, FlowTotals totals,
        const LayoutSettings& settings
    ) {
      totals.files = static_cast<double>(fileCount);
      totals.items = static_cast<double>(fileCount);
      auto record = makeRecord(FlowRecordKind::Files, column, folder, kNoRecord);
      record.count = fileCount;
      record.weight = weight(totalsTree(totals, tree.now), 0, settings);
      record.totals = totals;
      return record;
    }

    std::size_t groupCount(const Group& group) { return group.kids.size() + (group.tail.empty() ? 0 : 1); }

    // Folds the lightest group into one More record until the column fits; drops the lightest single records last.
    void fitGroups(std::vector<Group>& groups, std::size_t cap, const std::vector<FlowRecord>& records) {
      std::size_t total = 0;
      for (const auto& group : groups) {
        total += groupCount(group);
      }
      const auto parentWeight = [&records](const Group& group) { return records[group.parent].weight; };
      // Each pass folds a group to one record or drops one group.
      const auto passes = 2 * groups.size() + 1;
      for (std::size_t pass = 0; pass < passes && total > cap && !groups.empty(); ++pass) {
        auto multi = groups.end();
        for (auto it = groups.begin(); it != groups.end(); ++it) {
          if (groupCount(*it) > 1 && (multi == groups.end() || parentWeight(*it) < parentWeight(*multi))) {
            multi = it;
          }
        }
        if (multi != groups.end()) {
          total -= groupCount(*multi) - 1;
          multi->kids.insert(multi->kids.end(), multi->tail.begin(), multi->tail.end());
          multi->tail = std::move(multi->kids);
          multi->kids.clear();
          multi->collapsed = true;
          continue;
        }
        auto lightest = groups.begin();
        for (auto it = groups.begin(); it != groups.end(); ++it) {
          if (parentWeight(*it) < parentWeight(*lightest)) {
            lightest = it;
          }
        }
        groups.erase(lightest);
        total -= 1;
      }
    }

    struct ColumnPlan {
      std::vector<FlowRecord> records;
      std::size_t fileCount = 0;
      FlowTotals files;
    };

    ColumnPlan
    firstColumn(const Tree& tree, FlowTopology& topology, const LayoutSettings& settings, const FlowOptions& options) {
      topology.spent += children(tree, topology.folder).size();
      auto part = partition(tree, topology.folder, settings);
      ColumnPlan plan;
      plan.fileCount = part.fileCount;
      plan.files = part.files;
      const auto shown = std::min(options.firstMax, part.folders.size());
      for (std::size_t i = 0; i < shown; ++i) {
        plan.records.push_back(folderRecord(tree, part.folders[i], 1, 0));
      }
      if (part.folders.size() > shown) {
        plan.records.push_back(moreRecord(tree, topology.folder, std::span(part.folders).subspan(shown), 1, 0, false));
      }
      return plan;
    }

    ColumnPlan laterColumn(
        const Tree& tree, FlowTopology& topology, std::size_t column, const LayoutSettings& settings,
        const FlowOptions& options
    ) {
      ColumnPlan plan;
      std::vector<std::size_t> parents;
      for (const auto index : topology.columns[column - 1]) {
        if (topology.records[index].kind == FlowRecordKind::Folder) {
          parents.push_back(index);
        }
      }
      std::ranges::stable_sort(parents, [&topology](std::size_t left, std::size_t right) {
        return topology.records[left].weight > topology.records[right].weight;
      });
      std::vector<Group> groups;
      for (const auto parent : parents) {
        const auto node = topology.records[parent].node;
        const auto read = children(tree, node).size();
        if (read == 0) {
          continue;
        }
        if (topology.spent + read > options.budget) {
          topology.truncated = true;
          continue;
        }
        topology.spent += read;
        auto part = partition(tree, node, settings);
        plan.fileCount += part.fileCount;
        plan.files.bytes += part.files.bytes;
        plan.files.visits += part.files.visits;
        plan.files.modified = std::max(plan.files.modified, part.files.modified);
        if (!part.folders.empty()) {
          const auto quota = std::min(kQuota[column], part.folders.size());
          Group group;
          group.parent = parent;
          group.kids.assign(part.folders.begin(), part.folders.begin() + static_cast<std::ptrdiff_t>(quota));
          group.tail.assign(part.folders.begin() + static_cast<std::ptrdiff_t>(quota), part.folders.end());
          groups.push_back(std::move(group));
        }
      }
      std::ranges::stable_sort(groups, {}, &Group::parent);
      const std::size_t filesSlot = plan.fileCount > 0 ? 1 : 0;
      fitGroups(groups, options.perColumn > filesSlot ? options.perColumn - filesSlot : 0, topology.records);
      for (const auto& group : groups) {
        for (const auto& kid : group.kids) {
          plan.records.push_back(folderRecord(tree, kid, column, group.parent));
        }
        if (!group.tail.empty()) {
          plan.records.push_back(
              moreRecord(tree, topology.records[group.parent].node, group.tail, column, group.parent, group.collapsed)
          );
        }
      }
      return plan;
    }
  } // namespace

  FlowTopology
  buildFlowTopology(const Tree& tree, NodeId folder, const LayoutSettings& settings, const FlowOptions& options) {
    FlowTopology topology;
    topology.folder = folder;
    if (!hasNode(tree, folder)) {
      return topology;
    }
    auto current = makeRecord(FlowRecordKind::Current, 0, folder, kNoRecord);
    current.weight = weight(tree, folder, settings);
    current.totals = totalsOf(tree, folder);
    topology.records.push_back(std::move(current));
    topology.columns[0].push_back(0);
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      if (column > 1 && topology.columns[column - 1].empty()) {
        break;
      }
      auto plan = column == 1 ? firstColumn(tree, topology, settings, options)
                              : laterColumn(tree, topology, column, settings, options);
      if (plan.fileCount > 0 && !plan.records.empty()) {
        plan.records.push_back(filesRecord(tree, folder, column, plan.fileCount, plan.files, settings));
      }
      for (auto& record : plan.records) {
        const auto index = topology.records.size();
        if (record.parent != kNoRecord) {
          topology.records[record.parent].kids.push_back(index);
        }
        topology.columns[column].push_back(index);
        topology.records.push_back(std::move(record));
      }
    }
    if (topology.columns[1].empty()) {
      switch (folderState(tree.nodes[folder])) {
      case FolderState::Unreadable:
        topology.empty = FlowEmpty::Unreadable;
        break;
      case FolderState::NotMeasured:
        topology.empty = FlowEmpty::NotMeasured;
        break;
      case FolderState::OtherFilesystem:
      case FolderState::NotExpanded:
        topology.empty = FlowEmpty::NotExpanded;
        break;
      case FolderState::Listed:
        topology.empty = children(tree, folder).empty() ? FlowEmpty::Empty : FlowEmpty::NoFolders;
        break;
      }
    }
    return topology;
  }

  std::optional<std::size_t> flowRecordFor(const Tree& tree, const FlowTopology& topology, NodeId node) {
    if (topology.records.empty()) {
      return std::nullopt;
    }
    auto at = node;
    for (std::size_t step = 0; step <= tree.nodes.size() && hasNode(tree, at) && at != topology.folder; ++step) {
      for (std::size_t index = 0; index < topology.records.size(); ++index) {
        const auto& record = topology.records[index];
        if (record.kind == FlowRecordKind::Folder && record.node == at) {
          return index;
        }
      }
      const auto parent = tree.nodes[at].parent;
      if (parent == topology.folder) {
        for (const auto index : topology.columns[1]) {
          if (index >= topology.records.size()) {
            continue;
          }
          const auto& record = topology.records[index];
          if (record.kind == FlowRecordKind::More && std::ranges::contains(record.folders, at)) {
            return index;
          }
        }
        return std::nullopt;
      }
      at = parent;
    }
    return std::nullopt;
  }

  std::vector<bool> flowFocus(const FlowTopology& topology, std::size_t record) {
    const auto count = topology.records.size();
    std::vector<bool> focus(count, false);
    if (record >= count) {
      return focus;
    }
    // Records form a tree, so each walk visits a record at most once.
    for (std::size_t at = record, step = 0; at < count && step < count; at = topology.records[at].parent, ++step) {
      focus[at] = true;
    }
    std::vector<std::size_t> stack{record};
    for (std::size_t step = 0; step < count && !stack.empty(); ++step) {
      const auto at = stack.back();
      stack.pop_back();
      focus[at] = true;
      for (const auto kid : topology.records[at].kids) {
        if (kid < count && !focus[kid]) {
          stack.push_back(kid);
        }
      }
    }
    return focus;
  }

  std::string flowRecordName(const Tree& tree, const FlowTopology& topology, std::size_t record) {
    if (record >= topology.records.size()) {
      return {};
    }
    const auto& found = topology.records[record];
    switch (found.kind) {
    case FlowRecordKind::More:
      return found.collapsed ? std::format("{} folders", found.count) : std::format("+{} more", found.count);
    case FlowRecordKind::Files:
      return "files";
    case FlowRecordKind::Current:
    case FlowRecordKind::Folder:
      break;
    }
    if (!hasNode(tree, found.node) || tree.nodes[found.node].name.empty()) {
      return "/";
    }
    return tree.nodes[found.node].name;
  }

  std::string
  flowRecordValue(const Tree& tree, const FlowTopology& topology, std::size_t record, WorkbenchMeasure measure) {
    if (record >= topology.records.size()) {
      return {};
    }
    const auto& found = topology.records[record];
    if (found.kind == FlowRecordKind::More || found.kind == FlowRecordKind::Files) {
      return measureLabel(totalsTree(found.totals, tree.now), 0, measure);
    }
    return measureLabel(tree, found.node, measure);
  }
} // namespace luciole::workbench
