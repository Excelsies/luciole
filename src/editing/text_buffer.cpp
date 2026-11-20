#include "editing/text_buffer.h"

#include "editing/text_motion.h"
#include "formats/ascii.h"
#include "formats/text_facts.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace {
  using luciole::editing::kEditByteLimit;
  using luciole::editing::kEditLineBytes;

  bool isContinuation(char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U; }

  // CRLF and lone CR made LF.
  std::string withLf(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '\r') {
        out += text[i];
        continue;
      }
      out += '\n';
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
    }
    return out;
  }

  // The reason `text` cannot be editor text, ignoring the line limit.
  std::optional<std::string> textRefusal(std::string_view text) {
    if (text.contains('\r')) {
      return "The text contains a carriage return.";
    }
    if (text.contains('\0')) {
      return "The text contains a NUL byte.";
    }
    if (!luciole::formats::textFacts(text).validUtf8) {
      return "The text is not valid UTF-8.";
    }
    return std::nullopt;
  }

  std::string lineTooLong(std::size_t line) { return std::format("Line {} is longer than 16 KiB.", line + 1); }

  constexpr std::string_view kNoSpaces = "The indent unit needs at least one space.";

  std::string indentText(luciole::editing::IndentUnit unit) { return unit.tab ? "\t" : std::string(unit.spaces, ' '); }

  // Bytes of one unit at the start of a line: a tab, or up to `spaces` spaces.
  std::size_t unitAtStart(std::string_view line, luciole::editing::IndentUnit unit) {
    if (unit.tab) {
      return line.starts_with('\t') ? 1 : 0;
    }
    return std::min({line.find_first_not_of(' '), line.size(), unit.spaces});
  }

  // Bytes of one unit at the end of `text`.
  std::size_t unitBefore(std::string_view text, luciole::editing::IndentUnit unit) {
    if (unit.tab) {
      return text.ends_with('\t') ? 1 : 0;
    }
    const auto lastOther = text.find_last_not_of(' ');
    const auto spaces = lastOther == std::string_view::npos ? text.size() : text.size() - lastOther - 1;
    return std::min(spaces, unit.spaces);
  }
} // namespace

std::expected<luciole::editing::TextBuffer, std::string> luciole::editing::TextBuffer::create(std::string_view text) {
  if (text.size() > kEditByteLimit) {
    return std::unexpected("The text is larger than 1 MiB.");
  }
  if (auto reason = textRefusal(text)) {
    return std::unexpected(std::move(*reason));
  }
  TextBuffer buffer;
  buffer.m_lines.clear();
  buffer.m_bytes = text.size();
  std::size_t from = 0;
  // Bounded by the number of LF bytes plus one.
  while (true) {
    const auto lf = text.find('\n', from);
    const auto line = text.substr(from, lf == std::string_view::npos ? std::string_view::npos : lf - from);
    if (line.size() > kEditLineBytes) {
      return std::unexpected(lineTooLong(buffer.m_lines.size()));
    }
    buffer.m_lines.emplace_back(line);
    if (lf == std::string_view::npos) {
      break;
    }
    from = lf + 1;
  }
  return buffer;
}

std::string luciole::editing::TextBuffer::text() const {
  std::string text;
  text.reserve(m_bytes);
  for (std::size_t i = 0; i < m_lines.size(); ++i) {
    if (i > 0) {
      text += '\n';
    }
    text += m_lines[i];
  }
  return text;
}

luciole::editing::TextPosition luciole::editing::TextBuffer::clamp(TextPosition position) const {
  if (position.line >= m_lines.size()) {
    return {m_lines.size() - 1, m_lines.back().size()};
  }
  const std::string& line = m_lines[position.line];
  auto byte = std::min(position.byte, line.size());
  // A code point has at most three continuation bytes.
  while (byte > 0 && byte < line.size() && isContinuation(line[byte])) {
    --byte;
  }
  return {position.line, byte};
}

