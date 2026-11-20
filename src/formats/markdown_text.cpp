#include "formats/markdown_text.h"

#include "formats/unicode_space.h"

#include <algorithm>
#include <utility>

namespace luciole::formats::markdown {
  char charAt(std::string_view text, std::size_t index) { return index < text.size() ? text[index] : '\0'; }

  std::string_view trimmed(std::string_view text) {
    const std::string_view rest = text.substr(leadingUnicodeSpace(text));
    return rest.substr(0, rest.size() - trailingUnicodeSpace(rest));
  }

  bool isBlank(std::string_view text) { return leadingUnicodeSpace(text) == text.size(); }

  std::size_t indentOf(std::string_view text) {
    std::size_t count = 0;
    while (count < text.size() && text[count] == ' ') {
      ++count;
    }
    return count;
  }

  std::size_t sequenceLength(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
      return 0;
    }
    const auto lead = static_cast<unsigned char>(text[pos]);
    std::size_t length = 1;
    if (lead >= 0xF0U && lead <= 0xF4U) {
      length = 4;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      length = 3;
    } else if (lead >= 0xC2U && lead <= 0xDFU) {
      length = 2;
    }
    if (length > text.size() - pos) {
      return 1;
    }
    for (std::size_t k = 1; k < length; ++k) {
      if ((static_cast<unsigned char>(text[pos + k]) & 0xC0U) != 0x80U) {
        return 1;
      }
    }
    return length;
  }

  std::size_t unitsOf(std::size_t length) { return length == 4 ? 2 : 1; }

  std::size_t unitCount(std::string_view text) {
    std::size_t units = 0;
    for (std::size_t pos = 0; pos < text.size();) { // each step moves forward by 1 to 4 bytes
      const std::size_t length = sequenceLength(text, pos);
      units += unitsOf(length);
      pos += length;
    }
    return units;
  }

  std::size_t cutPoint(std::string_view text, std::size_t limit) {
    std::size_t pos = 0;
    std::size_t units = 0;
    while (pos < text.size()) { // each step moves forward by 1 to 4 bytes
      const std::size_t length = sequenceLength(text, pos);
      units += unitsOf(length);
      if (units > limit) {
        break;
      }
      pos += length;
    }
    return pos;
  }

  std::size_t originAt(const Line& line, std::size_t index) {
    return line.origin == kNoOrigin || index < line.mappedFrom ? kNoOrigin : line.origin + (index - line.mappedFrom);
  }

  Line sliceLine(const Line& line, std::size_t from) {
    const std::size_t start = std::min(from, line.text.size());
    if (start <= line.mappedFrom) {
      return {.text = line.text.substr(start), .mappedFrom = line.mappedFrom - start, .origin = line.origin};
    }
    return {.text = line.text.substr(start), .mappedFrom = 0, .origin = originAt(line, start)};
  }

  Line trimStart(const Line& line) { return sliceLine(line, leadingUnicodeSpace(line.text)); }

  Line trimLine(const Line& line) {
    Line out = trimStart(line);
    out.text.remove_suffix(trailingUnicodeSpace(out.text));
    return out;
  }

  void trimSource(Source& source) {
    const std::size_t lead = leadingUnicodeSpace(source.text);
    const std::size_t trail = trailingUnicodeSpace(std::string_view(source.text).substr(lead));
    const auto keepEnd = static_cast<std::ptrdiff_t>(source.text.size() - trail);
    source.text.erase(source.text.begin() + keepEnd, source.text.end());
    source.origin.erase(source.origin.begin() + keepEnd, source.origin.end());
    source.text.erase(0, lead);
    source.origin.erase(source.origin.begin(), source.origin.begin() + static_cast<std::ptrdiff_t>(lead));
  }

  Span store(Context& context, const Piece& piece) {
    Document& document = context.document;
    const std::size_t length = piece.text.size();
    if (length == 0) {
      return {};
    }
    if (piece.origin != kNoOrigin
        && piece.origin <= document.sourceSize
        && length <= document.sourceSize - piece.origin
        && std::string_view(document.text).substr(piece.origin, length) == piece.text) {
      return {.offset = piece.origin, .length = length};
    }
    const Span span{.offset = document.text.size(), .length = length};
    document.text += piece.text;
    return span;
  }

  Range commit(Context& context, std::vector<Pending>&& nodes) {
    std::vector<Run> stored;
    stored.reserve(nodes.size());
    for (const Pending& node : nodes) {
      stored.push_back(
          Run{.kind = node.kind,
              .text = store(context, node.text),
              .alt = store(context, node.alt),
              .children = node.children}
      );
    }
    return appendAll(context.document.runs, std::move(stored));
  }
} // namespace luciole::formats::markdown
