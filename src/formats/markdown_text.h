#pragma once

#include "formats/markdown.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// What the Markdown parser works on: lines and inline text that remember their input offsets, and the document
// they are stored in.
namespace luciole::formats::markdown {
  inline constexpr std::size_t kNoOrigin = std::numeric_limits<std::size_t>::max();

  // '\0' past the end, so a comparison with any other character fails there.
  char charAt(std::string_view text, std::size_t index);

  // Text without Unicode white space at either end.
  std::string_view trimmed(std::string_view text);

  // Whether text holds nothing but Unicode white space.
  bool isBlank(std::string_view text);

  // Spaces, not tabs, at the start of text.
  std::size_t indentOf(std::string_view text);

  // Bytes of the UTF-8 sequence starting at `pos`; a byte that does not start a whole sequence stands alone.
  // A position at or past the end gives 0.
  std::size_t sequenceLength(std::string_view text, std::size_t pos);

  // UTF-16 units of a sequence of `length` bytes: two for a four-byte sequence, one otherwise.
  std::size_t unitsOf(std::size_t length);

  // UTF-16 units of text.
  std::size_t unitCount(std::string_view text);

  // Bytes of the longest prefix of at most `limit` units that does not end inside a UTF-8 sequence.
  std::size_t cutPoint(std::string_view text, std::size_t limit);

  // A line of the input, or a part of one. Bytes from `mappedFrom` on are the input's, starting at `origin`.
  struct Line {
    std::string_view text;
    std::size_t mappedFrom = 0;
    std::size_t origin = kNoOrigin;
  };

  // The input offset of byte `index` of a line, or kNoOrigin.
  std::size_t originAt(const Line& line, std::size_t index);

  // The line from byte `from` on, keeping the input offsets of its bytes.
  Line sliceLine(const Line& line, std::size_t from);

  // The line without white space at its start, and without it at both ends.
  Line trimStart(const Line& line);
  Line trimLine(const Line& line);

  // Text with the input offset it may start at; `store` checks the claim before using it.
  struct Piece {
    std::string text;
    std::size_t origin = kNoOrigin;

    // The claim survives only while each part starts where the text before it ends in the input.
    void append(std::string_view bytes, std::size_t at) {
      if (text.empty()) {
        origin = at;
      } else if (origin != kNoOrigin && at != origin + text.size()) {
        origin = kNoOrigin;
      }
      text += bytes;
    }

    void append(const Piece& other) { append(other.text, other.origin); }
  };

  // Inline input with the input offset of each byte, or kNoOrigin for a byte the parser made.
  struct Source {
    std::string text;
    std::vector<std::size_t> origin;

    // Bytes [begin, end), claiming the first byte's offset only when every byte follows the one before it.
    Piece piece(std::size_t begin, std::size_t end) const {
      Piece out{.text = text.substr(begin, end - begin), .origin = begin < end ? origin[begin] : kNoOrigin};
      for (std::size_t k = begin + 1; k < end && out.origin != kNoOrigin; ++k) {
        if (origin[k] != origin[k - 1] + 1) {
          out.origin = kNoOrigin;
        }
      }
      return out;
    }

    void append(char c, std::size_t at) {
      text += c;
      origin.push_back(at);
    }

    void append(const Line& line) {
      for (std::size_t k = 0; k < line.text.size(); ++k) {
        append(line.text[k], originAt(line, k));
      }
    }
  };

  // The source without white space at either end, offsets kept.
  void trimSource(Source& source);

  struct Context {
    Document& document;
    std::deque<std::string> expanded; // lines whose leading tabs became spaces; a deque keeps views valid
    std::size_t blockCount = 0;
    std::int64_t work = static_cast<std::int64_t>(kWork);
  };

  // Where `piece` sits in Document::text: the input bytes it claims when they match, else appended text.
  Span store(Context& context, const Piece& piece);

  template <typename T> Range appendAll(std::vector<T>& target, std::vector<T>&& values) {
    const Range range{.first = target.size(), .count = values.size()};
    target.insert(target.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
    return range;
  }

  // A run before its text is stored.
  struct Pending {
    RunKind kind = RunKind::Text;
    Piece text{};
    Piece alt{};
    Range children{};
  };

  // Stores the runs and their texts; the range they take in Document::runs.
  Range commit(Context& context, std::vector<Pending>&& nodes);
} // namespace luciole::formats::markdown
