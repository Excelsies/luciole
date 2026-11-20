#include "ui/workbench/entry_tooltip.h"

#include "workbench/entry_text.h"
#include "workbench/scene_shapes.h"

#include <string>
#include <utility>

namespace luciole {
  namespace {
    TooltipContent contentOf(std::string text) {
      return text.empty() ? TooltipContent{} : TooltipContent{std::move(text)};
    }

    bool sameTree(const std::weak_ptr<const workbench::Tree>& left, const std::weak_ptr<const workbench::Tree>& right) {
      return !left.owner_before(right) && !right.owner_before(left);
    }
  } // namespace

  bool EntryTooltips::Key::operator==(const Key& other) const {
    return sameTree(tree, other.tree) && id == other.id && settings == other.settings && nodes == other.nodes;
  }

  template <typename Work> TooltipContent EntryTooltips::answer(Key key, Work work) {
    if (!m_key || !(*m_key == key)) {
      m_content = work();
      m_key = std::move(key);
      ++m_worked;
    }
    return m_content;
  }

  TooltipContent EntryTooltips::entry(
      const std::shared_ptr<const workbench::Tree>& tree, const workbench::LayoutSettings* shareIn,
      workbench::ShapeId id
  ) {
    using workbench::SceneRole;
    switch (workbench::sceneRole(id.role)) {
    case SceneRole::Piece:
    case SceneRole::Head:
    case SceneRole::Path:
    case SceneRole::Far:
    case SceneRole::Stem:
    case SceneRole::Ribbon:
      break;
    default:
      return {};
    }
    if (tree == nullptr) {
      return {};
    }
    Key key{.tree = tree, .id = id, .settings = std::nullopt, .nodes = {}};
    if (shareIn != nullptr) {
      key.settings = *shareIn;
    }
    return answer(std::move(key), [&] {
      const auto facts = workbench::entryFacts(*tree, id.node, shareIn);
      // The current folder's own shapes tell nothing the view does not, unless its total reads "at least".
      return id.node == tree->root && facts.notCounted.empty() ? TooltipContent{}
                                                               : contentOf(workbench::entryText(facts));
    });
  }

  TooltipContent EntryTooltips::merged(
      const std::shared_ptr<const workbench::Tree>& tree, workbench::ShapeId id,
      std::span<const workbench::NodeId> nodes
  ) {
    if (tree == nullptr) {
      return {};
    }
    Key key{
        .tree = tree,
        .id = id,
        .settings = std::nullopt,
        .nodes = std::vector<workbench::NodeId>(nodes.begin(), nodes.end()),
    };
    return answer(std::move(key), [&] { return contentOf(workbench::mergedText(*tree, nodes)); });
  }
} // namespace luciole
