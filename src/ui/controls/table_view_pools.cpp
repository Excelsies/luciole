#include "ui/controls/table_view_pools.h"

#include "ui/controls/box.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace luciole {
  // A run of each character between brackets, less the brackets alone, gives its advance at this size, rounding
  // included, even where a measure is of the ink.
  void TextAdvances::measure(Renderer& renderer, float size) {
    constexpr std::size_t kRun = 8;
    for (const auto weight : {FontWeight::Normal, FontWeight::SemiBold}) {
      const TextFont font{.weight = weight, .family = {}, .features = {}};
      auto& advances = weight == FontWeight::Normal ? m_regular : m_semibold;
      const float brackets = renderer.measureText("[]", size, font).width;
      for (std::size_t code = 0x20; code < 0x7F; ++code) {
        const std::string run = "[" + std::string(kRun, static_cast<char>(code)) + "]";
        advances[code] = std::max(0.0F, renderer.measureText(run, size, font).width - brackets) / kRun;
      }
    }
    m_size = size;
    ++m_generation;
  }

  float TextAdvances::width(std::string_view text, bool semibold, float limit) const {
    const auto& advances = semibold ? m_semibold : m_regular;
    float total = 0;
    for (const char c : text) {
      const auto byte = static_cast<unsigned char>(c);
      if (byte < 0x80) {
        total += advances[byte];
      } else if ((byte & 0xC0U) != 0x80U) {
        total += m_size;
      }
      if (total > limit) {
        break;
      }
    }
    return total;
  }

  TableLabelPool::TableLabelPool(Node& layer, FontWeight weight, ColorRole color)
      : m_layer(layer), m_weight(weight), m_color(color) {}

  void TableLabelPool::begin() {
    m_cursor = 0;
    for (auto& slot : m_slots) {
      slot.used = false;
    }
  }

  bool TableLabelPool::keep(std::uint64_t key) {
    const auto found = m_byKey.find(key);
    if (found == m_byKey.end()) {
      return false;
    }
    m_slots[found->second].used = true;
    return true;
  }

  Label& TableLabelPool::claim(std::uint64_t key, LayoutRect rect) {
    std::size_t index = m_slots.size();
    for (std::size_t i = m_cursor; i < m_slots.size(); ++i) {
      if (!m_slots[i].used) {
        index = i;
        break;
      }
    }
    if (index == m_slots.size()) {
      auto label = std::make_unique<Label>();
      label->setMaxLines(1);
      label->setEllipsize(TextEllipsize::End);
      label->setFontWeight(m_weight);
      label->setBaselineMode(LabelBaselineMode::TextFixedHeight);
      label->setHitTestVisible(false);
      m_slots.push_back({.label = static_cast<Label*>(m_layer.addChild(std::move(label)))});
    }
    m_cursor = index + 1;
    auto& slot = m_slots[index];
    if (slot.bound) {
      m_byKey.erase(slot.key);
    }
    slot.key = key;
    slot.rect = rect;
    slot.bound = true;
    slot.used = true;
    slot.pending = true;
    m_byKey[key] = index;
    slot.label->setColor(colorSpecFromRole(m_color));
    slot.label->setVisible(true);
    return *slot.label;
  }

  void TableLabelPool::end() {
    for (auto& slot : m_slots) {
      if (slot.used) {
        continue;
      }
      if (slot.bound) {
        m_byKey.erase(slot.key);
        slot.bound = false;
      }
      slot.label->setVisible(false);
    }
  }

  Label* TableLabelPool::find(std::uint64_t key) const {
    const auto found = m_byKey.find(key);
    return found == m_byKey.end() ? nullptr : m_slots[found->second].label;
  }

  void TableLabelPool::forget() {
    m_byKey.clear();
    for (auto& slot : m_slots) {
      slot.bound = false;
    }
  }

  void TableLabelPool::remeasure() {
    for (auto& slot : m_slots) {
      slot.pending = slot.used;
    }
  }

  void TableLabelPool::arrange(Renderer& renderer) {
    for (auto& slot : m_slots) {
      if (!slot.pending || !slot.used) {
        continue;
      }
      slot.pending = false;
      const LayoutRect rect = slot.rect;
      const float width = std::max(0.0F, rect.width);
      slot.label->Node::measure(renderer, LayoutConstraints::available(width, rect.height));
      slot.label->arrange(renderer, {.x = rect.x, .y = rect.y, .width = width, .height = 0});
      slot.label->setPosition(rect.x, rect.y + std::round((rect.height - slot.label->height()) / 2));
    }
  }

  void TableRulePool::show(std::size_t index, LayoutRect rect) {
    for (std::size_t i = m_rules.size(); i <= index; ++i) {
      auto rule = std::make_unique<Box>();
      rule->setFill(colorSpecFromRole(ColorRole::OutlineVariant));
      rule->setHitTestVisible(false);
      m_rules.push_back(static_cast<Box*>(m_layer.addChild(std::move(rule))));
    }
    Box& rule = *m_rules[index];
    rule.setPosition(rect.x, rect.y);
    rule.setFrameSize(rect.width, rect.height);
    rule.setVisible(true);
  }

  void TableRulePool::hideFrom(std::size_t count) {
    for (std::size_t i = count; i < m_rules.size(); ++i) {
      m_rules[i]->setVisible(false);
    }
  }
} // namespace luciole
