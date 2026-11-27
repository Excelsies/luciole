#pragma once

#include "formats/code_tokens.h"
#include "formats/color_codes.h"
#include "formats/file_kind.h"
#include "formats/text_tokens.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A file's text as blocks of styled lines, ready to draw: what a text preview shows.
namespace luciole::formats {
  inline constexpr std::size_t kStyledTextBytes = 256U * 1024U;
  inline constexpr std::size_t kStyledTextLines = 2000;
  inline constexpr std::size_t kStyledLineColumns = 2000;
  // Code points after which a prose line is split, so that no layout holds an unbounded paragraph: even a run with no
  // space wraps to at most a few hundred rows in a narrow column at the largest text size.
  inline constexpr std::size_t kStyledProseSegment = 1024;

  // What a run is, for its color. Heading, Emphasis and Strong come from Markdown, the others from the tokenizers.
  enum class StyleRole : std::uint8_t {
    Plain,
    Keyword,
    String,
    Number,
    Literal,
    Comment,
    Punctuation,
    Key,
    Tag,
    Attribute,
    Addition,
    Removal,
    Hunk,
    DiffHeader,
    Heading,
    Emphasis,
    Strong,
    InlineCode,
    Link,
    Quote
  };
  inline constexpr std::size_t kStyleRoleCount = 20;
  static_assert(kStyleRoleCount == static_cast<std::size_t>(StyleRole::Quote) + 1);

  struct StyledRun {
    std::string text; // valid UTF-8 without ASCII control characters other than tab
    StyleRole role = StyleRole::Plain;
    bool italic = false;
    bool bold = false;
    bool operator==(const StyledRun&) const = default;
  };

  // A color code's color, drawn before the character at `column` (code points into the line).
  struct ColorSwatch {
    Rgba color;
    std::uint32_t column = 0;
    bool operator==(const ColorSwatch&) const = default;
  };

  struct StyledLine {
    std::vector<StyledRun> runs;
    std::vector<ColorSwatch> swatches; // in column order
    bool continues = false; // cut at the column limit, which prose lines do not have, or ends JSON cut inside a value
  };

  // Code lines keep their spacing and never wrap; prose wraps. A rule has no lines.
  enum class BlockKind : std::uint8_t { Code, Prose, Table, Rule };

  enum class CellAlign : std::uint8_t { Start, Center, End };

  struct StyledBlock {
    BlockKind kind = BlockKind::Prose;
    std::vector<StyledLine> lines; // Table: one per cell, the header row first, then row by row
    std::uint8_t heading = 0;      // 1 to 6 for a heading
    std::uint8_t indent = 0;       // list nesting
    std::uint8_t quoteDepth = 0;
    std::string marker;                // a list item's marker, on the item's first block: "•" or "3."
    bool framed = false;               // a code block drawn on its own background
    bool monospace = false;            // plain text meant to be read: wraps like prose, drawn in the code family
    std::size_t columns = 0;           // Table
    std::vector<CellAlign> alignments; // Table: one per column
  };

  struct StyledDocument {
    std::vector<StyledBlock> blocks;
    std::optional<std::string> problem; // why JSON text is shown raw; none for text cut at the byte limit
    bool bytesCapped = false;           // text past the byte limit was not read
    bool linesCapped = false;           // lines past the line limit were left out
    bool widthCapped = false;           // a line was cut at the column limit
    bool parserCapped = false;          // the Markdown parser left text out at one of its own limits

    bool truncated() const { return bytesCapped || linesCapped || widthCapped || parserCapped; }
  };

  struct StyledTextLimits {
    std::size_t bytes = kStyledTextBytes;
    std::size_t lines = kStyledTextLines;     // lines of all blocks together, table cells included
    std::size_t columns = kStyledLineColumns; // code lines and table cells; prose wraps and is not cut
  };

  // The document of a file's text by its kind: Markdown rendered as blocks, JSON printed with two-space indentation
  // (invalid JSON shown raw with the reason in `problem`; JSON cut at the byte limit printed up to the cut, its last
  // line continuing, or line by line as it stands without a problem when what was read is not the start of one value),
  // plain text meant to be read (FileKind::prose) as one prose block of the file's own lines, and every other kind as
  // one code block colored by its tokenizer, plain for kinds without one. A prose line, Markdown's included, longer
  // than kStyledProseSegment code points becomes several, each cut after a space, inside a word longer than the
  // segment, and never between a character and its combining marks or joined characters (a cut moves on by at most
  // 32 code points). A leading byte order mark is dropped, invalid UTF-8 bytes become U+FFFD and ASCII control
  // characters other than tab and line breaks become their Control Pictures symbols. Swatches follow findLineColors,
  // with color names only in CSS and SCSS, at most kMaxColorsPerFile in the document.
  StyledDocument styledText(std::string_view text, const FileKind& kind, const StyledTextLimits& limits = {});

  // The line's text: its runs joined.
  std::string joinedText(const StyledLine& line);

  // The options styledText gives the code tokenizer for a kind: its comment style and keywords.
  tokenize::CodeOptions codeOptions(const FileKind& kind);

  // The role styledText gives a token class.
  StyleRole tokenRole(TokenClass tokenClass);
} // namespace luciole::formats
