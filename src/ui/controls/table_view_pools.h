#pragma once

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

class Box;
class Label;

namespace luciole {
  // Widths of text estimated from advances measured at one size: ASCII from the font in the regular and semibold
  // weights, anything else one em.
  class TextAdvances {
  public:
    void measure(Renderer& renderer, float size);
    bool ready(float size) const noexcept { return m_size == size; }
    // Changes with every measure.
    std::uint64_t generation() const noexcept { return m_generation; }
    // Stops adding once the width passes `limit`.
    float width(std::string_view text, bool semibold, float limit) const;

  private:
    std::array<float, 128> m_regular{};
    std::array<float, 128> m_semibold{};
    float m_size = 0;
    std::uint64_t m_generation = 0;
  };

  // One-line labels under one layer of a table, each bound to a key such as a cell. A frame keeps the labels whose
  // keys stay in view, then claims free labels for the rest. Claimed labels are measured at the next layout; kept
  // ones only move with their layer.
  class TableLabelPool {
  public:
    TableLabelPool(Node& layer, FontWeight weight, ColorRole color);

    std::size_t size() const noexcept { return m_slots.size(); }

    // Starts a frame: no label is in use.
    void begin();
    // Keeps the label bound to `key` in use; false when no label holds it.
    bool keep(std::uint64_t key);
    // A label for `key` from those not in use, or a new one, in the pool's color, placed in `rect` of the layer
    // at the next layout.
    Label& claim(std::uint64_t key, LayoutRect rect);
    // Hides the labels this frame did not use.
    void end();
    // The label bound to `key`; empty when none is.
    Label* find(std::uint64_t key) const;
    // No label keeps its key, so each binds again.
    void forget();
    // Every label in use is measured again at the next layout.
    void remeasure();
    // Measures the labels claimed since the last layout and centers each in its rectangle's height.
    void arrange(Renderer& renderer);

  private:
    struct Slot {
      Label* label = nullptr;
      std::uint64_t key = 0;
      LayoutRect rect{};
      bool bound = false;
      bool used = false;
      bool pending = false;
    };

    Node& m_layer;
    FontWeight m_weight;
    ColorRole m_color;
    std::vector<Slot> m_slots;
    std::unordered_map<std::uint64_t, std::size_t> m_byKey;
    std::size_t m_cursor = 0; // free slots are searched from here within one frame
  };

  // Rule boxes under one layer of a table: those shown this frame, the rest hidden.
  class TableRulePool {
  public:
    explicit TableRulePool(Node& layer) : m_layer(layer) {}

    void show(std::size_t index, LayoutRect rect);
    void hideFrom(std::size_t count);

  private:
    Node& m_layer;
    std::vector<Box*> m_rules;
  };
} // namespace luciole
