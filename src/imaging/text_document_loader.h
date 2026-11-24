#pragma once

#include "formats/file_kind.h"
#include "formats/styled_text.h"
#include "formats/text_facts.h"
#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/location.h"
#include "fs/user_file_save.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// A file's text read and styled off the main context, for a text preview.
namespace luciole {
  inline constexpr std::size_t kTextDocumentReadBytes = formats::kStyledTextBytes;

  enum class TextDocumentState : std::uint8_t {
    Shown,      // the whole text
    Cut,        // the first part of a file too large to show in full
    NotText,    // a hex dump of the first bytes
    PrivateKey, // the bytes hold private key material; nothing is shown
    Container,  // a binary certificate or key container; nothing is shown, not even as hex
    Unreadable, // message holds the reason
    Changed,    // the file changed while it was read
    Cancelled,
  };

  struct TextDocumentFacts {
    std::size_t bytes = 0; // bytes read
    std::size_t lines = 0; // lines of the text read; zero for NotText
    formats::TextFacts text{};
    bool cut = false; // the file holds more than the document shows
  };

  struct TextDocument {
    TextDocumentState state = TextDocumentState::Cancelled;
    std::shared_ptr<const formats::StyledDocument> document; // Shown, Cut and NotText
    formats::FileKind kind{};
    TextDocumentFacts facts{};
    std::optional<UserFileIdentity> identity; // the revision read, with the document
    std::string message;                      // Unreadable: the system's reason
  };

  // Blocking; run on a worker. The kind comes from the name, then from the content type, then from the bytes (valid
  // UTF-8 whose only control characters are tabs, line breaks, form feeds and escapes is plain text). Private
  // key material is refused before anything else. A file of a non-text kind, or text holding a NUL byte, is NotText
  // with a hex dump. A read cut at its limit drops an incomplete last character or a lone final CR before the facts
  // are taken. Text is Cut when the read or the document stops short of the file.
  TextDocument
  textDocument(std::expected<UserFileContent, FileError> read, std::string_view name, std::string_view contentType);

  // Reads at most kTextDocumentReadBytes on a worker and builds the document on another; done runs on the calling
  // thread's main context, also after cancellation.
  void startTextDocumentLoad(
      Location location, std::string name, std::string contentType, const CancelToken& token,
      std::move_only_function<void(TextDocument)> done
  );
} // namespace luciole
