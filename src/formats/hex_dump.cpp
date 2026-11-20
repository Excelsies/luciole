#include "formats/hex_dump.h"

#include "formats/styled_markdown.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace luciole::formats {
  namespace {
    constexpr std::string_view kDigits = "0123456789abcdef";
    constexpr std::size_t kGroupBytes = 8;
    constexpr std::size_t kOffsetDigits = 8;

    void add(std::vector<StyledRun>& runs, std::string_view text, StyleRole role) {
      styled::appendRun(runs, text, role, false, false);
    }

    std::array<char, 2> hexByte(std::uint8_t value) { return {kDigits[value >> 4U], kDigits[value & 0x0FU]}; }

    bool printable(std::uint8_t value) { return value >= 0x20 && value < 0x7F; }

    std::size_t choiceFor(std::size_t rowBytes) {
      const auto* const found = std::ranges::find_if(kHexDumpRowChoices, [&](std::size_t n) { return rowBytes >= n; });
      return found != kHexDumpRowChoices.end() ? *found : kHexDumpRowChoices.back();
    }

    // Columns of a row of `rowBytes`: where byte `index` starts, and where the text's opening bar sits.
    std::size_t byteColumn(std::size_t index) {
      return kOffsetDigits + 2 + (3 * index) + (index >= kGroupBytes ? 1 : 0);
    }
    std::size_t barColumn(std::size_t rowBytes) { return byteColumn(rowBytes - 1) + 4; }

    std::string offsetText(std::size_t offset) {
      std::string digits(kOffsetDigits, '0');
      for (std::size_t i = 0; i < kOffsetDigits; ++i) {
        digits[kOffsetDigits - 1 - i] = kDigits[offset & 0x0FU];
        offset >>= 4U;
      }
      return digits;
    }

    // One row: the offset, the hex of up to `rowBytes` bytes padded to a full row, and the bytes as text between bars.
    StyledLine row(std::string_view bytes, std::size_t offset, std::size_t rowBytes) {
      std::vector<StyledRun> runs;
      add(runs, offsetText(offset), StyleRole::Comment);
      add(runs, " ", StyleRole::Plain);
      for (std::size_t i = 0; i < rowBytes; ++i) {
        add(runs, i == kGroupBytes ? "  " : " ", StyleRole::Plain);
        if (i >= bytes.size()) {
          add(runs, "  ", StyleRole::Plain);
          continue;
        }
        const auto byte = static_cast<std::uint8_t>(bytes[i]);
        const auto hex = hexByte(byte);
        add(runs, {hex.data(), hex.size()}, byte == 0 ? StyleRole::Punctuation : StyleRole::Number);
      }
      add(runs, "  ", StyleRole::Plain);
      add(runs, "|", StyleRole::Punctuation);
      for (const char c : bytes) {
        const auto byte = static_cast<std::uint8_t>(c);
        add(runs, printable(byte) ? std::string_view(&c, 1) : ".",
            printable(byte) ? StyleRole::Plain : StyleRole::Punctuation);
      }
      add(runs, "|", StyleRole::Punctuation);
      return {.runs = std::move(runs), .swatches = {}, .continues = false};
    }

    std::optional<std::uint8_t> digit(char c) {
      const auto at = kDigits.find(c);
      return at == std::string_view::npos ? std::nullopt : std::optional(static_cast<std::uint8_t>(at));
    }

    // Appends the bytes of one row as row() writes it at `offset`; the count read, or none for anything else.
    std::optional<std::size_t>
    readRow(std::string_view text, std::size_t offset, std::size_t rowBytes, std::string& out) {
      const auto bar = barColumn(rowBytes);
      if (text.size() < bar + 3 || text.size() > bar + 2 + rowBytes || text[bar] != '|' || text.back() != '|') {
        return std::nullopt;
      }
      const std::size_t count = text.size() - bar - 2;
      if (text.substr(0, kOffsetDigits) != offsetText(offset) || text.substr(bar - 2, 2) != "  ") {
        return std::nullopt;
      }
      for (std::size_t i = 0; i < rowBytes; ++i) {
        const auto at = byteColumn(i);
        const auto gap = text.substr(at - (i == kGroupBytes ? 2 : 1), i == kGroupBytes ? 2 : 1);
        const auto high = digit(text[at]);
        const auto low = digit(text[at + 1]);
        if (gap.find_first_not_of(' ') != std::string_view::npos) {
          return std::nullopt;
        }
        if (i >= count) {
          if (text.substr(at, 2) != "  ") {
            return std::nullopt;
          }
          continue;
        }
        if (!high || !low) {
          return std::nullopt;
        }
        const auto byte = static_cast<std::uint8_t>((*high << 4U) | *low);
        const char shown = text[bar + 1 + i];
        if (shown != (printable(byte) ? static_cast<char>(byte) : '.')) {
          return std::nullopt;
        }
        out += static_cast<char>(byte);
      }
      return count;
    }

    bool oneCodeBlock(const StyledDocument& document) {
      return document.blocks.size() == 1 && document.blocks[0].kind == BlockKind::Code;
    }
  } // namespace

  StyledDocument hexDump(std::string_view bytes, std::size_t rowBytes) {
    const auto rows = choiceFor(rowBytes);
    StyledDocument document;
    document.bytesCapped = bytes.size() > kHexDumpBytes;
    const auto shown = bytes.substr(0, std::min(bytes.size(), kHexDumpBytes));
    auto& block = document.blocks.emplace_back();
    block.kind = BlockKind::Code;
    block.lines.reserve((shown.size() + rows - 1) / rows);
    // One row per `rows` bytes of at most kHexDumpBytes.
    for (std::size_t offset = 0; offset < shown.size(); offset += rows) {
      block.lines.push_back(row(shown.substr(offset, rows), offset, rows));
    }
    return document;
  }

  std::size_t hexDumpRowWidth(std::size_t rowBytes) {
    const auto rows = choiceFor(rowBytes);
    return barColumn(rows) + rows + 2;
  }

  std::size_t hexDumpRowsFitting(double columns) {
    const auto* const found = std::ranges::find_if(kHexDumpRowChoices, [&](std::size_t n) {
      return static_cast<double>(hexDumpRowWidth(n)) <= columns;
    });
    return found != kHexDumpRowChoices.end() ? *found : kHexDumpRowChoices.back();
  }

  std::size_t hexDumpRowBytes(const StyledDocument& dump) {
    if (!oneCodeBlock(dump) || dump.blocks[0].lines.empty()) {
      return 0;
    }
    const auto text = joinedText(dump.blocks[0].lines[0]);
    std::string bytes;
    const auto* const found = std::ranges::find_if(kHexDumpRowChoices, [&](std::size_t n) {
      return text.find('|') == barColumn(n) && readRow(text, 0, n, bytes);
    });
    return found != kHexDumpRowChoices.end() ? *found : 0;
  }

  std::optional<StyledDocument> hexDumpAgain(const StyledDocument& dump, std::size_t rowBytes) {
    if (!oneCodeBlock(dump)) {
      return std::nullopt;
    }
    const auto& lines = dump.blocks[0].lines;
    const auto from = hexDumpRowBytes(dump);
    if (!lines.empty() && (from == 0 || lines.size() > (kHexDumpBytes + from - 1) / from)) {
      return std::nullopt;
    }
    std::string bytes;
    // Bounded by the rows of at most kHexDumpBytes checked above.
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const auto count = readRow(joinedText(lines[index]), index * from, from, bytes);
      if (!count || (*count != from && index + 1 != lines.size())) {
        return std::nullopt;
      }
    }
    auto again = hexDump(bytes, rowBytes);
    again.bytesCapped = dump.bytesCapped;
    return again;
  }
} // namespace luciole::formats
