#include "formats/styled_markdown.h"

#include "formats/markdown.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::formats::styled {
  namespace {
    namespace md = markdown;

    // Above the parser's own nesting: kMaxDepth for blocks, kMaxInlineDepth times kMaxLabelDepth for runs.
    constexpr std::size_t kMaxBlockNesting = 2 * md::kMaxDepth + 2;
    constexpr std::size_t kMaxRunNesting = md::kMaxInlineDepth * (md::kMaxLabelDepth + 1) + 4;
    constexpr std::string_view kBullet = "•";

    struct Nesting {
      std::uint8_t indent = 0;
      std::uint8_t quoteDepth = 0;
      std::size_t depth = 0; // blocks above this one
    };

    struct Face {
      StyleRole base = StyleRole::Plain;
      bool italic = false;
      bool bold = false;
      bool link = false;
    };

    StyleRole roleOf(const Face& face) { return face.link ? StyleRole::Link : face.base; }

    CellAlign alignOf(md::Align align) {
      switch (align) {
      case md::Align::Center:
        return CellAlign::Center;
      case md::Align::Right:
        return CellAlign::End;
      case md::Align::None:
      case md::Align::Left:
        return CellAlign::Start;
      }
      return CellAlign::Start;
    }

    // The kind a fence's language word names, as a file extension would.
    FileKind fenceKind(std::string_view lang) {
      if (lang.empty()) {
        return {};
      }
      return fileKind("code." + std::string{lang});
    }

    class Writer {
    public:
      Writer(Builder& builder, const md::Document& document) : m_builder(builder), m_document(document) {}

      void blocks(std::span<const md::Block> list, const Nesting& nesting, std::string& marker);

    private:
      using Lines = std::vector<std::vector<StyledRun>>;

      void block(const md::Block& source, const Nesting& nesting, std::string& marker);
      void prose(const md::Block& source, const Nesting& nesting, std::string& marker);
      void code(const md::Block& source, const Nesting& nesting, std::string& marker);
      void list(const md::Block& source, const Nesting& nesting, std::string& marker);
      void table(const md::Block& source, const Nesting& nesting, std::string& marker);
      void runs(std::span<const md::Run> list, const Face& face, Lines& lines, std::size_t depth) const;
      void push(StyledBlock&& out, std::string& marker);

      Builder& m_builder;
      const md::Document& m_document;
      FileKind m_kind{.content = ContentKind::Text, .language = Language::Markdown};
    };

    StyledBlock started(BlockKind kind, const Nesting& nesting) {
      StyledBlock out;
      out.kind = kind;
      out.indent = nesting.indent;
      out.quoteDepth = nesting.quoteDepth;
      return out;
    }

    void Writer::push(StyledBlock&& out, std::string& marker) {
      out.marker = std::move(marker);
      marker.clear();
      m_builder.document.blocks.push_back(std::move(out));
    }

    void Writer::blocks(std::span<const md::Block> list, const Nesting& nesting, std::string& marker) {
      if (nesting.depth > kMaxBlockNesting) {
        return;
      }
      for (const auto& source : list) {
        if (m_builder.linesLeft == 0) {
          m_builder.document.linesCapped = true;
          return;
        }
        block(source, nesting, marker);
      }
    }

    void Writer::block(const md::Block& source, const Nesting& nesting, std::string& marker) {
      switch (source.kind) {
      case md::BlockKind::Heading:
      case md::BlockKind::Paragraph:
        prose(source, nesting, marker);
        break;
      case md::BlockKind::Code:
        code(source, nesting, marker);
        break;
      case md::BlockKind::List:
        list(source, nesting, marker);
        break;
      case md::BlockKind::Quote: {
        const Nesting inner{
            .indent = nesting.indent,
            .quoteDepth = static_cast<std::uint8_t>(nesting.quoteDepth + 1),
            .depth = nesting.depth + 1
        };
        blocks(md::blocks(m_document, source.children), inner, marker);
        break;
      }
      case md::BlockKind::Table:
        table(source, nesting, marker);
        break;
      case md::BlockKind::Rule:
        push(started(BlockKind::Rule, nesting), marker);
        break;
      }
    }

    void Writer::prose(const md::Block& source, const Nesting& nesting, std::string& marker) {
      const bool heading = source.kind == md::BlockKind::Heading;
      const Face face{
          .base = heading              ? StyleRole::Heading
              : nesting.quoteDepth > 0 ? StyleRole::Quote
                                       : StyleRole::Plain
      };
      Lines lines(1);
      runs(md::runs(m_document, source.runs), face, lines, 0);
      auto out = started(BlockKind::Prose, nesting);
      out.heading = heading ? source.level : 0;
      for (auto& line : lines) {
        if (!addLine(m_builder, out, std::move(line), m_kind)) {
          break;
        }
      }
      if (!out.lines.empty()) {
        push(std::move(out), marker);
      }
    }

    void Writer::code(const md::Block& source, const Nesting& nesting, std::string& marker) {
      const auto kind = fenceKind(md::text(m_document, source.lang));
      auto out = started(BlockKind::Code, nesting);
      out.framed = true;
      addCodeLines(m_builder, out, md::text(m_document, source.text), kind, textSyntax(kind.language));
      if (!out.lines.empty()) {
        push(std::move(out), marker);
      }
    }

    void Writer::list(const md::Block& source, const Nesting& nesting, std::string& marker) {
      const Nesting inner{
          .indent = static_cast<std::uint8_t>(nesting.indent + 1),
          .quoteDepth = nesting.quoteDepth,
          .depth = nesting.depth + 1
      };
      const auto entries = md::items(m_document, source.children);
      for (std::size_t k = 0; k < entries.size(); ++k) {
        if (m_builder.linesLeft == 0) {
          m_builder.document.linesCapped = true;
          return;
        }
        std::string itemMarker = source.ordered ? std::to_string(source.start + k) + "." : std::string{kBullet};
        if (k == 0 && !marker.empty()) {
          // A list that opens an item shares the item's line.
          itemMarker = std::exchange(marker, {}).append(" ").append(itemMarker);
        }
        const auto content = md::blocks(m_document, entries[k]);
        if (content.empty()) {
          auto out = started(BlockKind::Prose, inner);
          if (addLine(m_builder, out, {}, m_kind)) {
            push(std::move(out), itemMarker);
          }
          continue;
        }
        blocks(content, inner, itemMarker);
      }
    }

    void Writer::table(const md::Block& source, const Nesting& nesting, std::string& marker) {
      const auto all = md::cells(m_document, source.cells);
      const auto columns = source.columns;
      if (columns == 0 || all.size() < columns) {
        return;
      }
      auto out = started(BlockKind::Table, nesting);
      out.columns = columns;
      for (const auto align : md::alignments(m_document, source.alignments)) {
        out.alignments.push_back(alignOf(align));
      }
      out.alignments.resize(columns, CellAlign::Start);
      // Whole rows only: the header, then body rows while the line limit leaves room for one.
      for (std::size_t row = 0; (row + 1) * columns <= all.size(); ++row) {
        if (m_builder.linesLeft < columns) {
          m_builder.document.linesCapped = true;
          break;
        }
        for (std::size_t column = 0; column < columns; ++column) {
          Lines lines(1);
          runs(md::runs(m_document, all[(row * columns) + column]), Face{.bold = row == 0}, lines, 0);
          // A hard break in a cell reads as a space.
          std::vector<StyledRun> cell;
          for (std::size_t k = 0; k < lines.size(); ++k) {
            if (k > 0) {
              appendRun(cell, " ", StyleRole::Plain, false, row == 0);
            }
            for (const auto& run : lines[k]) {
              appendRun(cell, run.text, run.role, run.italic, run.bold);
            }
          }
          // Never refused: the row's room was checked above.
          addLine(m_builder, out, std::move(cell), m_kind);
        }
      }
      if (!out.lines.empty()) {
        push(std::move(out), marker);
      }
    }

    // Runs into lines: a hard break starts a new line.
    void Writer::runs(std::span<const md::Run> list, const Face& face, Lines& lines, std::size_t depth) const {
      if (depth > kMaxRunNesting) {
        return;
      }
      for (const auto& run : list) {
        switch (run.kind) {
        case md::RunKind::Text:
          appendRun(lines.back(), md::text(m_document, run.text), roleOf(face), face.italic, face.bold);
          break;
        case md::RunKind::Code:
          appendRun(lines.back(), md::text(m_document, run.text), StyleRole::InlineCode, face.italic, face.bold);
          break;
        case md::RunKind::Break:
          lines.emplace_back();
          break;
        case md::RunKind::Emphasis: {
          Face inner = face;
          inner.italic = true;
          inner.base = face.base == StyleRole::Heading ? face.base : StyleRole::Emphasis;
          runs(md::runs(m_document, run.children), inner, lines, depth + 1);
          break;
        }
        case md::RunKind::Strong: {
          Face inner = face;
          inner.bold = true;
          inner.base = face.base == StyleRole::Heading ? face.base : StyleRole::Strong;
          runs(md::runs(m_document, run.children), inner, lines, depth + 1);
          break;
        }
        case md::RunKind::Link: {
          Face inner = face;
          inner.link = true;
          runs(md::runs(m_document, run.children), inner, lines, depth + 1);
          break;
        }
        case md::RunKind::Image:
          appendRun(lines.back(), md::text(m_document, run.alt), StyleRole::Link, face.italic, face.bold);
          break;
        }
      }
    }
  } // namespace

  void addMarkdown(Builder& builder, std::string_view text) {
    const auto document = md::parse(text);
    builder.document.parserCapped = document.truncated;
    Writer writer{builder, document};
    std::string marker;
    writer.blocks(md::blocks(document, document.root), {}, marker);
  }
} // namespace luciole::formats::styled
