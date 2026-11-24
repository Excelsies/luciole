#include "imaging/text_document_loader.h"

#include "formats/hex_dump.h"
#include "fs/background_job.h"
#include "fs/file_metadata.h"
#include "model/private_key.h"

#include <algorithm>
#include <array>
#include <utility>

namespace luciole {
  namespace {
    struct TypeName {
      std::string_view type;
      std::string_view name; // a file name that fileKind reads as the type's kind
    };

    constexpr std::array kTypeNames{
        TypeName{"application/json", "a.json"},
        TypeName{"application/xml", "a.xml"},
        TypeName{"text/xml", "a.xml"},
        TypeName{"text/html", "a.html"},
        TypeName{"text/css", "a.css"},
        TypeName{"text/markdown", "a.md"},
        TypeName{"text/x-markdown", "a.md"},
        TypeName{"application/yaml", "a.yaml"},
        TypeName{"application/x-yaml", "a.yaml"},
        TypeName{"text/yaml", "a.yaml"},
        TypeName{"text/x-yaml", "a.yaml"},
        TypeName{"application/toml", "a.toml"},
        TypeName{"text/x-toml", "a.toml"},
        TypeName{"text/x-python", "a.py"},
        TypeName{"text/x-python3", "a.py"},
        TypeName{"application/x-shellscript", "a.sh"},
        TypeName{"text/x-shellscript", "a.sh"},
        TypeName{"text/x-csrc", "a.c"},
        TypeName{"text/x-chdr", "a.c"},
        TypeName{"text/x-c++src", "a.cpp"},
        TypeName{"text/x-c++hdr", "a.cpp"},
        TypeName{"text/rust", "a.rs"},
        TypeName{"text/x-rust", "a.rs"},
        TypeName{"text/x-go", "a.go"},
        TypeName{"application/x-desktop", "a.desktop"},
        TypeName{"text/x-patch", "a.diff"},
        TypeName{"text/x-diff", "a.diff"},
        TypeName{"text/csv", "a.csv"},
        TypeName{"text/tab-separated-values", "a.tsv"},
        TypeName{"application/sql", "a.sql"},
        TypeName{"text/x-lua", "a.lua"},
    };

    // The kind a content type names; the default kind when it names none.
    formats::FileKind kindOfType(std::string_view type) {
      if (type.empty()) {
        return {};
      }
      const auto* const found = std::ranges::find(kTypeNames, type, &TypeName::type);
      if (found != kTypeNames.end()) {
        return formats::fileKind(found->name);
      }
      if (type.ends_with("+json")) {
        return formats::fileKind("a.json");
      }
      if (type.ends_with("+xml")) {
        return formats::fileKind("a.xml");
      }
      return contentTypeIsText(std::string(type)) ? formats::fileKind("a.txt") : formats::FileKind{};
    }

    // Bytes that read as plain text: valid UTF-8 whose only control characters are horizontal and vertical tabs, line
    // breaks, form feeds and escapes.
    bool looksLikeText(std::string_view bytes) {
      constexpr std::string_view kAllowed = "\t\n\v\f\r\x1b";
      const bool clean = std::ranges::none_of(bytes, [&](char c) {
        return static_cast<unsigned char>(c) < 0x20 && !kAllowed.contains(c);
      });
      return clean && formats::textFacts(bytes).validUtf8;
    }

    // The bytes without an incomplete UTF-8 sequence or a lone CR at their end, where a read limit may have cut.
    std::string_view wholeCharacters(std::string_view bytes) {
      constexpr std::size_t kLongestSequence = 4;
      for (std::size_t back = 1; back <= std::min(bytes.size(), kLongestSequence); ++back) {
        const auto byte = static_cast<unsigned char>(bytes[bytes.size() - back]);
        if ((byte & 0xC0U) == 0x80U) {
          continue; // a continuation byte: keep looking for its lead
        }
        const std::size_t length = byte >= 0xF0U ? 4 : byte >= 0xE0U ? 3 : byte >= 0xC0U ? 2 : 1;
        if (length > back) {
          bytes.remove_suffix(back);
        }
        break;
      }
      if (bytes.ends_with('\r')) {
        bytes.remove_suffix(1);
      }
      return bytes;
    }

