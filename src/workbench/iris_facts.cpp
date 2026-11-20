#include "workbench/iris_facts.h"

#include "workbench/entry_text.h"
#include "workbench/format.h"
#include "workbench/iris_hit.h"
#include "workbench/measure.h"
#include "workbench/not_counted.h"

#include <cmath>
#include <format>
#include <optional>
#include <string_view>

namespace luciole::workbench::iris {
  namespace {
    constexpr std::string_view kTopTip = "Top of this view"; // the hub of a folder with nowhere to go up to

    std::string displayName(const Node& node) { return node.name.empty() ? std::string("/") : node.name; }

    ReadoutLine entryLine(const Tree& tree, const Scene& scene, NodeId id) {
      const auto& node = tree.nodes[id];
      auto facts = measureLabel(tree, id, scene.settings.sizeBy);
      if (const auto items = itemsLabel(tree, id); node.folder && items != facts) {
        facts += std::string(kFactsDot) + items;
      }
      return {.text = displayName(node), .facts = std::move(facts)};
    }

    // The merged sector that `id` stands for: the main ring's "+N more" for the folder, else a context ring's.
    const Sector* mergedSector(const Scene& scene, NodeId folder) {
      const SectorRing* ring = folder == scene.folder ? &scene.main : nullptr;
      for (const auto& context : scene.rings) {
        if (ring == nullptr && context.folder == folder) {
          ring = &context;
        }
      }
      if (ring == nullptr) {
        return nullptr;
      }
      for (const auto& sector : ring->sectors) {
        if (sector.kind != SectorKind::Entry) {
          return &sector;
        }
      }
      return nullptr;
    }

    std::optional<ReadoutLine> hoveredLine(const Tree& tree, const Scene& scene, ShapeId id) {
      const auto role = sceneRole(id.role);
      const bool merged = role == SceneRole::More || (role == SceneRole::Row && id.node == scene.folder);
      if (merged) {
        const auto* sector = mergedSector(scene, id.node);
        if (sector == nullptr || !hasNode(tree, id.node)) {
          return std::nullopt;
        }
        if (sector->kind == SectorKind::More) {
          return ReadoutLine{
              .text = std::format("{} more entries", countText(static_cast<double>(sector->merged.size()))), .facts = {}
          };
        }
        return ReadoutLine{
            .text = std::format(
                "{} more in {}", countText(static_cast<double>(sector->merged.size())), displayName(tree.nodes[id.node])
            ),
            .facts = {}
        };
      }
      if ((role == SceneRole::Piece || role == SceneRole::Row) && hasNode(tree, id.node)) {
        return entryLine(tree, scene, id.node);
      }
      return std::nullopt;
    }
  } // namespace

  ReadoutLine readoutLine(const Tree& tree, const Scene& scene, const ShapeMarks& marks, CursorStop stop) {
    if (marks.hovered) {
      if (auto line = hoveredLine(tree, scene, *marks.hovered)) {
        return std::move(*line);
      }
    }
    for (const auto id : {marks.linked, stop == CursorStop::Marks ? marks.cursor : kNoNode, scene.folder}) {
      if (hasNode(tree, id)) {
        return entryLine(tree, scene, id);
      }
    }
    return {};
  }

  std::string readoutText(const Tree& tree, const Scene& scene, const ShapeMarks& marks, CursorStop stop) {
    const auto line = readoutLine(tree, scene, marks, stop);
    return line.facts.empty() ? line.text : line.text + std::string(kFactsDot) + line.facts;
  }

  Tip shapeTip(const Tree& tree, const Scene& scene, ShapeId id) {
    const auto role = sceneRole(id.role);
    if (role == SceneRole::Chip || role == SceneRole::Stack) {
      const auto folder = folderOf(scene, id);
      return hasNode(tree, folder) ? Tip{.rows = {}, .text = nodePath(tree, folder)} : Tip{};
    }
    if (!hasNode(tree, id.node)) {
      return {};
    }
    switch (role) {
    case SceneRole::Piece:
    case SceneRole::Row: {
      if (id.node == scene.folder) {
        return {};
      }
      const auto facts = entryFacts(tree, id.node, &scene.settings);
      // An amount that reads "at least" needs a sentence, which a row of the table cannot hold.
      if (!facts.notCounted.empty()) {
        return {.rows = {}, .text = entryText(facts)};
      }
      Tip tip{
          .rows =
              {{.key = "Name", .value = facts.name},
               {.key = "Size", .value = facts.size},
               {.key = "Share", .value = facts.share}},
          .text = {},
      };
      std::string reasons;
      for (const auto reason : facts.reasons) {
        reasons += reasons.empty() ? "" : ", ";
        reasons += reason;
      }
      if (!reasons.empty()) {
        tip.rows.push_back({.key = "State", .value = std::move(reasons)});
      }
      return tip;
    }
    case SceneRole::Hub: {
      const auto parent = parentOf(tree, id.node);
      auto text =
          hasNode(tree, parent) ? std::format("Up to {}", displayName(tree.nodes[parent])) : std::string(kTopTip);
      if (const auto why = notCountedText(tree, id.node); !why.empty()) {
        text += "\n" + why;
      }
      return {.rows = {}, .text = std::move(text)};
    }
    default:
      return {};
    }
  }

  std::string nodePath(const Tree& tree, NodeId id) {
    std::string path;
    for (const auto step : pathFromTop(tree, id)) {
      const auto& name = tree.nodes[step].name;
      if (!path.empty() && path.back() != '/') {
        path += '/';
      }
      path += name.empty() ? "/" : name;
    }
    return path;
  }
} // namespace luciole::workbench::iris
