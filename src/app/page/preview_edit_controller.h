#pragma once

#include "app/page/preview_edit_listing.h"
#include "app/page/preview_edit_session.h"
#include "editing/edit_gate.h"
#include "fs/cancel_token.h"
#include "fs/directory_entries.h"
#include "fs/location.h"
#include "fs/user_file_save.h"
#include "render/scene/input_area.h"
#include "ui/workbench/column_preview_view.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class TextClipboard;

namespace luciole {
  class DirectoryModel;
}

namespace luciole {
  struct EditableTable;
  struct EditableText;
  class TableEditSession;

  // Editing a file in a Workbench column preview, by the rules of editing::EditGate: the edit bar, the read for
  // editing, the save, and the one question "Save Changes to <name>?" when unsaved changes are left. It belongs to
  // the page and outlives the column preview it is attached to, so a question still opens, and Save still writes from
  // the model, after another view mode took the view away. Main context only.
  //
  // The column preview attaches its view and asks before it follows another target; the page offers keys, asks
  // before it goes, and calls onFrame() once per frame. The question is shown through the hooks: its answers come
  // back through the Question's callbacks.
  class PreviewEditController {
  public:
    // One showing of the question.
    struct Question {
      std::string name;
      std::string sentence; // why a save failed; empty for none
      std::function<void()> save;
      // The question closed without Save: No, its close button, Escape.
      std::function<void()> closed;
      // No was chosen; called before `closed`, which tells a choice from closing the question without one.
      std::function<void()> no;
    };

    // All are required.
    struct Hooks {
      std::function<void()> frameNeeded;
      std::function<std::uint32_t()> modifiers; // KeyMod bits held now
      TextClipboard* clipboard = nullptr;
      // Whether `area` has the keyboard, and giving it the keyboard.
      std::function<bool(const InputArea& area)> focused;
      std::function<void(InputArea& area)> focus;
      // Giving the keyboard to the file list, when the editor that had it closes or its slot goes out of sight.
      std::function<void()> focusList;
      // The listing listed `file` anew after a save replaced it: the list's cursor, gone with the old entry, goes back
      // to it.
      std::function<void(const Location& file)> reveal;
      // Whether the question can be shown now, showing it (true when it is shown), a failed save's sentence in the
      // shown question, and closing it.
      std::function<bool()> mayAsk;
      std::function<bool(Question question)> ask;
      std::function<void(const std::string& sentence)> tell;
      std::function<void()> dismiss;
    };

    // What the column preview does for the editor.
    struct Slot {
      std::function<void()> release; // let its content go: the editor's view takes its place
      std::function<void()> closed;  // the editor closed: show the latest target, freshly loaded
    };

    // Throws std::invalid_argument when a hook is empty.
    explicit PreviewEditController(Hooks hooks);
    ~PreviewEditController();
    PreviewEditController(const PreviewEditController&) = delete;
    PreviewEditController& operator=(const PreviewEditController&) = delete;
    PreviewEditController(PreviewEditController&&) = delete;
    PreviewEditController& operator=(PreviewEditController&&) = delete;

    // The column preview's side.
    // `view` shows the edit bar and, while a file is edited, the editor; it stays until detach().
    void attach(ColumnPreviewView& view, Slot slot);
    // The view goes: when a file is edited this is a leave, and the question opens from the next onFrame().
    void detach();
    // The file the slot shows as text or as a table now, or none; a new file is checked for editing off the main
    // context. CSV and TSV files are edited as tables, XLSX and ODS files are not edited.
    void offer(const DirectoryEntry* entry);
    // Before the slot follows its target: `aimed` is the file the columns preview, `entry` its listing entry, `shown`
    // whether the slot is on screen and `covered` whether the expanded instrument hides it. True when the slot may
    // follow; false while the editor keeps it: the target is the edited file, or another one while the question about
    // leaving it is open. Another target and a covered slot are leaves; a slot the layout hides keeps the editor out
    // of sight, with the keyboard in the list, until it shows again.
    bool follow(const std::optional<Location>& aimed, const DirectoryEntry* entry, bool shown, bool covered);
    // The target is to stay on the edited file when the list's cursor goes: while it is edited, and after a save
    // until the listing lists it anew and the cursor is put back.
    bool holdsCursor() const noexcept { return m_listed.has_value(); }
    // For a page whose list no Workbench follows: what `model` lists of the file edited last. A save replaces the
    // file, so the list lists it anew under another entry and loses its cursor; the cursor goes back to the file once
    // it is listed anew. Called once per frame; nothing to do while a file is edited or none was saved.
    void watch(const DirectoryModel& model);
    // The page went out of sight: the editor's view frees its textures and keeps its place and text.
    void setShown(bool shown);
    // Once per frame before layout, with the preview's content text size.
    void update(float textSize);

