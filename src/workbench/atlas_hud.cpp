#include "workbench/atlas_hud.h"

#include "workbench/atlas_text.h"
#include "workbench/format.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace luciole::workbench {
  namespace {
    constexpr std::array<double, 5> kChipWidths{220, 170, 130, 100, 80};
    constexpr std::array<double, 5> kChipFonts{13, 12, 12, 11, 11};
    constexpr double kEdge = 12;
    constexpr double kChipGap = 3;
    constexpr double kButton = 30;
    constexpr double kButtonGap = 4;
    constexpr double kBarEdge = 4;
    constexpr double kBarTop = 3;
    constexpr double kBarChipHeight = 20;
    constexpr double kBarButtonWidth = 24;
    constexpr double kBarButtonGap = 2;
    constexpr double kBarButtonsRoom = 84;
    constexpr double kBarChipLeast = 28;
    constexpr double kBarChipMost = 110;
    constexpr double kBarChipFewest = 44;
    constexpr double kBarFont = 11;
    constexpr double kPxPerChar = 7;
    constexpr std::size_t kCurrentChars = 30;
    constexpr double kChipLines = 1.35; // a chip is this many lines of its font tall
    constexpr double kChipFrame = 6;    // plus its padding and border above and below
    constexpr double kChipBorders = 2;  // a pixel of border at each side

    double side(double value) { return std::isfinite(value) ? std::max(0.0, value) : 0.0; }

    double chipOpacity(std::size_t index) { return std::max(0.55, 1 - static_cast<double>(index) * 0.12); }

    std::size_t charsFor(double width) {
      return static_cast<std::size_t>(std::max(0.0, std::floor(width / kPxPerChar)));
    }

    void compactHud(HudLayout& layout, std::size_t ancestorCount) {
      const double w = layout.view.width;
      const double h = layout.view.height;
      layout.map = {.x = 0, .y = kAtlasBarHeight, .width = w, .height = std::max(0.0, h - kAtlasBarHeight)};
      layout.chipArea = {
          .x = kBarEdge, .y = kBarTop, .width = std::max(0.0, w - kBarButtonsRoom - kBarEdge), .height = kBarChipHeight
      };
      const double buttonsLeft = w - kBarEdge - 3 * kBarButtonWidth - 2 * kBarButtonGap;
      for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
        layout.buttons[i] = {
            .x = buttonsLeft + static_cast<double>(i) * (kBarButtonWidth + kBarButtonGap),
            .y = kBarTop,
            .width = kBarButtonWidth,
            .height = kBarChipHeight,
        };
      }
      const double room = w - 90;
      const auto count = static_cast<std::size_t>(std::clamp(std::floor(room / 60), 3.0, 5.0));
      const double chipWidth =
          std::min(kBarChipMost, std::max(kBarChipFewest, std::floor(room / static_cast<double>(count))));
      for (std::size_t i = std::min(ancestorCount, count); i-- > 0;) {
        layout.entries.push_back(
            {.kind = HudEntryKind::Ancestor,
             .levels = i + 1,
             .maxWidth = chipWidth,
             .fontSize = kBarFont,
             .opacity = chipOpacity(i),
             .maxChars = charsFor(chipWidth)}
        );
      }
    }

    void wideHud(HudLayout& layout, std::size_t ancestorCount) {
      const double w = layout.view.width;
      const double h = layout.view.height;
      layout.map = {.x = 0, .y = 0, .width = w, .height = h};
      layout.chipArea = {
          .x = kEdge, .y = kEdge, .width = std::max(0.0, w - 2 * kEdge), .height = std::max(0.0, h - 2 * kEdge)
      };
      const double buttonsTop = h - kEdge - 3 * kButton - 2 * kButtonGap;
      for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
        layout.buttons[i] = {
            .x = w - kEdge - kButton,
            .y = buttonsTop + static_cast<double>(i) * (kButton + kButtonGap),
            .width = kButton,
            .height = kButton,
        };
      }
      const std::size_t shown = std::min(ancestorCount, kChipWidths.size());
      if (ancestorCount > shown) {
        layout.entries.push_back(
            {.kind = HudEntryKind::MoreLevels,
             .levels = ancestorCount - shown,
             .maxWidth = layout.chipArea.width,
             .fontSize = kBarFont}
        );
      }
      for (std::size_t i = shown; i-- > 0;) {
        layout.entries.push_back(
            {.kind = HudEntryKind::Ancestor,
             .levels = i + 1,
             .maxWidth = kChipWidths[i],
             .fontSize = kChipFonts[i],
             .opacity = chipOpacity(i),
             .maxChars = charsFor(kChipWidths[i])}
        );
      }
      layout.entries.push_back(
          {.kind = HudEntryKind::Current,
           .maxWidth = layout.chipArea.width,
           .fontSize = kChipFonts[0],
           .maxChars = kCurrentChars}
      );
    }

    // Shrinks widths in proportion to their size until they fit `room`, never below kBarChipLeast. Each pass
    // fixes at least one more width, so it runs at most widths.size() times.
    void shrinkToFit(std::vector<double>& widths, double room) {
      std::vector<bool> fixed(widths.size(), false);
      for (std::size_t pass = 0; pass < widths.size(); ++pass) {
        double free = 0;
        double taken = 0;
        for (std::size_t i = 0; i < widths.size(); ++i) {
          (fixed[i] ? taken : free) += widths[i];
        }
        const double overflow = free + taken - room;
        if (overflow <= 0 || free <= 0) {
          return;
        }
        bool clamped = false;
        std::vector<double> next = widths;
        for (std::size_t i = 0; i < widths.size(); ++i) {
          if (!fixed[i]) {
            next[i] = widths[i] - overflow * widths[i] / free;
            if (next[i] < kBarChipLeast) {
              next[i] = kBarChipLeast;
              fixed[i] = true;
              clamped = true;
            }
          }
        }
        if (!clamped) {
          widths = std::move(next);
          return;
        }
        for (std::size_t i = 0; i < widths.size(); ++i) {
          if (fixed[i]) {
            widths[i] = next[i];
          }
        }
      }
    }
  } // namespace

  HudLayout hudLayout(ViewSize view, std::size_t ancestorCount) {
    HudLayout layout;
    layout.view = {.width = side(view.width), .height = side(view.height)};
    layout.compact = layout.view.width < kAtlasCompactBelow || layout.view.height < kAtlasCompactBelow;
    if (layout.compact) {
      compactHud(layout, ancestorCount);
    } else {
      wideHud(layout, ancestorCount);
    }
    return layout;
  }

  std::vector<Rect> placeHud(const HudLayout& layout, std::span<const Extent> natural) {
    const auto extent = [&](std::size_t i) {
      return i < natural.size() ? Extent{.width = side(natural[i].width), .height = side(natural[i].height)} : Extent{};
    };
    std::vector<Rect> rects;
    rects.reserve(layout.entries.size());
    if (!layout.compact) {
      double y = layout.chipArea.y;
      for (std::size_t i = 0; i < layout.entries.size(); ++i) {
        const auto size = extent(i);
        rects.push_back(
            {.x = layout.chipArea.x,
             .y = y,
             .width = std::min(size.width, layout.entries[i].maxWidth),
             .height = size.height}
        );
        y += size.height + kChipGap;
      }
      return rects;
    }
    std::vector<double> widths;
    widths.reserve(layout.entries.size());
    for (std::size_t i = 0; i < layout.entries.size(); ++i) {
      widths.push_back(std::clamp(extent(i).width, kBarChipLeast, std::max(kBarChipLeast, layout.entries[i].maxWidth)));
    }
    const double gaps = widths.empty() ? 0 : kChipGap * static_cast<double>(widths.size() - 1);
    shrinkToFit(widths, layout.chipArea.width - gaps);
    double x = layout.chipArea.x;
    for (const double width : widths) {
      rects.push_back({.x = x, .y = layout.chipArea.y, .width = width, .height = kBarChipHeight});
      x += width + kChipGap;
    }
    return rects;
  }

  std::string hudText(const Tree& tree, const HudEntry& entry) {
    if (entry.kind == HudEntryKind::MoreLevels) {
      return std::format("+{} levels", entry.levels);
    }
    const std::size_t up = entry.levels - 1;
    const NodeId node = entry.kind == HudEntryKind::Current ? tree.root
        : up < tree.ancestors.size()                        ? tree.ancestors[up]
                                                            : kNoNode;
    if (!hasNode(tree, node)) {
      return {};
    }
    const auto& name = tree.nodes[node].name;
    return middleEllipsis(name.empty() ? "/" : name, entry.maxChars);
  }

  std::vector<Extent> hudExtents(const Tree& tree, const HudLayout& layout, const LabelFont& font) {
    std::vector<Extent> sizes;
    sizes.reserve(layout.entries.size());
    for (const auto& entry : layout.entries) {
      const double text = std::max(atlasTextWidth(hudText(tree, entry), font, entry.fontSize), kAtlasLeastTextRoom);
      const double pad = entry.kind == HudEntryKind::MoreLevels ? kAtlasMoreLevelsPad
          : layout.compact                                      ? 2 * kAtlasBarChipPad
                                                                : 2 * kAtlasChipPad;
      sizes.push_back({
          .width = text + pad + kChipBorders,
          .height = layout.compact ? kBarChipHeight : entry.fontSize * kChipLines + kChipFrame,
      });
    }
    return sizes;
  }

  std::string hudPath(const Tree& tree, NodeId node) {
    std::string path;
    for (const auto id : pathFromTop(tree, node)) {
      if (!path.empty() && !path.ends_with('/')) {
        path += '/';
      }
      const auto& name = tree.nodes[id].name;
      path += name.empty() ? "/" : name;
    }
    return path;
  }
} // namespace luciole::workbench
