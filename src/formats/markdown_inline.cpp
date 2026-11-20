#include "formats/markdown_inline.h"

#include "formats/ascii.h"
#include "formats/unicode_space.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luciole::formats::markdown {
  namespace {
    constexpr std::size_t kNotFound = std::numeric_limits<std::size_t>::max();
    constexpr std::string_view kPunctuation = R"(!"#$%&'()*+,-./:;<=>?@[\]^_`{|}~)";
    constexpr std::string_view kSpecial = "\\\n`*_![";

    bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n'; }

    // Bytes of multi-byte sequences count as letters, so emphasis inside words of any script is intraword.
    bool isAlnum(char c) {
      return (c >= '0' && c <= '9')
          || (c >= 'a' && c <= 'z')
          || (c >= 'A' && c <= 'Z')
          || static_cast<unsigned char>(c) > 0x7FU;
    }

    bool isPunct(char c) { return kPunctuation.contains(c); }

    // Drops empty text runs and joins neighboring ones.
    std::vector<Pending> normalize(std::vector<Pending>&& nodes) {
      std::vector<Pending> out;
      out.reserve(nodes.size());
      for (Pending& node : nodes) {
        if (node.kind != RunKind::Text) {
          out.push_back(std::move(node));
        } else if (node.text.text.empty()) {
          continue;
        } else if (!out.empty() && out.back().kind == RunKind::Text) {
          out.back().text.append(node.text);
        } else {
          out.push_back(std::move(node));
        }
      }
      return out;
    }

    // One inline text: emphasis is matched against a stack of at most kMaxOpeners openers, each carrying the
    // deepest nesting it would wrap, so the depth check is constant time.
    class InlineParser {
    public:
      InlineParser(Context& context, const Source& source, std::size_t labelDepth)
          : m_context(context), m_source(source), m_text(source.text), m_labelDepth(labelDepth) {}

      Range run();

    private:
      struct Hit {
        Pending node;
        std::size_t end = 0;
      };

      struct Opener {
        char ch = '*';
        std::size_t length = 0;
        std::size_t index = 0;    // the opener's text run in m_nodes
        std::size_t depth = 0;    // deepest nesting the opener would wrap
        std::size_t position = 0; // the opener's first byte in the text
      };

      struct TickRuns {
        std::vector<std::size_t> positions;
        std::size_t next = 0;
      };

      void flush();
      void lineEnd();
      std::size_t emphasis(std::size_t index);
      void findTicks();
      Hit codeSpan(std::size_t index);
      Piece codeText(std::size_t begin, std::size_t end) const;
      std::size_t closer(std::size_t open, char opening, char closing, std::size_t limit);
      std::optional<Hit> targetAt(std::size_t open);
      std::optional<Hit> linkAt(std::size_t index, bool image);

      Context& m_context;
      const Source& m_source;
      std::string_view m_text;
      std::size_t m_labelDepth;
      std::vector<Pending> m_nodes;
      std::vector<Opener> m_stack;
      Piece m_buffer;
      std::unordered_map<std::size_t, TickRuns> m_ticks; // backtick run starts by run length
      bool m_ticksFound = false;
    };

    void InlineParser::flush() {
      if (!m_buffer.text.empty()) {
        m_nodes.push_back(Pending{.text = std::move(m_buffer)});
        m_buffer = {};
      }
    }

    // Two trailing spaces make a hard break; otherwise the line end is one space.
    void InlineParser::lineEnd() {
      const bool hard = m_buffer.text.ends_with("  ");
      m_buffer.text.resize(m_buffer.text.size() - trailingUnicodeSpace(m_buffer.text));
      if (hard) {
        flush();
        m_nodes.push_back(Pending{.kind = RunKind::Break});
      } else {
        m_buffer.append(" ", kNoOrigin);
      }
    }

    // A run of '*' or '_': closes openers on the stack, then may open. Returns the index after the run.
    std::size_t InlineParser::emphasis(std::size_t index) {
      const char ch = m_text[index];
      std::size_t end = index;
      while (end < m_text.size() && m_text[end] == ch) {
        ++end;
      }
      std::size_t length = end - index;
      const bool intraword = ch == '_';
      const bool spaceBefore = index == 0 || isSpace(m_text[index - 1]);
      const bool spaceAfter = end == m_text.size() || isSpace(m_text[end]);
      const bool letterBefore = index > 0 && isAlnum(m_text[index - 1]);
      const bool letterAfter = end < m_text.size() && isAlnum(m_text[end]);
      const bool canOpen = !spaceAfter && (!intraword || !letterBefore);
      const bool canClose = !spaceBefore && (!intraword || !letterAfter);
      std::size_t& steps = m_context.document.emphasisSteps;
      while (canClose && length > 0) { // each pass uses at least one marker
        std::size_t found = m_stack.size();
        ++steps;
        while (found > 0 && m_stack[found - 1].ch != ch) { // at most kMaxOpeners steps
          --found;
          ++steps;
        }
        if (found == 0) {
          break;
        }
        Opener& opener = m_stack[found - 1];
        const std::size_t use = length >= 2 && opener.length >= 2 ? 2 : 1;
        const std::size_t depth = opener.depth;
        if (depth + 1 > kMaxInlineDepth) {
          break;
        }
        steps += m_nodes.size() - opener.index; // these runs move into the wrapper and are not visited again
        const auto innerStart = m_nodes.begin() + static_cast<std::ptrdiff_t>(opener.index + 1);
        std::vector<Pending> inner(std::make_move_iterator(innerStart), std::make_move_iterator(m_nodes.end()));
        m_nodes.erase(innerStart, m_nodes.end());
        const Range children = commit(m_context, normalize(std::move(inner)));
        opener.length -= use;
        length -= use;
        m_nodes[opener.index].text = m_source.piece(opener.position, opener.position + opener.length);
        const std::size_t keep = opener.length > 0 ? found : found - 1;
        m_stack.erase(m_stack.begin() + static_cast<std::ptrdiff_t>(keep), m_stack.end());
        m_nodes.push_back(Pending{.kind = use == 2 ? RunKind::Strong : RunKind::Emphasis, .children = children});
        for (Opener& waiting : m_stack) { // every opener still waiting now holds a wrapper this deep
          waiting.depth = std::max(waiting.depth, depth + 1);
          ++steps;
        }
      }
      if (length > 0) {
        if (canOpen && m_stack.size() < kMaxOpeners) {
          m_stack.push_back(
              {.ch = ch, .length = length, .index = m_nodes.size(), .depth = 0, .position = end - length}
          );
        }
        m_nodes.push_back(Pending{.text = m_source.piece(end - length, end)});
      }
      return end;
    }

    void InlineParser::findTicks() {
      m_ticksFound = true;
      for (std::size_t start = 0; start < m_text.size();) {
        if (m_text[start] != '`') {
          ++start;
          continue;
        }
        std::size_t end = start;
        while (end < m_text.size() && m_text[end] == '`') {
          ++end;
        }
        m_ticks[end - start].positions.push_back(start);
        start = end;
      }
    }

    InlineParser::Hit InlineParser::codeSpan(std::size_t index) {
      if (!m_ticksFound) {
        findTicks();
      }
      std::size_t length = 0;
      while (index + length < m_text.size() && m_text[index + length] == '`') {
        ++length;
      }
      if (const auto found = m_ticks.find(length); found != m_ticks.end()) {
        TickRuns& runs = found->second;
        while (runs.next < runs.positions.size() && runs.positions[runs.next] <= index) {
          ++runs.next;
        }
        if (runs.next < runs.positions.size()) {
          const std::size_t close = runs.positions[runs.next];
          return {
              .node = Pending{.kind = RunKind::Code, .text = codeText(index + length, close)}, .end = close + length
          };
        }
      }
      return {.node = Pending{.text = m_source.piece(index, index + length)}, .end = index + length};
    }

    // Line ends become spaces; one space is stripped from each side when both have one and the text is not blank.
    Piece InlineParser::codeText(std::size_t begin, std::size_t end) const {
      Piece out = m_source.piece(begin, end);
      std::ranges::replace(out.text, '\n', ' ');
      if (out.text.size() > 2 && out.text.front() == ' ' && out.text.back() == ' ' && !isBlank(out.text)) {
        out = m_source.piece(begin + 1, end - 1);
        std::ranges::replace(out.text, '\n', ' ');
      }
      return out;
    }

    // The `closing` character that balances the `opening` one at `open`, within `limit` UTF-16 units after it, or
    // kNotFound. A backslash skips the unit after it, and each unit scanned spends one unit of the work budget.
    std::size_t InlineParser::closer(std::size_t open, char opening, char closing, std::size_t limit) {
      std::size_t depth = 1;
      std::size_t units = 0;
      bool escaped = false;
      for (std::size_t j = open + 1; j < m_text.size() && units < limit;) { // each pass moves j forward
        const std::size_t length = sequenceLength(m_text, j);
        const std::size_t width = unitsOf(length);
        const std::size_t scanned = std::min(width, limit - units) - (escaped ? 1 : 0);
        m_context.work -= static_cast<std::int64_t>(scanned);
        const char c = m_text[j];
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == opening) {
          ++depth;
        } else if (c == closing && --depth == 0) {
          return j;
        }
        j += length;
        units += width;
      }
      return kNotFound;
    }

    // The target in "(target)" opened at `open`, up to its first space; a title after it is dropped.
    std::optional<InlineParser::Hit> InlineParser::targetAt(std::size_t open) {
      const std::size_t close = closer(open, '(', ')', kMaxTarget);
      if (close == kNotFound) {
        return std::nullopt;
      }
      const std::string_view inside = m_text.substr(open + 1, close - open - 1);
      const std::size_t begin = open + 1 + leadingUnicodeSpace(inside);
      const std::string_view raw = trimmed(inside);
      const std::size_t cut = raw.find_first_of(" \t\n");
      return Hit{
          .node = Pending{.text = m_source.piece(begin, begin + (cut == std::string_view::npos ? raw.size() : cut))},
          .end = close + 1
      };
    }

    std::optional<InlineParser::Hit> InlineParser::linkAt(std::size_t index, bool image) {
      if (m_context.work <= 0) {
        return std::nullopt;
      }
      const std::size_t open = image ? index + 1 : index;
      const std::size_t close = closer(open, '[', ']', kMaxLabel);
      if (close == kNotFound || charAt(m_text, close + 1) != '(') {
        return std::nullopt;
      }
      std::optional<Hit> target = targetAt(close + 1);
      if (!target) {
        return std::nullopt;
      }
      Piece href = std::move(target->node.text);
      if (image) {
        return Hit{
            .node = Pending{.kind = RunKind::Image, .text = std::move(href), .alt = m_source.piece(open + 1, close)},
            .end = target->end
        };
      }
      Range children;
      if (m_labelDepth < kMaxLabelDepth) {
        Source label;
        for (std::size_t k = open + 1; k < close; ++k) {
          label.append(m_text[k], m_source.origin[k]);
        }
        children = parseInline(m_context, label, m_labelDepth + 1);
      } else {
        std::vector<Pending> plain;
        plain.push_back(Pending{.text = m_source.piece(open + 1, close)});
        children = commit(m_context, std::move(plain));
      }
      return Hit{
          .node = Pending{.kind = RunKind::Link, .text = std::move(href), .children = children}, .end = target->end
      };
    }

    Range InlineParser::run() {
      const std::size_t size = m_text.size();
      std::size_t i = 0;
      while (i < size) { // every branch moves i forward
        const char c = m_text[i];
        if (c == '\\' && charAt(m_text, i + 1) == '\n') {
          flush();
          m_nodes.push_back(Pending{.kind = RunKind::Break});
          i += 2;
          continue;
        }
        if (c == '\\' && i + 1 < size && isPunct(m_text[i + 1])) {
          m_buffer.append(m_text.substr(i + 1, 1), m_source.origin[i + 1]);
          i += 2;
          continue;
        }
        if (c == '\n') {
          lineEnd();
          ++i;
          continue;
        }
        if (c == '*' || c == '_') {
          flush();
          i = emphasis(i);
          continue;
        }
        std::optional<Hit> hit;
        if (c == '`') {
          flush();
          hit = codeSpan(i);
        } else if (c == '[' || (c == '!' && charAt(m_text, i + 1) == '[')) {
          hit = linkAt(i, c == '!');
          if (hit) {
            flush();
          }
        }
        if (hit) {
          m_nodes.push_back(std::move(hit->node));
          i = hit->end;
          continue;
        }
        std::size_t j = i + 1;
        while (j < size && !kSpecial.contains(m_text[j])) {
          ++j;
        }
        m_buffer.append(m_source.piece(i, j));
        i = j;
      }
      flush();
      return commit(m_context, normalize(std::move(m_nodes)));
    }
  } // namespace

  Range parseInline(Context& context, const Source& source, std::size_t labelDepth) {
    return InlineParser(context, source, labelDepth).run();
  }

  Source sourceOf(const Line& line) {
    Source source;
    source.append(line);
    return source;
  }
} // namespace luciole::formats::markdown
