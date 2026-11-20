#include "app/page/preview_edit_text.h"

#include "formats/file_kind.h"
#include "fs/background_job.h"
#include "model/private_key.h"
#include "ui/controls/code_editor_view.h"

#include <format>
#include <optional>
#include <utility>

namespace luciole {
  namespace {
    using Status = UserFileSaveStatus;

    // A system message as the end of a sentence: without a final period, which the sentence adds.
    std::string_view clause(std::string_view message) {
      while (message.ends_with('.')) {
        message.remove_suffix(1);
      }
      return message;
    }

    // A non-local file is refused before the read, so an unsupported read is a file that is not regular.
    std::string readRefusal(const FileError& error) {
      switch (error.kind) {
      case FileErrorKind::Unsupported:
        return "Only regular files can be edited.";
      case FileErrorKind::Changed:
        return "The file changed while it was read.";
      case FileErrorKind::Cancelled:
        return "Cancelled.";
      default:
        return std::format("This file cannot be read: {}.", clause(error.message));
      }
    }

    // Why a save could not replace the file, as the edit bar says it before editing starts; none for a file that is
    // gone or cannot be inspected, which the read then names.
    std::optional<std::string> editRefusal(const Location& location) {
      const auto refused = userFileSaveRefusal(location);
      if (!refused) {
        return std::nullopt;
      }
      switch (refused->status) {
      case Status::ReadOnlyFile:
        return "This file is read-only.";
      case Status::ReadOnlyFolder:
        return "The folder of this file is read-only.";
      case Status::HardLinked:
        return "This file has other hard links.";
      case Status::NotLocal:
        return "Only local files can be edited.";
      case Status::NotRegularFile:
        return "Only regular files can be edited.";
      default:
        return std::nullopt;
      }
    }

    // The kind the name gives, plain text for a name that gives no text kind.
    formats::FileKind kindOf(std::string_view name) {
      auto kind = formats::fileKind(name);
      return kind.content == formats::ContentKind::Text ? kind : formats::fileKind("a.txt");
    }

    std::string_view endingName(formats::LineEnding ending) {
      switch (ending) {
      case formats::LineEnding::CrLf:
        return "CRLF";
      case formats::LineEnding::Cr:
        return "CR";
      case formats::LineEnding::Lf:
      case formats::LineEnding::None:
        return "LF";
      }
      return "LF";
    }
  } // namespace

  std::expected<EditableText, std::string> readEditableText(const Location& location, const CancelToken& token) {
    // The checks that need no read come first, so a file they refuse is not read.
    if (auto refused = editRefusal(location)) {
      return std::unexpected(std::move(*refused));
    }
    auto read = readUserFile(location, editing::kEditByteLimit, token);
    if (!read) {
      return std::unexpected(readRefusal(read.error()));
    }
    if (read->truncated) {
      return std::unexpected(std::string(kTooLargeToEdit));
    }
    if (containsPrivateKey(read->bytes)) {
      return std::unexpected("This file holds a private key.");
    }
    const auto facts = formats::textFacts(read->bytes);
    if (!facts.validUtf8) {
      return std::unexpected("This file is not UTF-8 text.");
    }
    auto buffer = editing::TextBuffer::create(formats::editorText(read->bytes));
    if (!buffer) {
      return std::unexpected(std::move(buffer.error()));
    }
    return EditableText{.buffer = std::move(*buffer), .facts = facts, .identity = read->identity};
  }

  void startEditableTextRead(
      Location location, const CancelToken& token,
      std::move_only_function<void(std::expected<EditableText, std::string>)> done
  ) {
    using Reply = std::expected<EditableText, std::string>;
    runJob<Reply>(
        token, [location = std::move(location), token] { return readEditableText(location, token); },
        [done = std::move(done)](JobResult<Reply> reply) mutable {
          done(reply ? std::move(*reply) : std::unexpected(readRefusal(reply.error())));
        }
    );
  }

