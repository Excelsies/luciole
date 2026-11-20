#pragma once

#include "app/page/preview_edit_session.h"
#include "editing/source_lines.h"
#include "editing/text_buffer.h"
#include "formats/text_facts.h"
#include "fs/cancel_token.h"
#include "fs/location.h"
#include "fs/user_file_save.h"

#include <expected>
#include <functional>
#include <string>
#include <string_view>

class CodeEditorView;

namespace luciole {
  inline constexpr std::string_view kTooLargeToEdit = "Only part of this file fits here: it is larger than 1 MiB.";

  // A text file read whole for editing: its text, the facts that write it back, and the revision read.
  struct EditableText {
    editing::TextBuffer buffer;
    formats::TextFacts facts;
    UserFileIdentity identity;
  };

  // Blocking; run on a worker. Refuses, without reading, a file a save may not replace (userFileSaveRefusal). Then
  // reads the whole file, at most editing::kEditByteLimit bytes, and takes it for editing when all of it was read, it
  // holds no private key material and it is UTF-8 text the buffer accepts (no NUL, no line over
  // editing::kEditLineBytes). Otherwise one plain sentence says why not.
  std::expected<EditableText, std::string> readEditableText(const Location& location, const CancelToken& token);

  // readEditableText on a worker; done runs on the calling thread's main context, also after cancellation.
  void startEditableTextRead(
      Location location, const CancelToken& token,
      std::move_only_function<void(std::expected<EditableText, std::string>)> done
  );

  // The notice for a file with mixed line endings: a save writes `ending`, its most common one, throughout.
  std::string mixedEndingsNotice(formats::LineEnding ending);

  // One plain sentence for a save that did not go as asked; empty for Saved.
  std::string saveSentence(const UserFileSaveResult& result);

  // A text file edited in the code editor: its lines in a buffer, styled as source by the kind its name gives, and
  // written back with the line endings, byte order mark and final newline it was read with.
  class TextEditSession final : public PreviewEditSession {
  public:
    TextEditSession(EditableText text, std::string_view name);
    ~TextEditSession() override;
    TextEditSession(const TextEditSession&) = delete;
    TextEditSession& operator=(const TextEditSession&) = delete;
    TextEditSession(TextEditSession&&) = delete;
    TextEditSession& operator=(TextEditSession&&) = delete;

    std::unique_ptr<Node> makeView(TextClipboard& clipboard, PreviewEditEvents events) override;
    void dropView() override;
    InputArea* focusArea() const override;
    bool key(const InputArea::KeyData& key) override;
    void setShown(bool shown) override;
    void update(float textSize) override;
    bool modified() const override { return m_buffer.modified(); }
    std::uint64_t revision() const override { return m_buffer.revision(); }
    std::string fileBytes() const override;
    void saved(std::uint64_t revision) override;
    // LF, CRLF and CR each end a line; a last line without one counts too.
    FileShape shape(std::string_view bytes) const override;

  private:
    editing::TextBuffer m_buffer;
    formats::TextFacts m_facts;
    editing::SourceStyles m_styles; // over m_buffer, which never moves
    CodeEditorView* m_view = nullptr;
    float m_textSize = 0;
    bool m_shown = true;
  };
} // namespace luciole
