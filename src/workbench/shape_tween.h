#pragma once

#include "workbench/shape.h"

#include <cstddef>
#include <span>
#include <vector>

namespace luciole::workbench {
  // A transition between two shape lists. Shapes pair by id, the n-th occurrence of an id in the first list with
  // its n-th occurrence in the second. Paired shapes of one kind move, resize, soften and recolor (colors in linear
  // light, sector angles the short way round); a pair whose kinds differ cross-fades; unpaired shapes fade out or in.
  class ShapeTween {
  public:
    ShapeTween() = default;
    // Copies both lists and pairs them; allocation is proportional to the two list sizes.
    ShapeTween(std::span<const Shape> from, std::span<const Shape> to);

    // Replaces `out` with the shapes at `progress`: exactly the first list at 0 or below (and for NaN), exactly the
    // second at 1 or above. Between, shapes fading out come first, then the second list's shapes in its order, each
    // cross-fading shape right after its fading-out twin. Shapes fading out take no hits. Allocates only when `out`
    // holds less than capacity().
    void frame(float progress, std::vector<Shape>& out) const;

    // Most shapes a frame holds: the sum of the two list sizes.
    std::size_t capacity() const;

  private:
    struct Step {
      std::size_t from = 0; // kNone when the shape only fades in
      std::size_t to = 0;   // kNone when the shape only fades out
    };
    std::vector<Shape> m_from;
    std::vector<Shape> m_to;
    std::vector<Step> m_steps;
  };
} // namespace luciole::workbench