  std::string mixedEndingsNotice(formats::LineEnding ending) {
    return std::format(
        "This file mixes line endings. Saving writes {}, its most common one, throughout.", endingName(ending)
    );
  }

  std::string saveSentence(const UserFileSaveResult& result) {
    switch (result.status) {
    case Status::Saved:
      return {};
    case Status::SavedNotFlushed:
      return "The file was saved, but it may not be on the disk yet.";
    case Status::ChangedOnDisk:
      return "The file changed on disk since it was read, so it was not saved.";
    case Status::ReadOnlyFile:
      return "The file is read-only, so it was not saved.";
    case Status::ReadOnlyFolder:
      return "The folder is read-only, so the file was not saved.";
    case Status::HardLinked:
      return "The file has other hard links, so it was not saved.";
    case Status::NotRegularFile:
      return "Only regular files can be saved.";
    case Status::OwnerNotKept:
      return "The file's owner or permissions could not be kept, so it was not saved.";
    case Status::AttributesNotKept:
      return "The file's extended attributes could not be kept, so it was not saved.";
    case Status::NoSpace:
      return "There is no space left on the disk, so the file was not saved.";
    case Status::Cancelled:
      return "The save was cancelled.";
    case Status::NotLocal:
      return "Only local files can be saved.";
    case Status::Failed:
      return std::format("The file was not saved: {}.", clause(result.message));
    }
    return std::format("The file was not saved: {}.", clause(result.message));
  }

  TextEditSession::TextEditSession(EditableText text, std::string_view name)
      : m_buffer(std::move(text.buffer)), m_facts(text.facts),
        m_styles(kindOf(name), m_buffer.lineCount(), [this](std::size_t index) { return m_buffer.line(index); }) {}

  TextEditSession::~TextEditSession() = default;

  std::unique_ptr<Node> TextEditSession::makeView(TextClipboard& clipboard, PreviewEditEvents events) {
    auto view = std::make_unique<CodeEditorView>(m_buffer, m_styles, clipboard, editing::detectIndent(m_buffer));
    view->onEdited = std::move(events.edited);
    view->onSaveRequested = std::move(events.saveRequested);
    view->onEscape = std::move(events.escape);
    view->onRefused = std::move(events.refused);
    view->onFrameNeeded = std::move(events.frameNeeded);
    view->modifiers = std::move(events.modifiers);
    if (m_textSize > 0) {
      view->setTextSize(m_textSize);
    }
    m_view = view.get();
    m_shown = true;
    return view;
  }

  void TextEditSession::dropView() { m_view = nullptr; }

  InputArea* TextEditSession::focusArea() const { return m_view != nullptr ? &m_view->inputArea() : nullptr; }

  bool TextEditSession::key(const InputArea::KeyData& key) { return m_view != nullptr && m_view->key(key); }

  void TextEditSession::setShown(bool shown) {
    if (m_view != nullptr && shown != m_shown) {
      m_shown = shown;
      m_view->setShown(shown);
    }
  }

  void TextEditSession::update(float textSize) {
    if (m_view == nullptr) {
      return;
    }
    if (textSize != m_textSize) {
      m_textSize = textSize;
      m_view->setTextSize(textSize);
    }
    if (m_shown) {
      m_view->update();
    }
  }

  std::string TextEditSession::fileBytes() const { return formats::fileBytes(m_buffer.text(), m_facts); }

  FileShape TextEditSession::shape(std::string_view bytes) const {
    std::size_t lines = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      if (bytes[i] == '\n' || (bytes[i] == '\r' && (i + 1 == bytes.size() || bytes[i + 1] != '\n'))) {
        ++lines;
      }
    }
    const bool open = !bytes.empty() && bytes.back() != '\n' && bytes.back() != '\r';
    return {.lines = lines + (open ? 1 : 0), .cells = std::nullopt};
  }

  void TextEditSession::saved(std::uint64_t revision) {
    // A state undone and replaced since the save began cannot be marked; the text then stays modified.
    (void)m_buffer.markSaved(revision);
  }
} // namespace luciole
