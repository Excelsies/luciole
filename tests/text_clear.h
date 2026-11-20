#pragma once

#include "render/scene/node.h"
#include "render/scene/text_node.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"

#include <algorithm>
#include <cstddef>
#include <print>
#include <utility>
#include <vector>

namespace test {
  struct TextBox {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float right() const { return x + width; }
    float bottom() const { return y + height; }
  };

  inline TextBox textBoxOf(const Node& node) {
    TextBox box{.width = node.width(), .height = node.height()};
    Node::absolutePosition(&node, box.x, box.y);
    return box;
  }

  // Labels that lie flat: a turned label's frame is not where its text is.
  // Where a label's glyphs lie: its em box, centered in its frame, which also holds the font's leading.
  inline TextBox glyphBoxOf(const Label& label, float textScale) {
    auto frame = textBoxOf(label);
    const float em = std::min(frame.height, label.fontSize() * textScale);
    frame.y += (frame.height - em) / 2;
    frame.height = em;
    return frame;
  }

  inline void collectLabels(const Node& node, std::vector<const Label*>& out) {
    if (!node.visible() || node.rotation() != 0.0F) {
      return;
    }
    if (const auto* label = dynamic_cast<const Label*>(&node); label != nullptr) {
      out.push_back(label);
    }
    for (const auto& child : node.children()) {
      collectLabels(*child, out);
    }
  }

  // Every visible text under `root` lies within `root`'s own height: a row in a list shows its text in the row's
  // cell, and what leaves it lies over the next row.
  inline void expectTextWithin(const Node& root, float textScale) {
    std::vector<const Label*> labels;
    collectLabels(root, labels);
    const auto cell = textBoxOf(root);
    for (const auto* label : labels) {
      for (const auto& child : label->children()) {
        const auto* text = dynamic_cast<const TextNode*>(child.get());
        if (text == nullptr || !text->visible() || text->text().empty()) {
          continue;
        }
        const auto ink = glyphBoxOf(*label, textScale);
        if (ink.y < cell.y - 0.75F || ink.bottom() > cell.bottom() + 0.75F) {
          std::println(
              "  text '{}' {:.1f}..{:.1f} leaves its row {:.1f}..{:.1f}", text->text(), ink.y, ink.bottom(), cell.y,
              cell.bottom()
          );
          TEST_CHECK(false);
        }
      }
    }
  }

  // The visible texts under `root`, laid out: none is cut off by a bar or row that clips its children and no two
  // overlap. A scrolling list may cut a row at its edge, so the clip check stops at
  // one. Returns how many texts it checked.
  inline std::size_t expectTextClear(const Node& root, float textScale) {
    std::vector<const Label*> labels;
    collectLabels(root, labels);
    std::vector<std::pair<const Label*, TextBox>> texts;
    for (const auto* label : labels) {
      if (label->text().empty() || label->width() <= 0) {
        continue;
      }
      for (const auto& child : label->children()) {
        const auto* text = dynamic_cast<const TextNode*>(child.get());
        if (text == nullptr || !text->visible() || text->text().empty()) {
          continue;
        }
        const auto ink = glyphBoxOf(*label, textScale);
        for (const Node* up = label->parent(); up != nullptr; up = up->parent()) {
          if (dynamic_cast<const ScrollView*>(up) != nullptr || dynamic_cast<const VirtualGridView*>(up) != nullptr) {
            break;
          }
          if (up->clipChildren()) {
            const auto held = textBoxOf(*up);
            if (ink.y < held.y - 0.75F || ink.bottom() > held.bottom() + 0.75F) {
              std::println(
                  "  text '{}' {:.1f}..{:.1f} is clipped by {:.1f}..{:.1f}", text->text(), ink.y, ink.bottom(), held.y,
                  held.bottom()
              );
              TEST_CHECK(false);
            }
          }
        }
        texts.emplace_back(label, ink);
      }
    }
    for (std::size_t a = 0; a < texts.size(); ++a) {
      for (std::size_t b = a + 1; b < texts.size(); ++b) {
        const auto& first = texts[a].second;
        const auto& second = texts[b].second;
        const float across = std::min(first.right(), second.right()) - std::max(first.x, second.x);
        const float down = std::min(first.bottom(), second.bottom()) - std::max(first.y, second.y);
        if (across > 1.0F && down > 1.5F) {
          std::println(
              "  texts overlap: '{}' and '{}' by {:.1f} x {:.1f}", texts[a].first->text(), texts[b].first->text(),
              across, down
          );
          TEST_CHECK(false);
        }
      }
    }
    return texts.size();
  }
} // namespace test
