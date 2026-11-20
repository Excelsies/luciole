#include "workbench/not_counted.h"

#include "workbench/measure.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::workbench {
  namespace {
    constexpr std::size_t kNamed = 3; // folders named before "and N more"
    constexpr std::string_view kLead = "Not counted: ";
    constexpr std::string_view kGeneral = "some folders below were not counted in full";
    // A folder above the open one is measured only up to a limit, so what it was cut by is not in its flags.
    constexpr std::string_view kAbove =
        "Not counted: measuring this folder stopped at a limit, at another filesystem or at a folder that cannot be "
        "read.";

    struct Found {
      std::vector<std::string_view> mounts; // on another filesystem
      std::vector<std::string_view> locked; // could not be read
      bool unplaced = false;                // a lower bound the snapshot cannot trace to a flagged folder
      bool capped = false;                  // the cap ended the walk
    };

    // "a", "a, b, c and 2 more"; with `joined`, "a and b" and "a, b and c" for a list of three or fewer.
    std::string namesOf(const std::vector<std::string_view>& names, bool joined) {
      std::string out;
      const auto shown = std::min(names.size(), kNamed);
      for (std::size_t i = 0; i < shown; ++i) {
        const bool last = joined && i + 1 == shown && names.size() == shown;
        out += i == 0 ? "" : (last ? " and " : ", ");
        out += names[i].empty() ? "/" : names[i];
      }
      if (names.size() > shown) {
        out += std::format(" and {} more", names.size() - shown);
      }
      return out;
    }

    // Walks from `start` through lower bounds only: a folder that is exact holds nothing that was not counted. At most
    // `cap` nodes are read; each node is pushed once, by its one parent. Only lower-bound children are charged to the
    // cap, and every child of a visited node costs one cheap check, so a call costs at most the cap in nodes plus
    // their children, which the snapshot's node limit bounds.
    void walk(const Tree& tree, NodeId start, std::size_t cap, std::size_t& visited, Found& found) {
      std::vector<NodeId> pending{start};
      while (!pending.empty() && !found.capped) {
        const auto id = pending.back();
        pending.pop_back();
        if (visited == cap) {
          found.capped = true;
          return;
        }
        ++visited;
        const auto& node = tree.nodes[id];
        bool placed = false;
        if (node.otherFilesystem) {
          found.mounts.push_back(node.name);
          placed = true;
        } else if (node.unreadable) {
          found.locked.push_back(node.name);
          placed = true;
        }
        // Reverse, so the folders come out in the order the snapshot lists them. Nodes read and nodes waiting never
        // pass `cap`, so one folder with many lower-bound children costs at most `cap` pushes and one check for each
        // of its other children.
        for (const auto child : std::views::reverse(children(tree, id))) {
          if (!hasNode(tree, child) || measureState(tree.nodes[child]) != MeasureState::LowerBound) {
            continue;
          }
          if (visited + pending.size() >= cap) {
            found.capped = true;
            return;
          }
          pending.push_back(child);
          placed = true;
        }
        found.unplaced = found.unplaced || !placed;
      }
    }

    std::string sentence(const Found& found) {
      std::vector<std::string> parts;
      if (!found.capped) {
        if (!found.mounts.empty()) {
          parts.push_back(
              std::format(
                  "{} {} on another filesystem", namesOf(found.mounts, true), found.mounts.size() == 1 ? "is" : "are"
              )
          );
        }
        if (!found.locked.empty()) {
          parts.push_back(
              std::format(
                  "{} unreadable {} ({})", found.locked.size(), found.locked.size() == 1 ? "folder" : "folders",
                  namesOf(found.locked, false)
              )
          );
        }
      }
      if (found.unplaced || found.capped || parts.empty()) {
        parts.emplace_back(kGeneral);
      }
      std::string out(kLead);
      for (std::size_t i = 0; i < parts.size(); ++i) {
        out += i == 0 ? "" : "; ";
        out += parts[i];
      }
      return out + ".";
    }
  } // namespace

  std::string notCountedText(const Tree& tree, NodeId id, std::size_t cap) {
    return notCountedText(tree, std::span<const NodeId>(&id, 1), cap);
  }

  std::string notCountedText(const Tree& tree, std::span<const NodeId> ids, std::size_t cap) {
    Found found;
    std::size_t visited = 0;
    bool any = false;
    // One walk per lower bound among `ids`; the visits are shared, so the whole call reads at most `cap` nodes.
    for (const auto id : ids) {
      if (!hasNode(tree, id) || measureState(tree.nodes[id]) != MeasureState::LowerBound) {
        continue;
      }
      any = true;
      if (std::ranges::contains(tree.ancestors, id)) {
        return std::string(kAbove);
      }
      walk(tree, id, cap, visited, found);
      if (found.capped) {
        break;
      }
    }
    return any ? sentence(found) : std::string{};
  }
} // namespace luciole::workbench