void luciole::editing::TextBuffer::setSelection(TextSelection selection) {
  const TextSelection clamped{clamp(selection.anchor), clamp(selection.cursor)};
  if (clamped != m_selection) {
    m_history.closeGroup();
  }
  m_selection = clamped;
}

std::string luciole::editing::TextBuffer::textBetween(TextPosition first, TextPosition second) const {
  const auto start = std::min(clamp(first), clamp(second));
  const auto end = std::max(clamp(first), clamp(second));
  if (start.line == end.line) {
    return m_lines[start.line].substr(start.byte, end.byte - start.byte);
  }
  std::string text = m_lines[start.line].substr(start.byte);
  for (auto line = start.line + 1; line < end.line; ++line) {
    text += '\n';
    text += m_lines[line];
  }
  text += '\n';
  text += std::string_view(m_lines[end.line]).substr(0, end.byte);
  return text;
}

std::optional<std::string>
luciole::editing::TextBuffer::refusal(TextPosition start, TextPosition end, std::string_view text) const {
  std::size_t removed = end.byte - start.byte;
  for (auto line = start.line; line < end.line; ++line) {
    removed += m_lines[line].size() + 1;
  }
  if (m_bytes - removed + text.size() > kEditByteLimit) {
    return "The edit would make the text larger than 1 MiB.";
  }
  const auto tail = m_lines[end.line].size() - end.byte;
  std::size_t from = 0;
  // One pass per inserted line.
  for (auto line = start.line;; ++line) {
    const auto lf = text.find('\n', from);
    const auto last = lf == std::string_view::npos;
    auto width = (last ? text.size() : lf) - from;
    width += (line == start.line ? start.byte : 0) + (last ? tail : 0);
    if (width > kEditLineBytes) {
      return std::format("The edit would make line {} longer than 16 KiB.", line + 1);
    }
    if (last) {
      return std::nullopt;
    }
    from = lf + 1;
  }
}

luciole::editing::LineSpan
luciole::editing::TextBuffer::apply(TextPosition start, TextPosition end, std::string_view text) {
  std::vector<std::string> fresh{m_lines[start.line].substr(0, start.byte)};
  std::size_t from = 0;
  // One pass per LF in text.
  for (auto lf = text.find('\n'); lf != std::string_view::npos; lf = text.find('\n', from)) {
    fresh.back() += text.substr(from, lf - from);
    fresh.emplace_back();
    from = lf + 1;
  }
  fresh.back() += text.substr(from);
  fresh.back() += std::string_view(m_lines[end.line]).substr(end.byte);

  const auto first = static_cast<std::ptrdiff_t>(start.line);
  const auto removedLines = end.line - start.line + 1;
  std::size_t removedBytes = removedLines - 1;
  for (auto line = start.line; line <= end.line; ++line) {
    removedBytes += m_lines[line].size();
  }
  std::size_t insertedBytes = fresh.size() - 1;
  for (const auto& line : fresh) {
    insertedBytes += line.size();
  }
  m_bytes = m_bytes - removedBytes + insertedBytes;

  const auto kept = std::min(removedLines, fresh.size());
  std::ranges::move(fresh.begin(), fresh.begin() + static_cast<std::ptrdiff_t>(kept), m_lines.begin() + first);
  const auto after = m_lines.begin() + first + static_cast<std::ptrdiff_t>(kept);
  if (fresh.size() > removedLines) {
    m_lines.insert(
        after, std::make_move_iterator(fresh.begin() + static_cast<std::ptrdiff_t>(kept)),
        std::make_move_iterator(fresh.end())
    );
  } else {
    m_lines.erase(after, after + static_cast<std::ptrdiff_t>(removedLines - kept));
  }
  return {start.line, removedLines, fresh.size()};
}