    // LF, CRLF and CR each end a line; a last line without one counts too.
    std::size_t lineCount(std::string_view text) {
      std::size_t lines = 0;
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || (text[i] == '\r' && (i + 1 == text.size() || text[i + 1] != '\n'))) {
          ++lines;
        }
      }
      return lines + (!text.empty() && text.back() != '\n' && text.back() != '\r' ? 1 : 0);
    }

    TextDocument refusal(const FileError& error) {
      TextDocument out;
      switch (error.kind) {
      case FileErrorKind::Cancelled:
        out.state = TextDocumentState::Cancelled;
        break;
      case FileErrorKind::Changed:
        out.state = TextDocumentState::Changed;
        break;
      default:
        out.state = TextDocumentState::Unreadable;
        out.message = error.message;
        break;
      }
      return out;
    }

    TextDocument hexDocument(const UserFileContent& content, const formats::FileKind& kind) {
      auto dump = formats::hexDump(content.bytes);
      const bool cut = content.truncated || dump.bytesCapped;
      return {
          .state = TextDocumentState::NotText,
          .document = std::make_shared<const formats::StyledDocument>(std::move(dump)),
          .kind = kind,
          .facts = {.bytes = content.bytes.size(), .lines = 0, .text = {}, .cut = cut},
          .identity = content.identity,
          .message = {},
      };
    }
  } // namespace

  TextDocument
  textDocument(std::expected<UserFileContent, FileError> read, std::string_view name, std::string_view contentType) {
    if (!read) {
      return refusal(read.error());
    }
    const auto& content = *read;
    if (containsPrivateKey(content.bytes)) {
      TextDocument refused;
      refused.state = TextDocumentState::PrivateKey;
      return refused;
    }
    if (isKeyContentType(contentType) && !looksLikePemText(content.bytes, content.truncated)) {
      TextDocument refused;
      refused.state = TextDocumentState::Container;
      return refused;
    }
    const auto text = content.truncated ? wholeCharacters(content.bytes) : std::string_view(content.bytes);
    auto kind = formats::fileKind(name);
    if (kind.content == formats::ContentKind::Other) {
      kind = kindOfType(contentType);
    }
    if (kind.content == formats::ContentKind::Other && looksLikeText(text)) {
      kind = formats::fileKind("a.txt");
    }
    if (kind.content != formats::ContentKind::Text || text.contains('\0')) {
      return hexDocument(content, kind);
    }
    auto document = formats::styledText(text, kind);
    const bool cut = content.truncated || document.bytesCapped || document.linesCapped;
    return {
        .state = cut ? TextDocumentState::Cut : TextDocumentState::Shown,
        .document = std::make_shared<const formats::StyledDocument>(std::move(document)),
        .kind = kind,
        .facts =
            {.bytes = content.bytes.size(), .lines = lineCount(text), .text = formats::textFacts(text), .cut = cut},
        .identity = content.identity,
        .message = {},
    };
  }

  void startTextDocumentLoad(
      Location location, std::string name, std::string contentType, const CancelToken& token,
      std::move_only_function<void(TextDocument)> done
  ) {
    startUserFileRead(
        std::move(location), kTextDocumentReadBytes, token,
        [name = std::move(name), contentType = std::move(contentType), token,
         done = std::move(done)](std::expected<UserFileContent, FileError> read) mutable {
          runJob<TextDocument>(
              token,
              [read = std::move(read), name = std::move(name), contentType = std::move(contentType)] mutable {
                return textDocument(std::move(read), name, contentType);
              },
              [done = std::move(done)](JobResult<TextDocument> reply) mutable {
                done(reply ? std::move(*reply) : refusal(reply.error()));
              }
          );
        }
    );
  }
} // namespace luciole
