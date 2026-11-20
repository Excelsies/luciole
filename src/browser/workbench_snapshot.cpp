#include "browser/workbench_snapshot.h"

#include <algorithm>
#include <span>
#include <utility>

namespace {
  using luciole::Location;
  using luciole::workbench::kNoNode;
  using luciole::workbench::Node;
  using luciole::workbench::NodeId;
  using luciole::workbench::Tree;

  NodeId childNamed(const Tree& tree, NodeId parent, const std::string& name) {
    for (const auto id : luciole::workbench::children(tree, parent)) {
      if (tree.nodes[id].name == name) {
        return id;
      }
    }
    return kNoNode;
  }

  // Marks every node at or below the given ones; bounded by the node count, since each node is marked once.
  std::vector<bool> subtrees(const Tree& tree, std::vector<NodeId> stack) {
    std::vector<bool> inside(tree.nodes.size(), false);
    while (!stack.empty()) {
      const auto id = stack.back();
      stack.pop_back();
      if (!luciole::workbench::hasNode(tree, id) || inside[id]) {
        continue;
      }
      inside[id] = true;
      for (const auto child : luciole::workbench::children(tree, id)) {
        stack.push_back(child);
      }
    }
    return inside;
  }

  // Files below a node: its count for a folder, one for a file.
  double filesOf(const Node& node) { return node.folder ? node.files : 1; }

  // Moves the folder's totals from `was` (an entry it lost) to `now` (an entry it gained); either may be null. The
  // folder reads as measuring if `now` is, else as a lower bound if it was or `now` is. A lower bound is never
  // cleared here: the folder's own record may be cut short for a reason other than the entry it lost.
  void retotal(Node& folder, const Node* was, const Node* now) {
    const auto shift = [](double value, double less, double more) { return std::max(0.0, value - less + more); };
    folder.bytes = shift(folder.bytes, was ? was->bytes : 0, now ? now->bytes : 0);
    folder.files = shift(folder.files, was ? filesOf(*was) : 0, now ? filesOf(*now) : 0);
    folder.items = shift(folder.items, was ? 1 : 0, now ? 1 : 0);
    if (now) {
      folder.modified = std::max(folder.modified, now->modified);
      folder.partial = folder.partial || now->partial;
      folder.lowerBound = folder.lowerBound || now->lowerBound;
    }
  }

  // A child of the root replaced by `sub`, added when `old` is kNoNode, or removed when `sub` is null.
  struct Graft {
    std::string name;
    NodeId old = kNoNode;
    const Tree* sub = nullptr;
    NodeId subRoot = kNoNode; // in the grafted tree
  };

  // What each refreshed child does to the root; a child whose walk failed for another reason than being gone only
  // flags the folder it replaces unreadable. Bounded by the refresh count squared, for repeated names.
  std::vector<Graft> graftsOf(Tree& tree, std::span<const luciole::WorkbenchRefresh> refreshed) {
    std::vector<Graft> grafts;
    for (const auto& child : refreshed) {
      if (std::ranges::contains(grafts, child.name, &Graft::name)) {
        continue;
      }
      const auto old = childNamed(tree, tree.root, child.name);
      if (child.measured) {
        const auto& sub = child.measured->tree;
        if (luciole::workbench::hasNode(sub, sub.root)) {
          grafts.push_back({.name = child.name, .old = old, .sub = &sub});
        }
      } else if (child.error && child.error->kind == luciole::FileErrorKind::Missing) {
        if (old != kNoNode) {
          grafts.push_back({.name = child.name, .old = old});
        }
      } else if (old != kNoNode && tree.nodes[old].folder) {
        tree.nodes[old].unreadable = true;
      }
    }
    return grafts;
  }

