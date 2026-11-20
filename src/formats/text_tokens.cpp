#include "formats/text_tokens.h"

#include "formats/code_tokens.h"
#include "formats/json_text.h"
#include "formats/language_keywords.h"
#include "formats/line_tokens.h"
#include "formats/text_lines.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace luciole::formats {
  namespace {
    using tokenize::CodeOptions;
    using tokenize::cutWidth;
    using tokenize::Family;
    using tokenize::LineWriter;
    using tokenize::ScanState;
    using tokenize::splitLines;
    using tokenize::tokenizeLine;

    constexpr std::string_view kEllipsis = "…";
    constexpr std::size_t kMaxSpace = 2 * kMaxJsonDepth;

    FormattedText rawLines(std::string_view text, std::string error) {
      const auto split = splitLines(text);
      FormattedText out;
      out.lines.reserve(split.lines.size());
      for (const auto& span : split.lines) {
        TextLine line;
        LineWriter{.s = text.substr(span.begin, span.end - span.begin), .base = span.begin, .out = &line}.add(
            0, span.end - span.begin, TokenClass::Plain
        );
        out.lines.push_back(std::move(line));
      }
      out.totalLines = split.total;
      out.linesCapped = split.total > split.lines.size();
      out.error = std::move(error);
      return out;
    }

    FormattedText formatJson(std::string_view text) {
      if (auto problem = jsonProblem(text)) {
        return rawLines(text, std::move(*problem));
      }
      auto printed = printJson(text);
      FormattedText out;
      out.lines = std::move(printed.lines);
      out.linesCapped = printed.capped;
      if (!printed.capped) {
        out.totalLines = out.lines.size();
      }
      return out;
    }

    FormattedText formatLines(std::string_view text, TextSyntax syntax, const CodeOptions& code) {
      const auto split = splitLines(text);
      FormattedText out;
      out.totalLines = split.total;
      out.linesCapped = split.total > split.lines.size();
      out.lines.reserve(split.lines.size());
      ScanState state;
      for (const auto& span : split.lines) {
        TextLine line;
        tokenizeLine(
            {.s = text.substr(span.begin, span.end - span.begin), .base = span.begin, .out = &line}, syntax, code, state
        );
        out.lines.push_back(std::move(line));
      }
      return out;
    }
  } // namespace

  FormattedText formatText(std::string_view text, Language language, CommentStyle comment) {
    text = text.substr(0, std::min<std::size_t>(text.size(), std::numeric_limits<std::uint32_t>::max()));
    const auto syntax = textSyntax(language);
    if (syntax == TextSyntax::None) {
      FormattedText out;
      out.totalLines = 0;
      out.error = "unknown language";
      return out;
    }
    const bool otherStyle =
        comment == CommentStyle::Hash || comment == CommentStyle::Dash || comment == CommentStyle::Semi;
    const CodeOptions code{
        .style = otherStyle ? comment : CommentStyle::Slash,
        .family = Family::Code,
        .triple = true,
        .blocks = !otherStyle,
        .keywords = keywords(language),
    };
    auto out = syntax == TextSyntax::Json ? formatJson(text) : formatLines(text, syntax, code);
    for (auto& line : out.lines) {
      out.widthCut += cutWidth(text, line) ? 1 : 0;
    }
    return out;
  }

  std::string tokenText(std::string_view text, const Token& token) {
    if (token.tokenClass == TokenClass::Space) {
      return std::string(std::min<std::size_t>(token.length, kMaxSpace), ' ');
    }
    if (token.tokenClass == TokenClass::More) {
      return std::string{kEllipsis};
    }
    if (token.offset >= text.size()) {
      return {};
    }
    return std::string{text.substr(token.offset, token.length)};
  }

  std::string lineText(std::string_view text, const TextLine& line) {
    std::string out;
    for (const auto& token : line) {
      out += tokenText(text, token);
    }
    return out;
  }
} // namespace luciole::formats
