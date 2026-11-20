#include "app/page/preview_edit_table.h"

#include "app/page/preview_edit_controller.h"
#include "app/page/preview_edit_text.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "editing/text_buffer.h"
#include "fs/background_job.h"
#include "fs/directory_entries.h"
#include "thumbnails/table_loader.h"
#include "ui/controls/table_view.h"

#include <algorithm>
#include <utility>

namespace luciole {
  namespace {
    constexpr std::string_view kChangedWhileRead = "The file changed while it was read.";
    constexpr std::string_view kUnreadable = "This file cannot be read.";
    constexpr std::string_view kBeyondGrid =
        "This table has more rows or columns than the preview shows, so it cannot be edited.";
    constexpr std::string_view kNoRowsToEdit = "This table has no rows to edit.";
    constexpr std::string_view kGridFull =
        "This change would give the table more rows or columns than the preview shows.";
    constexpr std::string_view kGridLost = "The table could not be shown after this change.";
    constexpr std::uint32_t kModifiers = KeyMod::Shift | KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super;

    std::string readSentence(const FileError& error) {
      return std::string(error.kind == FileErrorKind::Cancelled ? std::string_view("Cancelled.") : kUnreadable);
    }

    std::optional<std::string> refusal(const std::expected<void, editing::TableError>& result) {
      return result ? std::nullopt : std::optional(result.error().message);
    }

    // The same entry with the same facts; names are compared before the locations, which cost more.
    bool sameListing(const DirectoryEntry& a, const DirectoryEntry& b) {
      return a.name == b.name
          && a.size == b.size
          && a.modifiedSeconds == b.modifiedSeconds
          && a.location() == b.location();
    }

    // Reads a file for editing on a worker: a CSV or TSV file as a table, any other as text. `done` takes either
    // answer on the main context, also after cancellation.
    template <typename Done> void startEditableRead(const DirectoryEntry& entry, const CancelToken& token, Done done) {
      const auto table = tableSource(entry.name, entry.contentType.value_or(""));
      if (table) {
        startEditableTableRead(entry.location(), table == TableSource::Tsv, token, std::move(done));
      } else {
        startEditableTextRead(entry.location(), token, std::move(done));
      }
    }
  } // namespace

  std::expected<EditableTable, std::string>
  readEditableTable(const Location& location, bool tabs, const CancelToken& token) {
    // A file is edited as a table only where it could be edited as text.
    const auto text = readEditableText(location, token);
    if (!text) {
      return std::unexpected(text.error());
    }
    // The table edits the bytes themselves, which the text read keeps only as lines.
    auto read = readUserFile(location, editing::kTableByteLimit, token);
    if (!read) {
      return std::unexpected(readSentence(read.error()));
    }
    if (read->truncated || read->identity != text->identity) {
      return std::unexpected(std::string(kChangedWhileRead));
    }
    const auto dialect = delimitedDialect(read->bytes, tabs);
    auto session = editing::TableSession::open(std::move(read->bytes), dialect);
    if (!session) {
      return std::unexpected(std::move(session.error().message));
    }
    auto grid = delimitedGrid(session->bytes(), dialect);
    if (grid.truncated) {
      return std::unexpected(std::string(kBeyondGrid));
    }
    if (grid.rowCount == 0 || grid.columnCount == 0) {
      return std::unexpected(std::string(kNoRowsToEdit));
    }
    return EditableTable{
        .session = std::move(*session),
        .grid = std::make_shared<const formats::TableGrid>(std::move(grid)),
        .identity = read->identity,
    };
  }

  void startEditableTableRead(
      Location location, bool tabs, const CancelToken& token,
      std::move_only_function<void(std::expected<EditableTable, std::string>)> done
  ) {
    using Reply = std::expected<EditableTable, std::string>;
    runJob<Reply>(
        token, [location = std::move(location), tabs, token] { return readEditableTable(location, tabs, token); },
        [done = std::move(done)](JobResult<Reply> reply) mutable {
          done(reply ? std::move(*reply) : std::unexpected(readSentence(reply.error())));
        }
    );
  }

  TableEditSession::TableEditSession(EditableTable table, std::function<void(InputArea& area)> focus)
      : m_session(std::move(table.session)), m_grid(std::move(table.grid)), m_focus(std::move(focus)) {}

  TableEditSession::~TableEditSession() {
    m_latestBuild->store(++m_builds, std::memory_order_relaxed);
    dropView();
  }