  // Appends a grafted child's levels below `root` at the end of `out`, with their child lists.
  void append(Tree& out, std::vector<std::vector<NodeId>>& lists, Graft& graft, NodeId root) {
    const auto& sub = *graft.sub;
    const auto offset = static_cast<NodeId>(out.nodes.size());
    graft.subRoot = offset + sub.root;
    lists.resize(out.nodes.size() + sub.nodes.size());
    for (NodeId id = 0; id < sub.nodes.size(); ++id) {
      auto node = sub.nodes[id];
      node.parent = node.parent == kNoNode ? root : offset + node.parent;
      for (const auto child : luciole::workbench::children(sub, id)) {
        lists[offset + id].push_back(offset + child);
      }
      out.nodes.push_back(std::move(node));
    }
    out.nodes[graft.subRoot].name = graft.name;
    out.nodes[graft.subRoot].hidden = graft.name.starts_with('.');
  }

  // The tree with every refreshed child applied in one copy, and the root's totals moved accordingly; a tree
  // without a root is left as it is.
  Tree graftAll(Tree tree, std::span<const luciole::WorkbenchRefresh> refreshed) {
    if (!luciole::workbench::hasNode(tree, tree.root)) {
      return tree;
    }
    auto grafts = graftsOf(tree, refreshed);
    if (grafts.empty()) {
      return tree;
    }
    std::vector<NodeId> olds;
    for (const auto& graft : grafts) {
      if (graft.old != kNoNode) {
        olds.push_back(graft.old);
      }
    }
    const auto removed = subtrees(tree, std::move(olds));
    std::vector<NodeId> remap(tree.nodes.size(), kNoNode);
    Tree out;
    out.now = tree.now;
    for (NodeId id = 0; id < tree.nodes.size(); ++id) {
      if (!removed[id]) {
        remap[id] = static_cast<NodeId>(out.nodes.size());
        out.nodes.push_back(tree.nodes[id]);
      }
    }
    const auto root = remap[tree.root];
    std::vector<std::vector<NodeId>> lists(out.nodes.size());
    for (auto& graft : grafts) {
      if (graft.sub) {
        append(out, lists, graft, root);
      }
    }
    for (NodeId id = 0; id < tree.nodes.size(); ++id) {
      if (removed[id]) {
        continue;
      }
      auto& node = out.nodes[remap[id]];
      node.parent = node.parent == kNoNode ? kNoNode : remap[node.parent];
      for (const auto child : luciole::workbench::children(tree, id)) {
        if (!removed[child]) {
          lists[remap[id]].push_back(remap[child]);
        } else if (
            const auto graft = std::ranges::find(grafts, child, &Graft::old); graft != grafts.end() && graft->sub
        ) {
          lists[remap[id]].push_back(graft->subRoot);
        }
      }
    }
    for (const auto& graft : grafts) {
      if (graft.old == kNoNode) {
        lists[root].push_back(graft.subRoot);
      }
    }
    for (NodeId id = 0; id < out.nodes.size(); ++id) {
      out.nodes[id].firstChild = static_cast<std::uint32_t>(out.children.size());
      out.nodes[id].childCount = static_cast<std::uint32_t>(lists[id].size());
      out.children.insert(out.children.end(), lists[id].begin(), lists[id].end());
    }
    out.root = root;
    for (const auto ancestor : tree.ancestors) {
      out.ancestors.push_back(remap[ancestor]);
    }
    for (const auto& graft : grafts) {
      const auto* was = graft.old == kNoNode ? nullptr : &tree.nodes[graft.old];
      retotal(out.nodes[root], was, graft.sub ? &out.nodes[graft.subRoot] : nullptr);
    }
    return out;
  }

  // The prefix every URI strictly below `uri` starts with.
  std::string below(const std::string& uri) { return uri.ends_with('/') ? uri : uri + '/'; }

