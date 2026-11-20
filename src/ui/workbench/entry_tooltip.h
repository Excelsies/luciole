#pragma once

#include "render/scene/tooltip_content.h"
#include "workbench/measure.h"
#include "workbench/shape.h"
#include "workbench/tree.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace luciole {
  // The tooltips of the shapes of one part. The presenter asks for the content on every layout pass while the pointer
  // rests, so the last answer is kept and given again while the snapshot, the shape and the settings stay the same;
  // only a different one is worked out, which reads the snapshot.
  class EntryTooltips {
  public:
    // The tooltip of a shape that stands for an entry (a piece, a card head, a bar of the path, a far cell, an
    // ancestor pill or a ribbon): workbench::entryText for its node, with its share among its siblings when `shareIn`
    // is given. Empty for merged pieces and drawing details, and for the current folder unless its total reads "at
    // least".
    TooltipContent entry(
        const std::shared_ptr<const workbench::Tree>& tree, const workbench::LayoutSettings* shareIn,
        workbench::ShapeId id
    );

    // The tooltip of the merged piece `id` standing for `nodes`: workbench::mergedText, so empty unless its total
    // reads "at least".
    TooltipContent merged(
        const std::shared_ptr<const workbench::Tree>& tree, workbench::ShapeId id,
        std::span<const workbench::NodeId> nodes
    );

    // How many answers were worked out rather than given again.
    std::size_t worked() const noexcept { return m_worked; }

  private:
    struct Key {
      std::weak_ptr<const workbench::Tree> tree;
      workbench::ShapeId id;
      std::optional<workbench::LayoutSettings> settings;
      std::vector<workbench::NodeId> nodes; // a merged piece's entries, which a new layout or other settings change
      bool operator==(const Key& other) const;
    };

    // The kept answer when `key` is the one asked before, else `work()` stored under it.
    template <typename Work> TooltipContent answer(Key key, Work work);

    std::optional<Key> m_key;
    TooltipContent m_content;
    std::size_t m_worked = 0;
  };
} // namespace luciole
