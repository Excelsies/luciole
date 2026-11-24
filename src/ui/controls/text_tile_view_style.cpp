#include "ui/controls/text_tile_view_style.h"

#include "formats/hex_dump.h"
#include "fs/gio_ptr.h"
#include "ui/palette.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <glib.h>

namespace luciole::text_tile_view {
  namespace {
    using formats::StyleRole;

    std::string sizeText(std::uint64_t bytes) {
      const GString text(g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS));
      return text ? std::string(text.get()) : std::to_string(bytes);
    }

    std::string cutWords(const TextDocument& source) {
      if (source.document && source.document->linesCapped) {
        return std::format("Showing the first {} lines", formats::kStyledTextLines);
      }
      if (source.identity) {
        return std::format("Showing the first {} of {}", sizeText(source.facts.bytes), sizeText(source.identity->size));
      }
      return std::format("Showing the first {}", sizeText(source.facts.bytes));
    }
  } // namespace

  TextTileTheme paletteTheme() {
    TextTileTheme theme;
    theme.roles.fill(colorForRole(ColorRole::OnSurface));
    const auto set = [&](StyleRole role, ColorRole color) {
      theme.roles[static_cast<std::size_t>(role)] = colorForRole(color);
    };
    set(StyleRole::Keyword, ColorRole::Primary);
    set(StyleRole::String, ColorRole::Tertiary);
    set(StyleRole::Number, ColorRole::Secondary);
    set(StyleRole::Literal, ColorRole::Secondary);
    set(StyleRole::Comment, ColorRole::OnSurfaceVariant);
    set(StyleRole::Punctuation, ColorRole::OnSurfaceVariant);
    set(StyleRole::Key, ColorRole::Primary);
    set(StyleRole::Tag, ColorRole::Secondary);
    set(StyleRole::Attribute, ColorRole::Tertiary);
    set(StyleRole::Addition, ColorRole::Tertiary);
    set(StyleRole::Removal, ColorRole::Error);
    set(StyleRole::Hunk, ColorRole::Secondary);
    set(StyleRole::DiffHeader, ColorRole::Primary);
    set(StyleRole::Heading, ColorRole::Primary);
    set(StyleRole::Link, ColorRole::Secondary);
    set(StyleRole::Quote, ColorRole::OnSurfaceVariant);
    theme.background = colorForRole(ColorRole::SurfaceContainerLow);
    theme.background.a = 1.0F; // tiles are uploaded as opaque pixels
    theme.codeBackground = colorForRole(ColorRole::SurfaceContainerHigh);
    theme.outline = colorForRole(ColorRole::OutlineVariant);
    theme.checkerLight = colorForRole(ColorRole::SurfaceContainerHighest);
    theme.checkerDark = colorForRole(ColorRole::SurfaceContainer);
    return theme;
  }

  bool wraps(const formats::StyledDocument& document) {
    return std::ranges::any_of(document.blocks, [](const formats::StyledBlock& block) {
      return block.kind != formats::BlockKind::Code || block.framed;
    });
  }

  std::pair<std::string, bool> wordsOf(const TextDocument& source) {
    switch (source.state) {
    case TextDocumentState::Shown:
      return {source.facts.bytes == 0 ? "Empty file" : "", false};
    case TextDocumentState::Cut:
      return {cutWords(source), true};
    case TextDocumentState::NotText:
      return {
          source.facts.cut
              ? std::format("Not UTF-8 text: showing the first {} as hex", sizeText(formats::kHexDumpBytes))
              : std::string("Not UTF-8 text: showing it as hex"),
          true
      };
    case TextDocumentState::PrivateKey:
      return {"This file holds a private key, so it is not shown.", false};
    case TextDocumentState::Container:
      return {"This is a binary certificate or key container, so it is not shown.", false};
    case TextDocumentState::Unreadable:
      return {
          source.message.empty() ? "This file could not be read." : "This file could not be read: " + source.message,
          false
      };
    case TextDocumentState::Changed:
      return {"This file changed while it was read.", false};
    case TextDocumentState::Cancelled:
      break;
    }
    return {"", false};
  }

  std::string colorText(const formats::Rgba& color) {
    return color.a == 255 ? std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b)
                          : std::format("#{:02x}{:02x}{:02x}{:02x}", color.r, color.g, color.b, color.a);
  }
} // namespace luciole::text_tile_view