  // Sets the visit count of every node a visit names. Only branches some visit lies in are followed, so locations
  // are built for those nodes alone; bounded by the node count.
  void applyVisits(
      Tree& tree, const Location& location, std::span<const Location> ancestors,
      std::span<const luciole::VisitRecord> visits
  ) {
    if (visits.empty() || !luciole::workbench::hasNode(tree, tree.root)) {
      return;
    }
    const auto levels = std::min(tree.ancestors.size(), ancestors.size());
    const auto top = levels > 0 ? tree.ancestors[levels - 1] : tree.root;
    const auto by = [](const luciole::VisitRecord& record) -> const std::string& { return record.uri; };
    std::vector<std::pair<NodeId, Location>> stack{{top, levels > 0 ? ancestors[levels - 1] : location}};
    for (std::size_t step = 0; !stack.empty() && step < tree.nodes.size(); ++step) {
      auto [id, place] = std::move(stack.back());
      stack.pop_back();
      const auto& uri = place.uri();
      if (const auto exact = std::ranges::lower_bound(visits, uri, {}, by);
          exact != visits.end() && exact->uri == uri) {
        tree.nodes[id].visits = static_cast<double>(exact->visit.count);
      }
      const auto prefix = below(uri);
      if (const auto next = std::ranges::lower_bound(visits, prefix, {}, by);
          next == visits.end() || !next->uri.starts_with(prefix)) {
        continue;
      }
      for (const auto child : luciole::workbench::children(tree, id)) {
        if (!tree.nodes[child].name.empty()) {
          stack.emplace_back(child, place.child(tree.nodes[child].name));
        }
      }
    }
  }

  // The folder of each Jump ahead row, from the names on its path below the root.
  std::vector<Location>
  jumpLocations(const Tree& tree, const Location& location, const luciole::workbench::JumpList& jump) {
    std::vector<Location> result;
    result.reserve(jump.rows.size());
    for (const auto& row : jump.rows) {
      const auto path = luciole::workbench::pathFromTop(tree, row.id);
      auto at = std::ranges::find(path, tree.root);
      auto place = location;
      for (at = at == path.end() ? at : std::next(at); at != path.end(); ++at) {
        place = place.child(tree.nodes[*at].name);
      }
      result.push_back(std::move(place));
    }
    return result;
  }

  std::vector<luciole::WorkbenchShare> sharesOf(const Tree& tree, const luciole::workbench::LayoutSettings& settings) {
    const auto ids = luciole::workbench::children(tree, tree.root);
    const auto values = luciole::workbench::shares(tree, tree.root, settings);
    std::vector<luciole::WorkbenchShare> result;
    for (std::size_t i = 0; i < ids.size() && i < values.size(); ++i) {
      if (!tree.nodes[ids[i]].name.empty()) {
        result.push_back({.name = tree.nodes[ids[i]].name, .id = ids[i], .share = std::max(0.0, values[i])});
      }
    }
    std::ranges::sort(result, {}, &luciole::WorkbenchShare::name);
    return result;
  }

  bool sameNode(const Node& a, const Node& b) {
    return a.name == b.name
        && a.parent == b.parent
        && a.firstChild == b.firstChild
        && a.childCount == b.childCount
        && a.bytes == b.bytes
        && a.files == b.files
        && a.items == b.items
        && a.modified == b.modified
        && a.visits == b.visits
        && a.category == b.category
        && a.folder == b.folder
        && a.hidden == b.hidden
        && a.partial == b.partial
        && a.lowerBound == b.lowerBound
        && a.unreadable == b.unreadable
        && a.truncated == b.truncated
        && a.otherFilesystem == b.otherFilesystem;
  }

  // Equal in everything but the time the tree was built.
  bool sameTree(const Tree& a, const Tree& b) {
    return a.root == b.root
        && a.ancestors == b.ancestors
        && a.children == b.children
        && std::ranges::equal(a.nodes, b.nodes, sameNode);
  }

  const luciole::WorkbenchShare* findShare(const std::vector<luciole::WorkbenchShare>& shares, std::string_view name) {
    const auto found = std::ranges::lower_bound(shares, name, {}, &luciole::WorkbenchShare::name);
    return found != shares.end() && found->name == name ? &*found : nullptr;
  }
} // namespace

