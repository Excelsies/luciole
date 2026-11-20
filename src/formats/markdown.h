#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Markdown to a flat tree of blocks and inline runs. Input is untrusted: raw HTML stays text, link and image
// targets are data that nothing here loads, and every scan is bounded by the limits below. Lengths count UTF-16
// units: one per character, two for a character above U+FFFF, one for a byte outside a whole UTF-8 sequence. A cut
// never splits a UTF-8 sequence. Spans and offsets count bytes.
namespace luciole::formats::markdown {
  inline constexpr std::size_t kMaxChars = 200000;   // input units read
  inline constexpr std::size_t kMaxLines = 4000;     // lines read
  inline constexpr std::size_t kMaxLine = 2000;      // units kept per line
  inline constexpr std::size_t kMaxDepth = 6;        // block nesting; deeper text becomes one plain paragraph
  inline constexpr std::size_t kMaxBlocks = 3000;    // blocks in a document
  inline constexpr std::size_t kMaxColumns = 50;     // table columns
  inline constexpr std::size_t kMaxRows = 500;       // table body rows, and items in one list
  inline constexpr std::size_t kMaxInline = 50000;   // units of inline text in one paragraph
  inline constexpr std::size_t kMaxOpeners = 64;     // emphasis markers waiting for a closer
  inline constexpr std::size_t kMaxInlineDepth = 16; // nesting of emphasis and strong
  inline constexpr std::size_t kMaxLabel = 300;      // units scanned for a link label
  inline constexpr std::size_t kMaxTarget = 600;     // units scanned for a link target
  inline constexpr std::size_t kMaxLabelDepth = 3;   // link labels parsed inside link labels
  inline constexpr std::size_t kWork = 3000000;      // units scanned for links in one document

  // Bytes of Document::text.
  struct Span {
    std::size_t offset = 0;
    std::size_t length = 0;
    bool operator==(const Span&) const = default;
  };

  // Consecutive elements of one Document array; which one is named where the range is stored.
  struct Range {
    std::size_t first = 0;
    std::size_t count = 0;
    bool operator==(const Range&) const = default;
  };

  enum class RunKind : std::uint8_t { Text, Emphasis, Strong, Code, Break, Link, Image };

  struct Run {
    RunKind kind = RunKind::Text;
    Span text{};      // Text and Code: the text. Link: the target. Image: the source.
    Span alt{};       // Image: the description, as written
    Range children{}; // Emphasis, Strong and Link: runs
  };

  enum class BlockKind : std::uint8_t { Heading, Paragraph, Code, List, Quote, Table, Rule };

  enum class Align : std::uint8_t { None, Left, Center, Right };

  struct Block {
    BlockKind kind = BlockKind::Paragraph;
    std::uint8_t level = 0;  // Heading: 1 to 6
    bool ordered = false;    // List
    std::uint32_t start = 0; // List: number of the first item, 0 for bullets
    Span text{};             // Code: the body
    Span lang{};             // Code: the first word of the info string
    Range runs{};            // Heading and Paragraph: runs
    Range children{};        // Quote: blocks. List: items.
    std::size_t columns = 0; // Table
    std::size_t rows = 0;    // Table: body rows
    Range alignments{};      // Table: one per column
    Range cells{};           // Table: the header row, then each body row, `columns` cells per row
  };

  struct Document {
    std::string text;           // the input as read, then text the parser made
    std::size_t sourceSize = 0; // a span that ends at or before this is the input's bytes at the same offsets
    std::vector<Block> blocks;
    std::vector<Run> runs;
    std::vector<Range> items; // a list item's blocks
    std::vector<Range> cells; // a table cell's runs
    std::vector<Align> alignments;
    Range root;                    // the top-level blocks
    bool truncated = false;        // a limit cut or dropped part of the input
    std::size_t emphasisSteps = 0; // work spent matching emphasis; linear in the input
  };

  Document parse(std::string_view text);

  // Each accessor returns an empty result for a span or range outside the document.
  std::string_view text(const Document& document, Span span);
  std::span<const Block> blocks(const Document& document, Range range);
  std::span<const Run> runs(const Document& document, Range range);
  std::span<const Range> items(const Document& document, Range range);
  std::span<const Range> cells(const Document& document, Range range);
  std::span<const Align> alignments(const Document& document, Range range);
} // namespace luciole::formats::markdown
