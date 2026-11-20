#include "workbench/atlas_text.h"

#include "workbench/atlas_labels.h"
#include "workbench/format.h"
#include "workbench/nest_labels.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>

namespace luciole::workbench {
  namespace {
    constexpr double kSplitFrom = 40;      // circles this large may split a name
    constexpr double kKeepCutRows = 56;    // and keep two cut lines from this size
    constexpr std::size_t kSplitChars = 8; // names this long may split
    constexpr std::string_view kSeparators = "-_. ";

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
  } // namespace

  double atlasTextWidth(std::string_view text, const LabelFont& font, double size) {
    if (!std::isfinite(size) || size <= 0) {
      return 0;
    }
    return labelTextWidth(text, font) * size / kAtlasFontBase;
  }

  std::string fitAtlasText(std::string_view text, double maxWidth, const LabelFont& font, double size) {
    return fitNestText(text, maxWidth, [&font, size](std::string_view cut) { return atlasTextWidth(cut, font, size); });
  }

  std::array<std::string, 2> splitAtlasName(std::string_view name, const LabelFont& font, double size) {
    const auto starts = codePointStarts(name);
    const std::size_t chars = starts.size();
    std::array<std::string, 2> best;
    double bestWidth = std::numeric_limits<double>::infinity();
    const auto tryAt = [&](std::size_t index) {
      const std::size_t at = index < chars ? starts[index] : name.size();
      const auto head = name.substr(0, at);
      const auto tail = name.substr(at);
      const double width = std::max(atlasTextWidth(head, font, size), atlasTextWidth(tail, font, size));
      if (width < bestWidth) {
        bestWidth = width;
        best = {std::string(head), std::string(tail)};
      }
    };
    for (std::size_t i = 3; i + 2 < chars; ++i) {
      const auto before = name.substr(starts[i - 1], starts[i] - starts[i - 1]);
      if (before.size() == 1 && kSeparators.contains(before.front())) {
        tryAt(i);
      }
    }
    if (!std::isfinite(bestWidth)) {
      tryAt((chars + 1) / 2);
    }
    return best;
  }

  std::vector<std::string>
  atlasNameRows(std::string_view name, double r, double total, double rowHeight, const LabelFont& font, double size) {
    if (name.empty()) {
      return {};
    }
    const double one = labelWidth(r, total / 2);
    if (atlasTextWidth(name, font, size) <= one) {
      return {std::string(name)};
    }
    auto single = fitAtlasText(name, one, font, size);
    if (r >= kSplitFrom && codePointCount(name) >= kSplitChars) {
      const double two = labelWidth(r, (total + rowHeight) / 2);
      const auto pair = splitAtlasName(name, font, size);
      std::vector<std::string> rows{fitAtlasText(pair[0], two, font, size), fitAtlasText(pair[1], two, font, size)};
      const bool both = !rows[0].empty() && !rows[1].empty();
      const bool whole = both && rows[0] == pair[0] && rows[1] == pair[1];
      if (whole || (both && !single.empty() && r >= kKeepCutRows)) {
        return rows;
      }
      if (!single.empty()) {
        return {std::move(single)};
      }
      return both ? rows : std::vector<std::string>{};
    }
    if (single.empty()) {
      return {};
    }
    return {std::move(single)};
  }
} // namespace luciole::workbench
