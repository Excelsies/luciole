#pragma once

#include "app/page/preview_edit_session.h"
#include "editing/table_session.h"
#include "formats/table_grid.h"
#include "fs/cancel_token.h"
#include "fs/location.h"
#include "fs/user_file_save.h"
#include "ui/controls/table_view_cell.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace luciole {
  class TableView;
  struct TableEditAvailability;
  struct TableEditRequest;

  inline constexpr std::string_view kSpreadsheetReadOnly = "XLSX and ODS spreadsheets are read-only here.";
  // A table of at most this many bytes gets its new grid at once after a change; a larger one from a worker.
  inline constexpr std::size_t kInlineGridBytes = 256 * 1024;

  // A delimited file read whole for editing: its bytes in a session with the dialect detected once, the grid that
  // shows all of it, and the revision read.
  struct EditableTable {
    editing::TableSession session;
    std::shared_ptr<const formats::TableGrid> grid;
    UserFileIdentity identity;
  };

  // Blocking; run on a worker. Takes the file for editing when readEditableText would take it as text, it reads
  // the same, as a table it has no quote that is never closed, and the grid shows every row and column of it.
  // `tabs` splits it at tabs, as a TSV. Otherwise one plain sentence says why not.
  std::expected<EditableTable, std::string>
  readEditableTable(const Location& location, bool tabs, const CancelToken& token);

  // readEditableTable on a worker; done runs on the calling thread's main context, also after cancellation.
  void startEditableTableRead(
      Location location, bool tabs, const CancelToken& token,
      std::move_only_function<void(std::expected<EditableTable, std::string>)> done
  );

  // A CSV or TSV file edited as a table: the session holds the bytes and every change, and the table view only asks
  // for changes. After each change the grid is built again from the session's bytes: at once for a table of at most
  // kInlineGridBytes, otherwise on a worker, and the view shows the newest grid that arrived. Saving writes the
  // session's bytes as they are.
  class TableEditSession final : public PreviewEditSession {
  public:
    // `focus` gives the keyboard to the view's grid or its cell field.
    TableEditSession(EditableTable table, std::function<void(InputArea& area)> focus);
    ~TableEditSession() override;
    TableEditSession(const TableEditSession&) = delete;
    TableEditSession& operator=(const TableEditSession&) = delete;
    TableEditSession(TableEditSession&&) = delete;
    TableEditSession& operator=(TableEditSession&&) = delete;

    std::unique_ptr<Node> makeView(TextClipboard& clipboard, PreviewEditEvents events) override;
    void dropView() override;
    // The cell field while it is open, the grid otherwise.
    InputArea* focusArea() const override;
    // Offered to the table first; Escape on the grid stops editing, as in the text editor.
    bool key(const InputArea::KeyData& key) override;
    // Hidden, an open cell field commits.
    void setShown(bool shown) override;
    void update(float textSize) override;
    bool modified() const override { return m_session.modified(); }
    std::uint64_t revision() const override { return m_session.revision(); }
    std::string fileBytes() const override { return m_session.bytes(); }
    void saved(std::uint64_t revision) override { m_session.markSaved(revision); }
    // The rows below a header row and the columns of the session's table.
    FileShape shape(std::string_view bytes) const override;

    // Commits an open cell field, so its text is a change before a save or a leave asks about changes.
    void commitField();
    // Whether a newer worker-built grid waits for an open cell field to close.
    bool gridPending() const noexcept { return m_gridPending; }

  private:
    void cellEdited(TableCell source, const std::string& text);
    void request(const TableEditRequest& request);
    // Makes the change a request asks for; a refusal's sentence when it is refused.
    std::optional<std::string> change(const TableEditRequest& request);
    // After a change was asked for: `before` is the revision it started from.
    void changed(std::uint64_t before);
    bool fitsGrid(std::size_t rows, std::size_t columns) const;
    void rebuild();
    void showGrid(std::uint64_t build, std::shared_ptr<const formats::TableGrid> grid);
    void applyPendingGrid();
    std::string wholeText(TableCell source) const;
    TableEditAvailability availability() const;

    editing::TableSession m_session;
    std::shared_ptr<const formats::TableGrid> m_grid; // the newest grid built
    std::function<void(InputArea& area)> m_focus;
    PreviewEditEvents m_events;
    TableView* m_view = nullptr;
    std::shared_ptr<int> m_viewToken;
    std::optional<TableCell> m_selected; // the source cell selected while a view is shown or after it went
    std::uint64_t m_builds = 0;          // grids asked for
    std::shared_ptr<std::atomic<std::uint64_t>> m_latestBuild = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::uint64_t m_shown = 0; // the build of m_grid
    float m_textSize = 0;
    bool m_visible = true;
    bool m_gridPending = false;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0); // grids from workers check it is still there
  };
} // namespace luciole
