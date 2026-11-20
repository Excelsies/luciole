#pragma once

#include "workbench/tree.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace luciole::workbench {
  // A relative path fitted to a width: the leaf is kept whole when it fits, the prefix gives way first.
  struct PathSplit {
    std::string prefix; // ends with '/' when not empty
    std::string leaf;
    bool operator==(const PathSplit&) const = default;
  };

  // As the list writes sizes: "0 bytes", "1 byte", "1023 bytes", then one decimal of the largest unit of 1024 the
  // count reaches: "1.0 KiB", "37.0 MiB", up to EiB. Rounded to whole bytes first, at most 2^64. Empty for negative or
  // non-finite.
  std::string sizeText(double bytes);

  // Rounded, with comma thousands separators. Empty for non-finite values.
  std::string countText(double count);

  // The count as countText writes it and the noun in the plural unless the printed count is 1: "1 result", "2,000
  // folders".
  std::string countedText(double count, std::string_view singular);

  // "1 item", "2,000 items".
  std::string itemsText(double count);

  // A share of 0 to 1 as a percentage: one decimal under 9.95 %, "<0.1%" above 0 and under that. Empty for a
  // negative or non-finite share.
  std::string shareText(double share);

  // Elapsed time: "just now", "5 min ago", "3 h ago", "2 d ago", "3 mo ago", "2 y ago", or "in the future".
  // Empty for non-finite values.
  std::string agoText(double seconds);

  // Code points in UTF-8 text. The first byte always starts one; a later stray continuation byte joins the code
  // point before it.
  std::size_t codePointCount(std::string_view text);

  // Keeps the start and end of `text` within `maxChars` code points, joined by an ellipsis.
  std::string middleEllipsis(std::string_view text, std::size_t maxChars);

  // Names from `base` down to `id` joined by '/': empty when they are the same node, none when `base` is not
  // above `id`.
  std::optional<std::string> relativePath(const Tree& tree, NodeId id, NodeId base);

  // Widths count code points. A prefix with fewer than 3 code points of room is dropped.
  PathSplit splitRelativePath(std::string_view path, std::size_t maxChars);
} // namespace luciole::workbench