double luciole::WorkbenchSnapshot::share(const ListedEntry& entry) const {
  if (const auto* found = findShare(shares, entry.name)) {
    return found->share;
  }
  if (total <= 0 || (entry.hidden && !settings.showHidden)) {
    return 0.0;
  }
  workbench::Tree one;
  one.now = tree.now;
  one.root = 0;
  one.nodes.push_back({
      .name = entry.name,
      .bytes = entry.folder ? 0.0 : static_cast<double>(entry.bytes),
      .items = static_cast<double>(entry.items.value_or(0)),
      .modified = static_cast<double>(entry.modified),
      .folder = entry.folder,
      .hidden = entry.hidden,
  });
  return workbench::weight(one, one.root, settings) / total;
}

luciole::workbench::NodeId luciole::WorkbenchSnapshot::node(std::string_view name) const {
  const auto* found = findShare(shares, name);
  return found ? found->id : kNoNode;
}

std::optional<luciole::WorkbenchDraft> luciole::buildDraft(const WorkbenchBuildInput& input) {
  const MeasureView none;
  const auto& view = input.view ? *input.view : none;
  MeasuredTreeInput request{
      .current = input.location,
      .entries = {},
      .ancestors = input.ancestors ? *input.ancestors : std::vector<MeasuredLevel>{},
      .visits = {},
      .now = input.now,
  };
  auto measured = std::make_shared<const MeasuredTree>(buildMeasuredTree(request, view));
  const auto& tree = measured->tree;
  if (workbench::children(tree, tree.root).empty() && !input.folderFinished && !input.error) {
    return std::nullopt;
  }
  WorkbenchDraft draft{
      .location = input.location,
      .ancestors = {},
      .measured = std::move(measured),
      .refreshed = {},
      .visits = {},
      .settings = {},
      .error = input.error,
  };
  for (const auto& level : request.ancestors) {
    draft.ancestors.push_back(level.location);
  }
  for (const auto& child : input.children) {
    auto built = child.built;
    if (!built && child.table && !child.table->records.empty()) {
      const MeasuredTreeInput levels{
          .current = child.location, .entries = {}, .ancestors = {}, .visits = {}, .now = input.now, .levels = 2
      };
      built = std::make_shared<const MeasuredTree>(buildMeasuredTree(levels, view));
    }
    draft.refreshed.push_back({
        .name = child.name,
        .measured = std::move(built),
        .error = child.table ? child.table->error : std::nullopt,
        .from = child.table,
    });
  }
  return draft;
}

std::shared_ptr<const luciole::WorkbenchSnapshot>
luciole::finishSnapshot(const WorkbenchDraft& draft, const std::shared_ptr<const WorkbenchSnapshot>& previous) {
  auto tree = graftAll(draft.measured ? draft.measured->tree : workbench::Tree{}, draft.refreshed);
  if (draft.visits) {
    if (std::ranges::is_sorted(*draft.visits, {}, &VisitRecord::uri)) {
      applyVisits(tree, draft.location, draft.ancestors, *draft.visits);
    } else {
      auto sorted = *draft.visits;
      std::ranges::sort(sorted, {}, &VisitRecord::uri);
      applyVisits(tree, draft.location, draft.ancestors, sorted);
    }
  }
  if (previous
      && previous->location == draft.location
      && previous->settings == draft.settings
      && previous->error == draft.error
      && sameTree(previous->tree, tree)) {
    return nullptr;
  }
  auto jump = workbench::jumpAhead(tree, draft.settings);
  auto locations = jumpLocations(tree, draft.location, jump);
  auto shares = sharesOf(tree, draft.settings);
  const auto total = workbench::entries(tree, tree.root, draft.settings).total;
  return std::make_shared<const WorkbenchSnapshot>(WorkbenchSnapshot{
      .serial = (previous ? previous->serial : 0) + 1,
      .location = draft.location,
      .settings = draft.settings,
      .tree = std::move(tree),
      .jump = std::move(jump),
      .jumpLocations = std::move(locations),
      .shares = std::move(shares),
      .total = total,
      .error = draft.error,
  });
}