luciole::editing::EditResult luciole::editing::TextBuffer::edit(
    TextSelection range, std::string_view text, std::optional<TextSelection> after, StepKind kind, std::uint64_t nowMs
) {
  const auto start = range.start();
  const auto end = range.end();
  const auto cursor = endOfText(start, text);
  const auto next = after.value_or(TextSelection{cursor, cursor});
  auto removed = textBetween(start, end);
  if (removed == text) {
    setSelection(next);
    return std::optional<LineSpan>();
  }
  if (auto reason = refusal(start, end, text)) {
    return std::unexpected(std::move(*reason));
  }
  const auto before = m_selection;
  const auto span = apply(start, end, text);
  m_selection = next;
  m_history.record({start, std::move(removed), std::string(text), before, next}, kind, nowMs);
  return span;
}

luciole::editing::EditResult luciole::editing::TextBuffer::insertText(std::string_view text, std::uint64_t nowMs) {
  if (text.contains('\0')) {
    return std::unexpected("The inserted text contains a NUL byte.");
  }
  if (!formats::textFacts(text).validUtf8) {
    return std::unexpected("The inserted text is not valid UTF-8.");
  }
  const auto clean = withLf(text);
  const bool typing =
      m_selection.empty() && !clean.empty() && !clean.contains('\n') && clusterEnd(clean, 0) == clean.size();
  return edit(m_selection, clean, std::nullopt, typing ? StepKind::Typing : StepKind::Other, nowMs);
}

luciole::editing::EditResult luciole::editing::TextBuffer::newline(std::uint64_t nowMs) {
  const auto start = m_selection.start();
  const std::string_view line = m_lines[start.line];
  const auto blanks = std::min(formats::skipBlanks(line, 0), start.byte);
  return edit(m_selection, "\n" + std::string(line.substr(0, blanks)), std::nullopt, StepKind::Other, nowMs);
}

luciole::editing::EditResult
luciole::editing::TextBuffer::erase(TextPosition target, StepKind kind, std::uint64_t nowMs) {
  return edit({target, m_selection.cursor}, "", std::nullopt, kind, nowMs);
}

luciole::editing::EditResult luciole::editing::TextBuffer::eraseBackward(EraseUnit unit, std::uint64_t nowMs) {
  if (!m_selection.empty()) {
    return edit(m_selection, "", std::nullopt, StepKind::Other, nowMs);
  }
  const auto cursor = m_selection.cursor;
  auto target = clusterLeft(*this, cursor);
  if (unit == EraseUnit::Word) {
    target = wordLeft(*this, cursor);
  } else if (unit == EraseUnit::LineEdge && cursor.byte > 0) {
    target = {cursor.line, 0};
  }
  return erase(target, StepKind::EraseBackward, nowMs);
}

luciole::editing::EditResult luciole::editing::TextBuffer::eraseForward(EraseUnit unit, std::uint64_t nowMs) {
  if (!m_selection.empty()) {
    return edit(m_selection, "", std::nullopt, StepKind::Other, nowMs);
  }
  const auto cursor = m_selection.cursor;
  const auto size = m_lines[cursor.line].size();
  auto target = clusterRight(*this, cursor);
  if (unit == EraseUnit::Word) {
    target = wordRight(*this, cursor);
  } else if (unit == EraseUnit::LineEdge && cursor.byte < size) {
    target = {cursor.line, size};
  }
  return erase(target, StepKind::EraseForward, nowMs);
}

luciole::editing::EditResult luciole::editing::TextBuffer::indent(IndentUnit unit, std::uint64_t nowMs) {
  if (!unit.tab && unit.spaces == 0) {
    return std::unexpected(std::string(kNoSpaces));
  }
  if (m_selection.start().line != m_selection.end().line) {
    return shiftLines(unit, true, nowMs);
  }
  return edit(m_selection, indentText(unit), std::nullopt, StepKind::Other, nowMs);
}

