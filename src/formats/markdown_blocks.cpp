#include "formats/markdown_blocks.h"

#include "formats/markdown_inline.h"
#include "formats/markdown_lines.h"
#include "formats/unicode_space.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace luciole::formats::markdown {
  namespace {
    constexpr std::size_t kCodeIndent = 4;

    struct Parsed {
      std::size_t end = 0;
      Block block;
    };

    // Lines joined by '\n', leading space trimmed from each and trailing space from the last, cut to kMaxInline.
    Source paragraphText(std::span<const Line> lines) {
      Source source;
      for (std::size_t k = 0; k < lines.size(); ++k) {
        const Line line = k + 1 == lines.size() ? trimLine(lines[k]) : trimStart(lines[k]);
        if (k > 0) {
          const Line previous = trimStart(lines[k - 1]);
          source.append('\n', originAt(previous, previous.text.size()));
        }
        source.append(line);
      }
      const std::size_t keep = cutPoint(source.text, kMaxInline);
      source.text.resize(keep);
      source.origin.resize(keep);
      return source;
    }

    Range paragraphRuns(Context& context, std::span<const Line> lines) {
      std::size_t total = 0;
      for (const Line& line : lines) {
        total += unitCount(line.text);
      }
      if (total > kMaxInline) {
        context.document.truncated = true;
      }
      return parseInline(context, paragraphText(lines), 0);
    }

    // Joins lines with '\n'; the joint claims the input offset after the line before it.
    Piece joinLines(std::span<const Line> lines) {
      Piece piece;
      for (std::size_t k = 0; k < lines.size(); ++k) {
        if (k > 0) {
          piece.append("\n", originAt(lines[k - 1], lines[k - 1].text.size()));
        }
        piece.append(lines[k].text, originAt(lines[k], 0));
      }
      return piece;
    }

    Block codeBlock(Context& context, std::span<const Line> body, const Line& lang) {
      return {
          .kind = BlockKind::Code,
          .text = store(context, joinLines(body)),
          .lang = store(context, {.text = std::string(lang.text), .origin = originAt(lang, 0)})
      };
    }

    Parsed collectFence(Context& context, std::span<const Line> lines, std::size_t i, const Fence& fence) {
      std::vector<Line> body;
      std::size_t j = i + 1;
      for (; j < lines.size(); ++j) {
        const Line& line = lines[j];
        const std::size_t indent = indentOf(line.text);
        std::size_t count = 0;
        while (indent <= kMaxIndent && charAt(line.text, indent + count) == fence.ch) {
          ++count;
        }
        if (count >= fence.count && isBlank(sliceLine(line, indent + count).text)) {
          ++j;
          break;
        }
        body.push_back(sliceLine(line, std::min(indent, fence.indent)));
      }
      return {.end = j, .block = codeBlock(context, body, fence.lang)};
    }

    Parsed collectIndented(Context& context, std::span<const Line> lines, std::size_t i) {
      std::size_t last = i;
      for (std::size_t j = i; j < lines.size() && (isBlank(lines[j].text) || indentOf(lines[j].text) >= kCodeIndent);
           ++j) {
        if (!isBlank(lines[j].text)) {
          last = j;
        }
      }
      std::vector<Line> body;
      for (std::size_t j = i; j <= last; ++j) {
        body.push_back(isBlank(lines[j].text) ? Line{} : sliceLine(lines[j], kCodeIndent));
      }
      return {.end = last + 1, .block = codeBlock(context, body, {})};
    }

    Parsed collectQuote(Context& context, std::span<const Line> lines, std::size_t i, std::size_t depth) {
      std::vector<Line> inner;
      std::size_t j = i;
      for (; j < lines.size(); ++j) {
        const Line& line = lines[j];
        if (startsQuote(line.text)) {
          const Line rest = sliceLine(line, indentOf(line.text) + 1);
          inner.push_back(charAt(rest.text, 0) == ' ' ? sliceLine(rest, 1) : rest);
        } else if (!isBlank(line.text) && !inner.empty() && !isBlank(inner.back().text) && !interrupts(line)) {
          inner.push_back(line);
        } else {
          break;
        }
      }
      return {.end = j, .block = {.kind = BlockKind::Quote, .children = parseBlocks(context, inner, depth + 1)}};
    }

    struct Item {
      std::vector<Line> body;
      std::size_t end = 0;
    };

    // One list item starting at lines[i]: its lines with the marker's indent removed.
    Item collectItem(std::span<const Line> lines, std::size_t i, const Marker& marker) {
      Item item{.body = {sliceLine(lines[i], marker.content)}, .end = i + 1};
      std::size_t& j = item.end;
      while (j < lines.size()) { // every pass moves j forward or stops
        const Line& line = lines[j];
        if (isBlank(line.text)) {
          std::size_t k = j;
          while (k < lines.size() && isBlank(lines[k].text)) {
            ++k;
          }
          if (k < lines.size() && indentOf(lines[k].text) >= marker.content) {
            for (; j < k; ++j) {
              item.body.emplace_back();
            }
            continue;
          }
          break;
        }
        if (indentOf(line.text) >= marker.content) {
          item.body.push_back(sliceLine(line, marker.content));
        } else if (!isBlank(item.body.back().text) && !interrupts(line) && !markerOf(line.text)) {
          item.body.push_back(trimLine(line));
        } else {
          break;
        }
        ++j;
      }
      return item;
    }

    Parsed listAt(Context& context, std::span<const Line> lines, std::size_t i, std::size_t depth) {
      const Marker first = *markerOf(lines[i].text);
      std::vector<Range> items;
      std::size_t j = i;
      while (j < lines.size() && items.size() < kMaxRows) { // every item moves j forward
        const std::optional<Marker> marker = markerOf(lines[j].text);
        if (!marker
            || marker->ordered != first.ordered
            || marker->bullet != first.bullet
            || marker->indent > first.indent + kMaxIndent) {
          break;
        }
        const Item item = collectItem(lines, j, *marker);
        items.push_back(parseBlocks(context, item.body, depth + 1));
        j = item.end;
        std::size_t k = j;
        while (k < lines.size() && isBlank(lines[k].text)) {
          ++k;
        }
        if (k < lines.size() && k > j && markerOf(lines[k].text)) {
          j = k;
        }
      }
      return {
          .end = j,
          .block = {
              .kind = BlockKind::List,
              .ordered = first.ordered,
              .start = first.number,
              .children = appendAll(context.document.items, std::move(items))
          }
      };
    }

    // Cells between pipes; "\|" is a literal pipe, outer pipes are optional, at most kMaxColumns cells.
    std::vector<Source> splitRow(const Line& line) {
      Line row = trimLine(line);
      if (charAt(row.text, 0) == '|') {
        row = sliceLine(row, 1);
      }
      std::vector<Source> cells;
      Source current;
      for (std::size_t i = 0; i < row.text.size() && cells.size() < kMaxColumns; ++i) {
        if (row.text[i] == '\\' && charAt(row.text, i + 1) == '|') {
          current.append('|', originAt(row, i + 1));
          ++i;
        } else if (row.text[i] == '|') {
          trimSource(current);
          cells.push_back(std::move(current));
          current = {};
        } else {
          current.append(row.text[i], originAt(row, i));
        }
      }
      if (!isBlank(current.text) && cells.size() < kMaxColumns) {
        trimSource(current);
        cells.push_back(std::move(current));
      }
      return cells;
    }

    std::optional<std::vector<Align>> delimiterRow(const Line& line) {
      if (!line.text.contains('|')) {
        return std::nullopt;
      }
      std::vector<Align> align;
      for (const Source& cell : splitRow(line)) {
        const std::string_view text = cell.text;
        const bool left = text.starts_with(':');
        const bool right = text.ends_with(':');
        std::string_view core = text.substr(left ? 1 : 0);
        if (core.ends_with(':')) {
          core.remove_suffix(1);
        }
        if (core.empty() || !std::ranges::all_of(core, [](char c) { return c == '-'; })) {
          return std::nullopt;
        }
        align.push_back(left && right ? Align::Center : right ? Align::Right : left ? Align::Left : Align::None);
      }
      return align.empty() ? std::nullopt : std::optional(std::move(align));
    }

    std::optional<Parsed> tableAt(Context& context, std::span<const Line> lines, std::size_t i) {
      if (i + 1 >= lines.size() || !(lines[i].text.contains('|') || lines[i + 1].text.contains('|'))) {
        return std::nullopt;
      }
      std::optional<std::vector<Align>> align = delimiterRow(lines[i + 1]);
      if (!align) {
        return std::nullopt;
      }
      const std::vector<Source> head = splitRow(lines[i]);
      const std::size_t columns = align->size();
      if (head.size() != columns) {
        return std::nullopt;
      }
      std::vector<std::vector<Source>> rows;
      std::size_t j = i + 2;
      for (; j < lines.size() && !isBlank(lines[j].text) && lines[j].text.contains('|'); ++j) {
        if (rows.size() < kMaxRows) {
          rows.push_back(splitRow(lines[j]));
        } else {
          context.document.truncated = true;
        }
      }
      std::vector<Range> cells;
      cells.reserve(columns * (rows.size() + 1));
      for (const Source& cell : head) {
        cells.push_back(parseInline(context, cell, 0));
      }
      for (const std::vector<Source>& row : rows) {
        for (std::size_t k = 0; k < columns; ++k) {
          cells.push_back(parseInline(context, k < row.size() ? row[k] : Source{}, 0));
        }
      }
      return Parsed{
          .end = j,
          .block = {
              .kind = BlockKind::Table,
              .columns = columns,
              .rows = rows.size(),
              .alignments = appendAll(context.document.alignments, std::move(*align)),
              .cells = appendAll(context.document.cells, std::move(cells))
          }
      };
    }

    // The paragraph, or setext heading, starting at lines[i].
    Parsed paragraphAt(Context& context, std::span<const Line> lines, std::size_t i) {
      std::size_t j = i + 1;
      for (; j < lines.size() && !isBlank(lines[j].text); ++j) {
        const bool equals = allOf(lines[j].text, '=');
        if (equals || allOf(lines[j].text, '-')) {
          return {
              .end = j + 1,
              .block = {
                  .kind = BlockKind::Heading,
                  .level = static_cast<std::uint8_t>(equals ? 1 : 2),
                  .runs = paragraphRuns(context, lines.subspan(i, j - i))
              }
          };
        }
        if (interrupts(lines[j])) {
          break;
        }
      }
      return {
          .end = j, .block = {.kind = BlockKind::Paragraph, .runs = paragraphRuns(context, lines.subspan(i, j - i))}
      };
    }

    Parsed blockAt(Context& context, std::span<const Line> lines, std::size_t i, std::size_t depth) {
      const Line& line = lines[i];
      if (const std::optional<Fence> fence = fenceOf(line)) {
        return collectFence(context, lines, i, *fence);
      }
      if (indentOf(line.text) >= kCodeIndent) {
        return collectIndented(context, lines, i);
      }
      if (const std::optional<Atx> atx = atxOf(line)) {
        return {
            .end = i + 1,
            .block = {
                .kind = BlockKind::Heading, .level = atx->level, .runs = parseInline(context, sourceOf(atx->text), 0)
            }
        };
      }
      if (isRule(line.text)) {
        return {.end = i + 1, .block = {.kind = BlockKind::Rule}};
      }
      if (startsQuote(line.text)) {
        return collectQuote(context, lines, i, depth);
      }
      if (markerOf(line.text)) {
        return listAt(context, lines, i, depth);
      }
      if (std::optional<Parsed> table = tableAt(context, lines, i)) {
        return *table;
      }
      return paragraphAt(context, lines, i);
    }

    // Past kMaxDepth the remaining lines become one paragraph of plain text.
    Range flattened(Context& context, std::span<const Line> lines) {
      context.document.truncated = true;
      std::vector<Line> rest;
      for (const Line& line : lines) {
        if (!isBlank(line.text)) {
          rest.push_back(line);
        }
      }
      if (rest.empty()) {
        return {.first = context.document.blocks.size(), .count = 0};
      }
      const Source text = paragraphText(rest);
      std::vector<Pending> plain;
      plain.push_back(Pending{.text = text.piece(0, text.text.size())});
      std::vector<Block> out;
      out.push_back({.kind = BlockKind::Paragraph, .runs = commit(context, std::move(plain))});
      return appendAll(context.document.blocks, std::move(out));
    }

  } // namespace

  Range parseBlocks(Context& context, std::span<const Line> lines, std::size_t depth) {
    if (depth >= kMaxDepth) {
      return flattened(context, lines);
    }
    std::vector<Block> out;
    for (std::size_t i = 0; i < lines.size();) { // every block moves i forward
      if (isBlank(lines[i].text)) {
        ++i;
        continue;
      }
      if (context.blockCount >= kMaxBlocks) {
        context.document.truncated = true;
        break;
      }
      ++context.blockCount;
      Parsed parsed = blockAt(context, lines, i, depth);
      out.push_back(parsed.block);
      i = std::max(parsed.end, i + 1);
    }
    return appendAll(context.document.blocks, std::move(out));
  }
} // namespace luciole::formats::markdown
