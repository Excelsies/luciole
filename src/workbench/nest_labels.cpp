#include "workbench/nest_labels.h"

#include "workbench/format.h"
#include "workbench/measure.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::workbench {
  namespace {
    constexpr double kLabelPad = 8;
    constexpr double kFitMinimum = 18;
    constexpr std::size_t kSideChars = 3;        // a cut text keeps at least this many code points on each side
    constexpr std::size_t kLongestExtension = 8; // code points after the dot of an extension kept whole

    // Byte offset where each code point starts; a stray continuation byte joins the code point before it.
    std::vector<std::size_t> codePointStarts(std::string_view text) {
      std::vector<std::size_t> starts;
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 0 || (static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U) {
          starts.push_back(i);
        }
      }
      return starts;
    }

    // Code points of the extension with its dot, or 0 when the text has none short enough to keep whole.
    std::size_t extensionChars(std::string_view text, std::span<const std::size_t> starts) {
      const auto dot = text.rfind('.');
      if (dot == std::string_view::npos || dot == 0 || dot + 1 >= text.size()) {
        return 0;
      }
      const auto after =
          static_cast<std::size_t>(std::ranges::distance(std::ranges::upper_bound(starts, dot), starts.end()));
      return after <= kLongestExtension ? after + 1 : 0;
    }

    // The first `head` and last `tail` code points joined by an ellipsis.
    std::string
    cutText(std::string_view text, std::span<const std::size_t> starts, std::size_t head, std::size_t tail) {
      std::string result(text.substr(0, starts[head]));
      result += "…";
      result += text.substr(starts[starts.size() - tail]);
      return result;
    }

    // The text that stays when fitNestText cuts to the least: three code points, the ellipsis and the extension or
    // three code points. Empty when the text has no cut.
    std::string narrowestCut(std::string_view text) {
      const auto starts = codePointStarts(text);
      const std::size_t extension = extensionChars(text, starts);
      const std::size_t kept = std::max(2 * kSideChars, extension + kSideChars);
      if (starts.size() <= kept) {
        return {};
      }
      const std::size_t tail = std::max({kept / 2, extension, kSideChars});
      return cutText(text, starts, kept - tail, tail);
    }

    constexpr double kIconRoomFrom = 110; // tiles this wide and 40 px tall show an icon
    constexpr double kIconRoomHeight = 40;
    constexpr double kIconSize = 14;
    constexpr double kIconGap = 6;
    constexpr double kLabelMinRoom = 28;
    constexpr double kLabelMinHeight = 22;

    bool hasIcon(TreemapRect rect) { return rect.w >= kIconRoomFrom && rect.h >= kIconRoomHeight; }
  } // namespace

  double nestTileNameRoom(TreemapRect rect) {
    const double available = rect.w - kLabelPad * 2;
    if (!std::isfinite(rect.w) || !std::isfinite(rect.h) || available < kLabelMinRoom || rect.h < kLabelMinHeight) {
      return 0;
    }
    return available - (hasIcon(rect) ? kIconSize + kIconGap : 0);
  }

  double narrowestNestTextWidth(std::string_view text, const NestTextMeasure& measure) {
    if (text.empty()) {
      return 0;
    }
    const double whole = measure(text);
    const auto cut = narrowestCut(text);
    return cut.empty() ? whole : std::min(whole, measure(cut));
  }

  std::optional<NestTileLabel>
  nestTileLabel(const Tree& tree, const LayoutSettings& settings, NodeId id, TreemapRect rect) {
    const double available = rect.w - kLabelPad * 2;
    if (!hasNode(tree, id) || !std::isfinite(rect.x) || !std::isfinite(rect.y) || nestTileNameRoom(rect) <= 0) {
      return std::nullopt;
    }
    const auto& node = tree.nodes[id];
    const bool big = rect.w >= 200 && rect.h >= 90;
    const double fontSize = big ? 15 : (rect.w >= 130 && rect.h >= 52 ? 13 : 12);
    const double iconSize = hasIcon(rect) ? kIconSize : 0;
    const double iconRoom = iconSize > 0 ? iconSize + kIconGap : 0;
    const bool twoLines = rect.h >= 46;
    const auto measure = measureLabel(tree, id, settings.sizeBy);
    const double lineY = twoLines ? rect.y + kLabelPad + fontSize : rect.y + rect.h / 2 + fontSize * 0.36;
    NestTileLabel label{
        .name = node.name,
        .detail = measure,
        .fontSize = fontSize,
        .iconSize = iconSize,
        .icon =
            {.x = rect.x + kLabelPad,
             .y = twoLines ? rect.y + kLabelPad + fontSize / 2 - iconSize / 2 - 1 : rect.y + rect.h / 2 - iconSize / 2},
        .category = node.category < kFileCategoryCount ? static_cast<FileCategory>(node.category) : FileCategory::Other,
        .twoLines = twoLines,
        .nameAt = {.x = rect.x + kLabelPad + iconRoom, .y = lineY},
        .nameWidth = available - iconRoom,
        .detailAt = twoLines ? TreemapPoint{.x = rect.x + kLabelPad, .y = lineY + 17}
                             : TreemapPoint{.x = rect.x + rect.w - kLabelPad, .y = lineY},
        .detailWidth = twoLines ? available : available * 0.45,
    };
    if (twoLines && node.folder && available > 150) {
      // The count is the measure itself under Size by Items: told once.
      if (auto items = itemsLabel(tree, id); items != label.detail) {
        label.detail += std::string(kFactsDot) + items;
      }
    }
    return label;
  }

  bool nestMeasureShown(const NestTileLabel& label, double measureWidth) {
    return !label.twoLines && measureWidth + 14 < label.detailWidth;
  }

  double nestNameRoom(const NestTileLabel& label, double measureWidth) {
    return nestMeasureShown(label, measureWidth) ? label.nameWidth - (measureWidth + 14) : label.nameWidth;
  }

  NestMoreLabel nestMoreLabel(std::size_t count, double bytes, MeasureState state, TreemapRect rect) {
    const auto number = "+" + countText(static_cast<double>(count));
    const bool twoLines = rect.h >= 46;
    return {
        .text = number + " more",
        .shortText = number,
        .maxWidth = rect.w - 8,
        .center = {.x = rect.x + rect.w / 2, .y = rect.y + rect.h / 2 - (twoLines ? 8 : 0)},
        .size = twoLines ? stateText(state, bytes, sizeText(bytes)) : std::string{},
    };
  }

  std::string fitNestText(std::string_view text, double maxWidth, const NestTextMeasure& measure) {
    if (!(maxWidth >= kFitMinimum)) {
      return {};
    }
    if (measure(text) <= maxWidth) {
      return std::string(text);
    }
    const auto starts = codePointStarts(text);
    const std::size_t count = starts.size();
    const std::size_t extension = extensionChars(text, starts);
    // The tail of `kept` code points: half of them, and at least the extension.
    const auto tailOf = [extension](std::size_t kept) { return std::max({kept / 2, extension, kSideChars}); };
    std::size_t low = std::max(2 * kSideChars, extension + kSideChars);
    if (count <= low) {
      return {};
    }
    std::size_t high = count - 1;
    std::string best;
    // Halves the range each step, so it ends within log2 of the text length.
    while (low <= high) {
      const std::size_t kept = (low + high) / 2;
      const std::size_t tail = tailOf(kept);
      auto candidate = cutText(text, starts, kept - tail, tail);
      if (measure(candidate) <= maxWidth) {
        best = std::move(candidate);
        low = kept + 1;
      } else {
        high = kept - 1;
      }
    }
    return best;
  }
} // namespace luciole::workbench