luciole::editing::EditResult luciole::editing::TextBuffer::outdent(IndentUnit unit, std::uint64_t nowMs) {
  if (!unit.tab && unit.spaces == 0) {
    return std::unexpected(std::string(kNoSpaces));
  }
  if (!m_selection.empty()) {
    return shiftLines(unit, false, nowMs);
  }
  const auto cursor = m_selection.cursor;
  const std::string_view before = std::string_view(m_lines[cursor.line]).substr(0, cursor.byte);
  return erase({cursor.line, cursor.byte - unitBefore(before, unit)}, StepKind::Other, nowMs);
}

luciole::editing::EditResult
luciole::editing::TextBuffer::shiftLines(IndentUnit unit, bool inward, std::uint64_t nowMs) {
  const auto start = m_selection.start();
  const auto end = m_selection.end();
  const auto last = end.line > start.line && end.byte == 0 ? end.line - 1 : end.line;
  const auto prefix = indentText(unit);
  std::vector<std::size_t> widths;
  std::string text;
  for (auto index = start.line; index <= last; ++index) {
    const std::string_view line = m_lines[index];
    if (inward) {
      widths.push_back(line.empty() ? 0 : prefix.size());
      text += line.empty() ? "" : prefix;
      text += line;
    } else {
      widths.push_back(unitAtStart(line, unit));
      text += line.substr(widths.back());
    }
    text += index < last ? "\n" : "";
  }
  const auto shift = [&](TextPosition position) -> TextPosition {
    if (position.line < start.line || position.line > last || position.byte == 0) {
      return position;
    }
    const auto width = widths[position.line - start.line];
    return {position.line, inward ? position.byte + width : position.byte - std::min(position.byte, width)};
  };
  const TextSelection range{{start.line, 0}, {last, m_lines[last].size()}};
  const TextSelection after{shift(m_selection.anchor), shift(m_selection.cursor)};
  return edit(range, text, after, StepKind::Other, nowMs);
}

luciole::editing::IndentUnit luciole::editing::detectIndent(const TextBuffer& buffer) {
  std::size_t tabLines = 0;
  std::size_t spaceLines = 0;
  // Steps of 4, 2 and 8 spaces, in the order ties prefer.
  constexpr std::array<std::size_t, 3> kSteps{4, 2, 8};
  std::array<std::size_t, 3> stepCounts{};
  std::optional<std::size_t> previous;
  for (std::size_t index = 0; index < buffer.lineCount(); ++index) {
    const auto line = buffer.line(index);
    if (formats::skipBlanks(line, 0) == line.size()) {
      continue;
    }
    if (line.front() == '\t') {
      ++tabLines;
      previous.reset();
      continue;
    }
    const auto spaces = line.find_first_not_of(' ');
    spaceLines += spaces > 0 ? 1 : 0;
    if (previous) {
      const auto step = spaces > *previous ? spaces - *previous : *previous - spaces;
      for (std::size_t k = 0; k < kSteps.size(); ++k) {
        stepCounts[k] += step == kSteps[k] ? 1 : 0;
      }
    }
    previous = spaces;
  }
  if (tabLines > spaceLines) {
    return {.tab = true};
  }
  const auto* most = std::ranges::max_element(stepCounts);
  return {.spaces = *most > 0 ? kSteps[static_cast<std::size_t>(most - stepCounts.begin())] : kSteps[0]};
}

bool luciole::editing::TextBuffer::markSaved(std::uint64_t revision) { return m_history.markSaved(revision); }

std::optional<luciole::editing::LineSpan> luciole::editing::TextBuffer::undo() {
  return replay(m_history.undoStep(), false);
}

std::optional<luciole::editing::LineSpan> luciole::editing::TextBuffer::redo() {
  return replay(m_history.redoStep(), true);
}

std::optional<luciole::editing::LineSpan> luciole::editing::TextBuffer::replay(const HistoryStep* step, bool forward) {
  if (step == nullptr) {
    return std::nullopt;
  }
  const auto& current = forward ? step->removed : step->inserted;
  const auto& restored = forward ? step->inserted : step->removed;
  const auto span = apply(step->start, endOfText(step->start, current), restored);
  m_selection = forward ? step->after : step->before;
  return span;
}