  std::unique_ptr<Node> TableEditSession::makeView(TextClipboard& /*clipboard*/, PreviewEditEvents events) {
    dropView();
    m_events = std::move(events);
    auto view = std::make_unique<TableView>();
    m_view = view.get();
    m_viewToken = std::make_shared<int>(0);
    const auto token = std::weak_ptr<int>(m_viewToken);
    view->onFrameNeeded = [this, token] {
      if (!token.expired()) {
        m_events.frameNeeded();
      }
    };
    view->cellText = [this, token](TableCell source) { return token.expired() ? std::string{} : wholeText(source); };
    view->onCellEdited = [this, token](TableCell source, const std::string& text) {
      if (!token.expired()) {
        cellEdited(source, text);
      }
    };
    view->onEditRequest = [this, token](const TableEditRequest& request) {
      if (!token.expired()) {
        this->request(request);
      }
    };
    view->onSaveRequested = [this, token] {
      if (!token.expired()) {
        m_events.saveRequested();
      }
    };
    view->onFocus = [this, token](InputArea* area) {
      if (!token.expired() && area != nullptr) {
        m_focus(*area);
      }
    };
    if (m_textSize > 0) {
      view->setTextSize(m_textSize);
    }
    view->setTable(m_grid);
    view->setEditable(true);
    view->setEditAvailability(availability());
    if (m_selected && m_selected->row >= m_grid->firstSourceRow) {
      view->select({.row = m_selected->row - m_grid->firstSourceRow, .column = m_selected->column});
    }
    m_selected = view->selectedSource();
    view->onSelectionChanged = [this, token](TableCell source) {
      if (!token.expired()) {
        m_selected = source;
      }
    };
    m_visible = true;
    return view;
  }

  void TableEditSession::dropView() {
    m_viewToken.reset();
    m_view = nullptr;
    m_gridPending = false;
  }

  InputArea* TableEditSession::focusArea() const {
    if (m_view == nullptr) {
      return nullptr;
    }
    if (m_view->editingCell()) {
      return &m_view->canvas().editor().fieldArea();
    }
    return &m_view->canvas();
  }

  bool TableEditSession::key(const InputArea::KeyData& key) {
    if (m_view == nullptr) {
      return false;
    }
    if (m_view->offerKey(key)) {
      applyPendingGrid();
      return true;
    }
    if (key.pressed
        && !key.preedit
        && (key.modifiers & kModifiers) == 0
        && KeySymbol::isEscape(key.sym)
        && m_view->canvas().focused()) {
      m_events.escape();
      return true;
    }
    return false;
  }

  void TableEditSession::setShown(bool shown) {
    if (!shown) {
      commitField();
    }
    m_visible = shown;
  }

  void TableEditSession::update(float textSize) {
    if (m_view == nullptr) {
      return;
    }
    applyPendingGrid();
    if (textSize != m_textSize) {
      m_textSize = textSize;
      m_view->setTextSize(textSize);
    }
    if (m_visible) {
      m_view->update();
      applyPendingGrid();
    }
  }

  void TableEditSession::commitField() {
    if (m_view != nullptr) {
      m_view->commitField();
      applyPendingGrid();
    }
  }

  void TableEditSession::cellEdited(TableCell source, const std::string& text) {
    const auto before = m_session.revision();
    if (auto refused = refusal(m_session.setCell(source.row, source.column, text))) {
      m_events.refused(*refused);
      applyPendingGrid();
      return;
    }
    changed(before);
    applyPendingGrid();
  }

  void TableEditSession::request(const TableEditRequest& request) {
    const auto before = m_session.revision();
    if (auto refused = change(request)) {
      m_events.refused(*refused);
      applyPendingGrid();
      return;
    }
    changed(before);
    applyPendingGrid();
  }

  std::optional<std::string> TableEditSession::change(const TableEditRequest& request) {
    const auto [row, column] = request.source;
    const std::size_t rows = m_session.rowCount();
    const std::size_t columns = m_session.columnCount();
    switch (request.edit) {
    case TableEdit::InsertRowBelow:
      return fitsGrid(rows + 1, columns) ? refusal(m_session.insertRow(row + 1)) : std::string(kGridFull);
    case TableEdit::DeleteRow:
      return refusal(m_session.deleteRow(row));
    case TableEdit::InsertColumnRight:
      return fitsGrid(rows, columns + 1) ? refusal(m_session.insertColumn(column + 1)) : std::string(kGridFull);
    case TableEdit::DeleteColumn:
      return refusal(m_session.deleteColumn(column));
    case TableEdit::Undo:
      (void)m_session.undo();
      break;
    case TableEdit::Redo:
      (void)m_session.redo();
      break;
    }
    return std::nullopt;
  }

  void TableEditSession::changed(std::uint64_t before) {
    if (m_session.revision() == before) {
      return; // the change left the bytes as they were
    }
    rebuild();
    if (m_view != nullptr) {
      m_view->setEditAvailability(availability());
    }
    m_events.edited();
  }

  bool TableEditSession::fitsGrid(std::size_t rows, std::size_t columns) const {
    const std::size_t body = rows - std::min<std::size_t>(rows, m_session.dialect().header ? 1 : 0);
    return body <= formats::kMaxGridRows
        && columns <= formats::kMaxGridColumns
        && body * columns <= formats::kMaxGridCells;
  }

