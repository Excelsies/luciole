#include "workbench/format.h"

#include "workbench/rounding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <ranges>
#include <vector>

namespace luciole::workbench {
  namespace {
    constexpr std::string_view kEllipsis = "…";
    constexpr std::array<std::string_view, 6> kSizeUnits{"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    constexpr double kLargestSize = 18446744073709551616.0; // 2^64, the largest byte count the list writes
    constexpr std::string_view kUnitSpace = "\u00A0";       // no-break space between a number and its unit

    struct AgeStep {
      double limit;
      std::string_view unit;
      double divisor;
    };
    constexpr std::array<AgeStep, 5> kAgeSteps{{
        {.limit = 3600.0, .unit = "min", .divisor = 60.0},
        {.limit = 86400.0, .unit = "h", .divisor = 3600.0},
        {.limit = 86400.0 * 30.0, .unit = "d", .divisor = 86400.0},
        {.limit = 86400.0 * 365.0, .unit = "mo", .divisor = 86400.0 * 30.0},
        {.limit = std::numeric_limits<double>::infinity(), .unit = "y", .divisor = 86400.0 * 365.0},
    }};

    // Byte offsets where code points start; a stray continuation byte stays with the code point before it.
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

  std::size_t codePointCount(std::string_view text) {
    if (text.empty()) {
      return 0;
    }
    const auto lead = [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; };
    return 1 + static_cast<std::size_t>(std::ranges::count_if(text.substr(1), lead));
  }

  std::string sizeText(double bytes) {
    if (!std::isfinite(bytes) || bytes < 0.0) {
      return {};
    }
    const double whole = std::min(roundHalfUp(bytes), kLargestSize);
    if (whole < 1024.0) {
      return std::format("{:.0f} {}", whole, whole == 1.0 ? "byte" : "bytes");
    }
    // The largest unit the count reaches, divided once so the decimal rounds as the list's does.
    double factor = 1024.0;
    std::size_t unit = 0;
    while (unit + 1 < kSizeUnits.size() && whole >= factor * 1024.0) {
      factor *= 1024.0;
      ++unit;
    }
    return std::format("{:.1f}{}{}", whole / factor, kUnitSpace, kSizeUnits[unit]);
  }

  std::string countText(double count) {
    if (!std::isfinite(count)) {
      return {};
    }
    const double rounded = roundHalfUp(count);
    const auto digits = std::format("{:.0f}", std::abs(rounded));
    std::string text = rounded < 0.0 ? "-" : "";
    text.reserve(text.size() + digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i) {
      if (i > 0 && (digits.size() - i) % 3 == 0) {
        text += ',';
      }
      text += digits[i];
    }
    return text;
  }

  std::string countedText(double count, std::string_view singular) {
    if (!std::isfinite(count)) {
      return {};
    }
    const bool one = roundHalfUp(count) == 1.0;
    return std::format("{} {}{}", countText(count), singular, one ? "" : "s");
  }

  std::string itemsText(double count) { return countedText(count, "item"); }

  std::string agoText(double seconds) {
    if (!std::isfinite(seconds)) {
      return {};
    }
    if (seconds < 0.0) {
      return "in the future";
    }
    if (seconds < 60.0) {
      return "just now";
    }
    for (const auto& step : kAgeSteps) {
      if (seconds < step.limit) {
        return std::format("{:.0f} {} ago", std::floor(seconds / step.divisor), step.unit);
      }
    }
    return {};
  }

  std::string shareText(double share) {
    if (!std::isfinite(share) || share < 0) {
      return {};
    }
    const double percent = share * 100;
    if (percent == 0) {
      return "0%";
    }
    if (percent < 0.05) {
      return "<0.1%";
    }
    return percent < 9.95 ? std::format("{:.1f}%", percent) : std::format("{:.0f}%", percent);
  }

  std::string middleEllipsis(std::string_view text, std::size_t maxChars) {
    if (maxChars == 0) {
      return {};
    }
    const auto starts = codePointStarts(text);
    if (starts.size() <= maxChars) {
      return std::string(text);
    }
    if (maxChars == 1) {
      return std::string(kEllipsis);
    }
    const auto keep = maxChars - 1;
    const auto head = (keep + 1) / 2;
    const auto tail = keep / 2;
    std::string result(text.substr(0, starts[head]));
    result += kEllipsis;
    result += tail == 0 ? std::string_view{} : text.substr(starts[starts.size() - tail]);
    return result;
  }

  std::optional<std::string> relativePath(const Tree& tree, NodeId id, NodeId base) {
    if (!hasNode(tree, id) || !hasNode(tree, base)) {
      return std::nullopt;
    }
    std::vector<NodeId> path;
    // One step per node at most, so a cycle ends.
    for (auto current = id; current != base; current = tree.nodes[current].parent) {
      if (!hasNode(tree, current) || path.size() >= tree.nodes.size()) {
        return std::nullopt;
      }
      path.push_back(current);
    }
    std::string text;
    for (const auto step : std::views::reverse(path)) {
      if (!text.empty()) {
        text += '/';
      }
      text += tree.nodes[step].name;
    }
    return text;
  }

  PathSplit splitRelativePath(std::string_view path, std::size_t maxChars) {
    const auto slash = path.rfind('/');
    const auto prefix = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash + 1);
    const auto leaf = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto leafChars = codePointCount(leaf);
    if (leafChars >= maxChars) {
      return {.prefix = {}, .leaf = middleEllipsis(leaf, maxChars)};
    }
    const auto room = maxChars - leafChars;
    if (codePointCount(prefix) <= room) {
      return {.prefix = std::string(prefix), .leaf = std::string(leaf)};
    }
    return {.prefix = room >= 3 ? middleEllipsis(prefix, room) : std::string{}, .leaf = std::string(leaf)};
  }
} // namespace luciole::workbench
