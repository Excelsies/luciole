#include "formats/markdown.h"

#include "formats/markdown_blocks.h"
#include "formats/markdown_text.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::formats::markdown {
  namespace {
    constexpr std::size_t kTabWidth = 4;

    // Lines split at "\r\n", "\r" or "\n"; a final line ending ends the last line. Stops once more than
    // kMaxLines lines are certain, and says so in `more`.
    std::vector<std::string_view> splitLines(std::string_view input, bool& more) {
      std::vector<std::string_view> lines;
      std::size_t start = 0;
      for (std::size_t p = 0; p < input.size() && !more; ++p) {
        if (input[p] != '\r' && input[p] != '\n') {
          continue;
        }
        lines.push_back(input.substr(start, p - start));
        if (input[p] == '\r' && charAt(input, p + 1) == '\n') {
          ++p;
        }
        start = p + 1;
        more = lines.size() > kMaxLines;
      }
      if (more) {
        return lines;
      }
      lines.push_back(input.substr(start));
      if (lines.size() > 1 && lines.back().empty()) {
        lines.pop_back();
      }
      return lines;
    }

    // Leading tabs become four spaces each; the rest of the line keeps its input offsets.
    Line readLine(Context& context, std::string_view input, std::string_view raw) {
      const auto origin = static_cast<std::size_t>(raw.data() - input.data());
      std::size_t lead = 0;
      bool tab = false;
      while (lead < raw.size() && (raw[lead] == ' ' || raw[lead] == '\t')) {
        tab = tab || raw[lead] == '\t';
        ++lead;
      }
      if (!tab) {
        return {.text = raw, .mappedFrom = 0, .origin = origin};
      }
      std::string expanded;
      for (std::size_t k = 0; k < lead; ++k) {
        expanded.append(raw[k] == '\t' ? kTabWidth : 1, ' ');
      }
      const std::size_t mappedFrom = expanded.size();
      expanded += raw.substr(lead);
      context.expanded.push_back(std::move(expanded));
      return {.text = context.expanded.back(), .mappedFrom = mappedFrom, .origin = origin + lead};
    }

    template <typename T> std::span<const T> slice(const std::vector<T>& values, Range range) {
      if (range.first > values.size() || range.count > values.size() - range.first) {
        return {};
      }
      return std::span<const T>(values).subspan(range.first, range.count);
    }
  } // namespace

  Document parse(std::string_view text) {
    Document document;
    const std::string_view input = text.substr(0, cutPoint(text, kMaxChars));
    document.text = std::string(input);
    document.sourceSize = input.size();
    document.truncated = input.size() < text.size();
    Context context{.document = document, .expanded = {}};
    bool more = false;
    const std::vector<std::string_view> raw = splitLines(input, more);
    if (more || raw.size() > kMaxLines) {
      document.truncated = true;
    }
    std::vector<Line> lines;
    lines.reserve(std::min(raw.size(), kMaxLines));
    for (std::size_t k = 0; k < raw.size() && k < kMaxLines; ++k) {
      const std::size_t keep = cutPoint(raw[k], kMaxLine);
      if (keep < raw[k].size()) {
        document.truncated = true;
      }
      lines.push_back(readLine(context, input, raw[k].substr(0, keep)));
    }
    document.root = parseBlocks(context, lines, 0);
    return document;
  }

  std::string_view text(const Document& document, Span span) {
    if (span.offset > document.text.size() || span.length > document.text.size() - span.offset) {
      return {};
    }
    return std::string_view(document.text).substr(span.offset, span.length);
  }

  std::span<const Block> blocks(const Document& document, Range range) { return slice(document.blocks, range); }

  std::span<const Run> runs(const Document& document, Range range) { return slice(document.runs, range); }

  std::span<const Range> items(const Document& document, Range range) { return slice(document.items, range); }

  std::span<const Range> cells(const Document& document, Range range) { return slice(document.cells, range); }

  std::span<const Align> alignments(const Document& document, Range range) { return slice(document.alignments, range); }
} // namespace luciole::formats::markdown