  void TableEditSession::rebuild() {
    const std::uint64_t build = ++m_builds;
    m_latestBuild->store(build, std::memory_order_relaxed);
    if (m_session.bytes().size() <= kInlineGridBytes) {
      showGrid(
          build, std::make_shared<const formats::TableGrid>(delimitedGrid(m_session.bytes(), m_session.dialect()))
      );
      return;
    }
    using Grid = std::shared_ptr<const formats::TableGrid>;
    runJob<Grid>(
        [bytes = m_session.bytes(), dialect = m_session.dialect(), latest = m_latestBuild, build] -> Grid {
          if (latest->load(std::memory_order_relaxed) != build) {
            return {};
          }
          return std::make_shared<const formats::TableGrid>(delimitedGrid(bytes, dialect));
        },
        [this, alive = std::weak_ptr<int>(m_alive), build](JobResult<Grid> grid) {
          if (alive.expired()) {
            return;
          }
          if (!grid) {
            m_events.refused(std::string(kGridLost));
            return;
          }
          if (!*grid) {
            return;
          }
          showGrid(build, std::move(*grid));
        }
    );
  }

  void TableEditSession::showGrid(std::uint64_t build, std::shared_ptr<const formats::TableGrid> grid) {
    if (build <= m_shown) {
      return; // a newer change's grid is shown already
    }
    m_shown = build;
    m_grid = std::move(grid);
    if (m_view == nullptr) {
      return;
    }
    if (m_view->editingCell()) {
      m_gridPending = true;
      return;
    }
    m_gridPending = false;
    m_view->replaceTable(m_grid);
  }

  void TableEditSession::applyPendingGrid() {
    if (m_gridPending && m_view != nullptr && !m_view->editingCell()) {
      m_gridPending = false;
      m_view->replaceTable(m_grid);
    }
  }

  std::string TableEditSession::wholeText(TableCell source) const {
    const auto cells = formats::rowCells(m_session.table(), source.row);
    if (source.column >= cells.size()) {
      return {};
    }
    return formats::cellText(m_session.bytes(), m_session.dialect(), cells[source.column], editing::kTableByteLimit)
        .text;
  }

  FileShape TableEditSession::shape(std::string_view /*bytes*/) const {
    const std::size_t header = m_session.dialect().header ? 1 : 0;
    return {
        .lines = std::nullopt,
        .cells = std::pair(m_session.rowCount() - std::min(m_session.rowCount(), header), m_session.columnCount())
    };
  }

  TableEditAvailability TableEditSession::availability() const {
    const std::size_t rows = m_session.rowCount();
    const std::size_t header = m_session.dialect().header ? 1 : 0;
    return {
        .undo = m_session.canUndo(),
        .redo = m_session.canRedo(),
        .deleteRow = rows > header + 1,
        .deleteColumn = m_session.columnCount() > 1,
    };
  }

  // The controller's table half: a shown file is read for editing as a table or as text, and a table's open cell
  // field commits before anything asks about changes.

  void PreviewEditController::offer(const DirectoryEntry* entry) {
    if ((entry == nullptr && !m_offer) || (entry != nullptr && m_offer && sameListing(m_offer->entry, *entry))) {
      return;
    }
    m_probe.cancel();
    if (m_opening) {
      m_opening->cancel();
      m_opening.reset();
    }
    m_offer.reset();
    if (entry != nullptr && !m_session) {
      m_offer = Offer{.entry = *entry, .checked = false, .reason = {}};
      const auto table = tableSource(entry->name, entry->contentType.value_or(""));
      if (table == TableSource::Xlsx || table == TableSource::Ods) {
        m_offer->checked = true;
        m_offer->reason = std::string(kSpreadsheetReadOnly);
      } else if (entry->size && *entry->size > editing::kEditByteLimit) {
        m_offer->checked = true;
        m_offer->reason = std::string(kTooLargeToEdit);
      } else {
        // The same read and checks as enabling, so the button says why before it is pressed.
        m_probe = CancelToken{};
        startEditableRead(*entry, m_probe, [this, alive = std::weak_ptr<int>(m_alive), token = m_probe](auto read) {
          if (alive.expired() || token.cancelled() || !m_offer) {
            return;
          }
          m_offer->checked = true;
          m_offer->reason = read ? std::string() : std::move(read.error());
          refresh();
          m_hooks.frameNeeded();
        });
      }
    }
    refresh();
  }

  void PreviewEditController::enable() {
    if (!m_offer || !m_offer->checked || !m_offer->reason.empty() || m_opening || m_session) {
      return;
    }
    m_opening = CancelToken{};
    startEditableRead(
        m_offer->entry, *m_opening, [this, alive = std::weak_ptr<int>(m_alive), token = *m_opening](auto read) {
          if (!alive.expired() && !token.cancelled()) {
            opened(std::move(read));
          }
        }
    );
    refresh();
  }

  void PreviewEditController::opened(std::expected<EditableTable, std::string> table) {
    if (refusedOpening(table ? nullptr : &table.error())) {
      return;
    }
    const auto identity = table->identity;
    auto session =
        std::make_unique<TableEditSession>(std::move(*table), [this](InputArea& area) { m_hooks.focus(area); });
    m_table = session.get();
    begin(std::move(session), identity, {});
  }

  void PreviewEditController::settle() {
    if (m_table != nullptr) {
      m_table->commitField();
    }
  }

  bool PreviewEditController::unsaved() {
    settle();
    return m_gate.editing() && m_gate.dirty();
  }
} // namespace luciole
