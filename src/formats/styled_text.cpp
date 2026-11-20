#include "formats/styled_text.h"

#include "formats/code_tokens.h"
#include "formats/json_text.h"
#include "formats/language_keywords.h"
#include "formats/line_tokens.h"
#include "formats/styled_markdown.h"
#include "formats/text_lines.h"
#include "formats/text_tokens.h"
#include "formats/utf8.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace luciole::formats {
  namespace {
    constexpr std::uint32_t kReplacement = 0xFFFD;
    constexpr std::uint32_t kControlPictures = 0x2400;
    constexpr std::uint32_t kDeletePicture = 0x2421;
    constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
    // Cleaning can triple the text; token offsets are 32-bit.
    constexpr std::size_t kMaxReadBytes = std::numeric_limits<std::uint32_t>::max() / 3;

    bool isContinuation(unsigned char byte) { return (byte & 0xC0U) == 0x80U; }

    // Bytes of the well-formed UTF-8 sequence at i, or 0.
    std::size_t sequenceAt(std::string_view text, std::size_t i) {
      const auto lead = static_cast<unsigned char>(text[i]);
      if (lead < 0x80U) {
        return 1;
      }
      std::size_t length = 0;
      unsigned char low = 0x80U;
      unsigned char high = 0xBFU;
      if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2;
      } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3;
        low = lead == 0xE0U ? 0xA0U : low;
        high = lead == 0xEDU ? 0x9FU : high;
      } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4;
        low = lead == 0xF0U ? 0x90U : low;
        high = lead == 0xF4U ? 0x8FU : high;
      } else {
        return 0;
      }
      if (i + length > text.size()) {
        return 0;
      }
      const auto second = static_cast<unsigned char>(text[i + 1]);
      if (second < low || second > high) {
        return 0;
      }
      for (std::size_t k = 2; k < length; ++k) {
        if (!isContinuation(static_cast<unsigned char>(text[i + k]))) {
          return 0;
        }
      }
      return length;
    }

    // Valid UTF-8 without ASCII controls other than tab and line breaks.
    std::string cleanText(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      std::size_t i = 0;
      // Every pass moves i forward by at least one byte.
      while (i < text.size()) {
        const auto length = sequenceAt(text, i);
        const auto byte = static_cast<unsigned char>(text[i]);
        if (length == 0) {
          appendUtf8(out, kReplacement);
          ++i;
          continue;
        }
        if (byte == 0x7FU) {
          appendUtf8(out, kDeletePicture);
        } else if (byte < 0x20U && byte != '\t' && byte != '\n' && byte != '\r') {
          appendUtf8(out, kControlPictures + byte);
        } else {
          out.append(text.substr(i, length));
        }
        i += length;
      }
      return out;
    }

    // The longest prefix of at most `limit` bytes that does not end inside a UTF-8 sequence.
    std::string_view cutText(std::string_view text, std::size_t limit) {
      if (limit >= text.size()) {
        return text;
      }
      auto end = limit;
      for (std::size_t step = 0; step < 3 && end > 0 && isContinuation(static_cast<unsigned char>(text[end])); ++step) {
        --end;
      }
      return text.substr(0, end);
    }

    std::vector<StyledRun> runsOf(std::string_view text, const TextLine& tokens) {
      std::vector<StyledRun> runs;
      for (const auto& token : tokens) {
        styled::appendRun(runs, tokenText(text, token), tokenRole(token.tokenClass), false, false);
      }
      return runs;
    }

    std::size_t codePointCount(std::string_view text) {
      return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
        return !isContinuation(static_cast<unsigned char>(c));
      }));
    }

    // Keeps the first `columns` code points of the runs; true when it cut.
    bool cutRuns(std::vector<StyledRun>& runs, std::size_t columns) {
      auto room = columns;
      for (std::size_t k = 0; k < runs.size(); ++k) {
        const auto width = codePointCount(runs[k].text);
        if (width <= room) {
          room -= width;
          continue;
        }
        runs[k].text.resize(tokenize::prefixBytes(runs[k].text, room));
        runs.resize(room > 0 ? k + 1 : k);
        return true;
      }
      return false;
    }

    // The code point at byte i of well-formed text.
    std::uint32_t codePointAt(std::string_view text, std::size_t i) {
      const auto length = sequenceAt(text, i);
      const auto lead = static_cast<unsigned char>(text[i]);
      if (length < 2) {
        return lead;
      }
      std::uint32_t code = lead & (0xFFU >> (length + 1));
      for (std::size_t k = 1; k < length; ++k) {
        code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3FU);
      }
      return code;
    }

    constexpr std::uint32_t kJoiner = 0x200D;

    // Whether the code point stays with the one before it: a combining mark, a variation selector, a skin tone.
    bool extendsCluster(std::uint32_t code) {
      return (code >= 0x300 && code <= 0x36F)
          || (code >= 0x20D0 && code <= 0x20FF)
          || (code >= 0xFE00 && code <= 0xFE0F)
          || (code >= 0x1F3FB && code <= 0x1F3FF)
          || (code >= 0xE0100 && code <= 0xE01EF)
          || code == kJoiner;
    }

    constexpr std::size_t kMaxClusterTail = 32; // code points a cut moves on to keep one cluster whole

    // The code point that ends just before byte `at` (at > 0).
    std::uint32_t codePointBefore(std::string_view text, std::size_t at) {
      std::size_t start = at - 1;
      // At most three continuation bytes precede the lead byte.
      for (std::size_t step = 0; step < 3 && start > 0 && isContinuation(static_cast<unsigned char>(text[start]));
           ++step) {
        --start;
      }
      return codePointAt(text, start);
    }

    // Moves a cut at byte `at` on past the marks and joined characters that belong with the code point before it.
    std::size_t keepCluster(std::string_view text, std::size_t at) {
      // Each pass moves one code point on, at most kMaxClusterTail times.
      for (std::size_t moved = 0; moved < kMaxClusterTail && at > 0 && at < text.size(); ++moved) {
        const auto length = sequenceAt(text, at);
        const bool joins =
            length != 0 && (extendsCluster(codePointAt(text, at)) || codePointBefore(text, at) == kJoiner);
        if (!joins) {
          break;
        }
        at += length;
      }
      return at;
    }

    // Byte offset at which the segment starting at `begin` of `text` ends: after its last space or tab within `limit`
    // code points, or at `limit` code points (moved on to the end of a cluster) when it has none, or at the end of
    // the text.
    std::size_t segmentEnd(std::string_view text, std::size_t begin, std::size_t limit) {
      std::size_t points = 0;
      std::size_t lastBreak = 0;
      std::size_t i = begin;
      // Each pass moves i forward by one byte.
      while (i < text.size()) {
        if (!isContinuation(static_cast<unsigned char>(text[i]))) {
          if (points == limit) {
            return lastBreak > begin ? lastBreak : keepCluster(text, i);
          }
          ++points;
        }
        ++i;
        if (text[i - 1] == ' ' || text[i - 1] == '\t') {
          lastBreak = i;
        }
      }
      return text.size();
    }

    // The part of `runs` between byte offsets [begin, end) of their joined text, in the same styles.
    std::vector<StyledRun> sliceRuns(const std::vector<StyledRun>& runs, std::size_t begin, std::size_t end) {
      std::vector<StyledRun> out;
      std::size_t at = 0;
      for (const auto& run : runs) {
        const auto from = std::max(begin, at);
        const auto to = std::min(end, at + run.text.size());
        if (from < to) {
          styled::appendRun(
              out, std::string_view{run.text}.substr(from - at, to - from), run.role, run.italic, run.bold
          );
        }
        at += run.text.size();
      }
      return out;
    }

    bool addWhole(styled::Builder& builder, StyledBlock& block, std::vector<StyledRun>&& runs, const FileKind& kind) {
      if (builder.linesLeft == 0) {
        builder.document.linesCapped = true;
        return false;
      }
      --builder.linesLeft;
      StyledLine line;
      line.runs = std::move(runs);
      const auto whole = joinedText(line);
      const bool names = kind.language == Language::Css || kind.language == Language::Scss;
      const auto matches = builder.colorsLeft > 0 ? findLineColors(whole, kind, names) : std::vector<ColorMatch>{};
      // Prose wraps; the byte limit and the Markdown parser bound its lines.
      const auto columns = block.kind == BlockKind::Prose ? std::numeric_limits<std::size_t>::max() : builder.columns;
      line.continues = cutRuns(line.runs, columns);
      builder.document.widthCapped = builder.document.widthCapped || line.continues;
      for (const auto& match : matches) {
        const auto column = codePointCount(std::string_view{whole}.substr(0, match.offset));
        const auto end = column + codePointCount(std::string_view{whole}.substr(match.offset, match.length));
        if (builder.colorsLeft == 0 || end > columns) {
          break;
        }
        line.swatches.push_back({.color = match.color, .column = static_cast<std::uint32_t>(column)});
        --builder.colorsLeft;
      }
      block.lines.push_back(std::move(line));
      return true;
    }

    // Adds a line; a prose line past kStyledProseSegment code points is added as several. False at the line limit.
    bool
    addSegments(styled::Builder& builder, StyledBlock& block, std::vector<StyledRun>&& runs, const FileKind& kind) {
      if (block.kind != BlockKind::Prose) {
        return addWhole(builder, block, std::move(runs), kind);
      }
      const auto whole = std::ranges::fold_left(runs, std::string{}, [](std::string text, const StyledRun& run) {
        return text += run.text;
      });
      if (codePointCount(whole) <= kStyledProseSegment) {
        return addWhole(builder, block, std::move(runs), kind);
      }
      std::size_t begin = 0;
      // Each pass adds one segment and moves begin forward by at least one byte.
      while (begin < whole.size()) {
        const auto end = segmentEnd(whole, begin, kStyledProseSegment);
        if (!addWhole(builder, block, sliceRuns(runs, begin, end), kind)) {
          return false;
        }
        begin = end;
      }
      return true;
    }

    void addJson(styled::Builder& builder, std::string_view text, const FileKind& kind) {
      StyledBlock block;
      block.kind = BlockKind::Code;
      // Text cut at the byte limit only has to be the start of a document.
      const bool cut = builder.document.bytesCapped;
      if (auto problem = jsonProblem(text, cut ? JsonExtent::Start : JsonExtent::Whole)) {
        // Cut text that is not one value's start has its lines colored as they stand, with no problem reported.
        builder.document.problem = cut ? std::nullopt : std::move(problem);
        styled::addCodeLines(builder, block, text, kind, cut ? TextSyntax::JsonLines : TextSyntax::Plain);
      } else {
        const auto printed = printJson(text);
        builder.document.linesCapped = builder.document.linesCapped || printed.capped;
        bool added = true;
        for (const auto& tokens : printed.lines) {
          added = styled::addLine(builder, block, runsOf(text, tokens), kind);
          if (!added) {
            break;
          }
        }
        // The last line of a document cut inside a value continues past the cut.
        if (added && printed.endsEarly && !block.lines.empty()) {
          block.lines.back().continues = true;
        }
      }
      if (!block.lines.empty()) {
        builder.document.blocks.push_back(std::move(block));
      }
    }
  } // namespace

  namespace styled {
    void appendRun(std::vector<StyledRun>& runs, std::string_view text, StyleRole role, bool italic, bool bold) {
      if (text.empty()) {
        return;
      }
      if (!runs.empty() && runs.back().role == role && runs.back().italic == italic && runs.back().bold == bold) {
        runs.back().text += text;
        return;
      }
      runs.push_back({.text = std::string{text}, .role = role, .italic = italic, .bold = bold});
    }

    bool addLine(Builder& builder, StyledBlock& block, std::vector<StyledRun>&& runs, const FileKind& kind) {
      return addSegments(builder, block, std::move(runs), kind);
    }

    void
    addCodeLines(Builder& builder, StyledBlock& block, std::string_view text, const FileKind& kind, TextSyntax syntax) {
      const auto options = codeOptions(kind);
      syntax = syntax == TextSyntax::Json ? TextSyntax::JsonLines : syntax;
      tokenize::ScanState state;
      std::size_t pos = 0;
      // Each pass adds one line and moves pos past its line break, or leaves.
      while (true) {
        const auto lineBreak = text.find_first_of("\r\n", pos);
        const auto end = lineBreak == std::string_view::npos ? text.size() : lineBreak;
        TextLine tokens;
        tokenize::tokenizeLine({.s = text.substr(pos, end - pos), .base = pos, .out = &tokens}, syntax, options, state);
        if (!addSegments(builder, block, runsOf(text, tokens), kind) || lineBreak == std::string_view::npos) {
          return;
        }
        const bool crlf = text[lineBreak] == '\r' && lineBreak + 1 < text.size() && text[lineBreak + 1] == '\n';
        pos = lineBreak + (crlf ? 2 : 1);
        if (pos == text.size()) {
          return;
        }
      }
    }
  } // namespace styled

  StyledDocument styledText(std::string_view text, const FileKind& kind, const StyledTextLimits& limits) {
    StyledDocument document;
    const auto read = cutText(text, std::min(limits.bytes, kMaxReadBytes));
    document.bytesCapped = read.size() < text.size();
    const auto clean = cleanText(read.starts_with(kByteOrderMark) ? read.substr(kByteOrderMark.size()) : read);
    styled::Builder builder{.document = document, .columns = limits.columns, .linesLeft = limits.lines};
    const auto syntax = textSyntax(kind.language);
    if (syntax == TextSyntax::Markdown) {
      styled::addMarkdown(builder, clean);
    } else if (syntax == TextSyntax::Json) {
      addJson(builder, clean, kind);
    } else {
      StyledBlock block;
      block.kind = kind.prose ? BlockKind::Prose : BlockKind::Code;
      styled::addCodeLines(builder, block, clean, kind, syntax);
      if (!block.lines.empty()) {
        document.blocks.push_back(std::move(block));
      }
    }
    return document;
  }

  std::string joinedText(const StyledLine& line) {
    std::string out;
    for (const auto& run : line.runs) {
      out += run.text;
    }
    return out;
  }

  tokenize::CodeOptions codeOptions(const FileKind& kind) {
    const bool otherStyle =
        kind.comment == CommentStyle::Hash || kind.comment == CommentStyle::Dash || kind.comment == CommentStyle::Semi;
    return {
        .style = otherStyle ? kind.comment : CommentStyle::Slash,
        .family = tokenize::Family::Code,
        .triple = true,
        .blocks = !otherStyle,
        .keywords = keywords(kind.language),
    };
  }

  StyleRole tokenRole(TokenClass tokenClass) {
    switch (tokenClass) {
    case TokenClass::Key:
      return StyleRole::Key;
    case TokenClass::String:
      return StyleRole::String;
    case TokenClass::Number:
      return StyleRole::Number;
    case TokenClass::Literal:
      return StyleRole::Literal;
    case TokenClass::Comment:
      return StyleRole::Comment;
    case TokenClass::Punct:
      return StyleRole::Punctuation;
    case TokenClass::Tag:
      return StyleRole::Tag;
    case TokenClass::Attr:
      return StyleRole::Attribute;
    case TokenClass::Keyword:
      return StyleRole::Keyword;
    case TokenClass::Added:
      return StyleRole::Addition;
    case TokenClass::Removed:
      return StyleRole::Removal;
    case TokenClass::Hunk:
      return StyleRole::Hunk;
    case TokenClass::Header:
      return StyleRole::DiffHeader;
    case TokenClass::Plain:
    case TokenClass::Space:
    case TokenClass::More:
      return StyleRole::Plain;
    }
    return StyleRole::Plain;
  }
} // namespace luciole::formats