    // The page's side, behind PageEditing.
    bool textFocused() const;
    // A key offered to the editor first. The page shortcuts that leave the file (leavesEditedFile) are not taken: the
    // page runs them as it runs the buttons' actions.
    bool key(const InputArea::KeyData& key);
    // An open table cell field commits first, so its text counts.
    bool unsaved();
    bool saving() const { return m_gate.saving(); }
    // The file being edited, or none.
    const std::optional<Location>& file() const noexcept { return m_file; }
    // Changes to a file of the drive at `root` are not saved yet, or a save of one is running.
    bool unsavedIn(const Location& root);
    bool savingIn(const Location& root) const;
    // True when the page may go now; otherwise the question opens and `resume` runs once it is answered No, or
    // Save and the save succeeded.
    // With `dismissCancels`, closing the question without choosing No keeps the changes and the editor, and `resume`
    // runs with the changes still unsaved. That holds only while every request waiting on the question is such a
    // leave: a page's own leaves, Stop editing and another leave without it make the same dismissal mean No.
    bool leave(std::function<void()> resume, bool dismissCancels = false);
    // The page is deactivated while the question is shown: its popup closes without an answer and the question is
    // shown again by onFrame() once the page can show it.
    void keepQuestion();
    // Shows a question that waits.
    void onFrame();

    const editing::EditGate& gate() const noexcept { return m_gate; }
    // The lines or the cells of the file as last saved; none before the first save of this edit.
    const std::optional<FileShape>& savedShape() const noexcept { return m_savedShape; }

  private:
    enum class QuestionState : std::uint8_t { None, Wanted, Shown };

    struct Offer {
      DirectoryEntry entry;
      bool checked = false;
      std::string reason; // why it cannot be edited; empty when it can
    };

    // offer(), enable(), unsaved(), the table's opened() and settle() are in preview_edit_table.cpp.
    void enable();
    // Gives the editor the keyboard.
    void refocus();
    void opened(std::expected<EditableText, std::string> text);
    void opened(std::expected<EditableTable, std::string> table);
    // The read that enables editing ended; true when it refused, which the bar then says, or the gate did.
    bool refusedOpening(const std::string* reason);
    void begin(std::unique_ptr<PreviewEditSession> session, const UserFileIdentity& identity, std::string notice);
    // Commits text typed into a table's cell field, before a save or a leave asks about changes.
    void settle();
    void save();
    void stop();
    void saved(UserFileSaveResult result);
    void questionClosed();
    // The question was closed: true when that was no choice and the pending leaves give up (see leave()).
    bool dismissCancelsLeave();
    void requestLeave(bool dismissCancels = false);
    // Records what kind of request joins the question, forgetting the kinds of one that ended.
    void noteRequest(bool dismissCancels);
    void apply(const editing::GateResult& result);
    void failed(const std::string& sentence);
    void closeQuestion();
    void close();
    void showView();
    void dropView();
    void listed(const DirectoryEntry& entry);
    void refresh();
    ColumnPreviewEditBar bar() const;

    Hooks m_hooks;
    editing::EditGate m_gate;
    ColumnPreviewView* m_view = nullptr;
    Slot m_slot;
    std::optional<Offer> m_offer;
    CancelToken m_probe;
    std::optional<CancelToken> m_opening; // the read that enables editing
    // The file being edited.
    std::unique_ptr<PreviewEditSession> m_session;
    TableEditSession* m_table = nullptr; // m_session while it edits a table
    std::optional<Location> m_file;
    std::string m_name;
    UserFileIdentity m_identity;
    Node* m_editorNode = nullptr; // the session's view while the slot holds it
    std::string m_notice;
    // The file edited, and after editing the file saved last until the listing lists it anew.
    std::optional<EditedListing> m_listed;
    bool m_editorShown = false;
    bool m_hadKeyboard = false; // the editor had the keyboard in its last shown frame
    bool m_leaving = false;
    bool m_cancelLeaves = false;  // a leave that gives up when the question is closed without a choice waits
    bool m_otherRequests = false; // a request that does not waits
    bool m_noChosen = false;      // No was pressed, as against Escape or the close button
    // The save.
    std::optional<FileShape> m_savedShape;
    FileShape m_savingShape;
    CancelToken m_saveToken;
    std::uint64_t m_saveRevision = 0;
    // The question.
    QuestionState m_question = QuestionState::None;
    std::string m_questionSentence;
    std::vector<std::function<void()>> m_resumes;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0); // answers from workers check it is still there
  };
} // namespace luciole
