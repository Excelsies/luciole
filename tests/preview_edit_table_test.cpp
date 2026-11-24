#include "app/page/preview_edit_table.h"
#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "cpu_budget.h"
#include "table_view_rig.h"
#include "temp_folder.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <glib.h>
#include <iterator>
#include <optional>
#include <print>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

// A delimited file read whole for editing and edited as a table, through real files in a private folder and a real
// table view driven by an input dispatcher.
namespace {
  namespace fs = std::filesystem;
  using namespace luciole;
  using luciole::test::tableview::center;
  using luciole::test::tableview::FontRenderer;
  using luciole::test::tableview::kHeight;
  using luciole::test::tableview::kWidth;
  using luciole::test::tableview::threadMs;

  // A frame of a 60 Hz display. Unoptimized code builds the grid about 15 times slower, the change about 10 times.
#ifdef __OPTIMIZE__
  constexpr double kChangeBudgetMs = 1000.0 / 60;
#else
  constexpr double kChangeBudgetMs = 4 * 1000.0 / 60;
#endif

  struct Folder {
    luciole::test::TempFolder temp{"luciole-edit-table-test"};

    fs::path write(const std::string& name, const std::string& bytes, mode_t mode = 0644) const {
      const auto path = temp.path() / name;
      TEST_CHECK((std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes).good());
      TEST_CHECK(::chmod(path.c_str(), mode) == 0);
      return path;
    }
  };

  std::expected<EditableTable, std::string> read(const fs::path& path, bool tabs = false) {
    return readEditableTable(Location::fromPath(path.native()), tabs, {});
  }

  std::string refusal(const fs::path& path) {
    const auto table = read(path);
    TEST_CHECK(!table.has_value());
    return table ? std::string() : table.error();
  }

  class NoClipboard final : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return std::nullopt; }
    void setClipboardText(std::string /*value*/) override {}
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    void requestClipboardText(std::function<void(std::optional<std::string>)> callback) override {
      callback(std::nullopt);
    }
  };

  // A session's view alone in a scene with a real input dispatcher, laid out as a page does it, and what the session
  // tells its controller.
  struct Editing {
    FontRenderer renderer;
    NoClipboard clipboard;
    Node root;
    InputDispatcher dispatcher;
    std::unique_ptr<TableEditSession> session;
    TableView* view = nullptr;
    int edits = 0;
    int saves = 0;
    int escapes = 0;
    int frames = 0;
    std::vector<std::string> refusals;
    std::uint32_t serial = 0;
    std::uint32_t clock = 0;

    explicit Editing(EditableTable table) {
      root.setSize(kWidth, kHeight);
      session =
          std::make_unique<TableEditSession>(std::move(table), [this](InputArea& area) { dispatcher.setFocus(&area); });
      auto made = session->makeView(
          clipboard,
          PreviewEditEvents{
              .edited = [this] { ++edits; },
              .saveRequested = [this] { ++saves; },
              .escape = [this] { ++escapes; },
              .refused = [this](const std::string& sentence) { refusals.push_back(sentence); },
              .frameNeeded = [this] { ++frames; },
              .modifiers = [] { return 0U; },
          }
      );
      view = dynamic_cast<TableView*>(made.get());
      TEST_CHECK(view != nullptr);
      root.addChild(std::move(made));
      dispatcher.setSceneRoot(&root);
      settle();
      dispatcher.setFocus(session->focusArea());
    }
    ~Editing() {
      dispatcher.setSceneRoot(nullptr);
      session->dropView();
    }
    Editing(const Editing&) = delete;
    Editing& operator=(const Editing&) = delete;

    void settle() {
      for (int pass = 0; pass < 8; ++pass) {
        frames = 0;
        session->update(TableView::kDefaultTextSize);
        view->arrange(renderer, {.x = 0, .y = 0, .width = kWidth, .height = kHeight});
        if (frames == 0) {
          return;
        }
      }
    }

    // A key as the page offers it: to the session first, then to the focused area.
    void key(std::uint32_t sym, std::uint32_t modifiers = 0, std::uint32_t utf32 = 0) {
      for (const bool pressed : {true, false}) {
        const InputArea::KeyData data{.sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = pressed};
        if (!session->key(data)) {
          dispatcher.keyEvent(sym, utf32, modifiers, pressed);
        }
      }
      settle();
    }

    void type(std::string_view text) {
      for (const char c : text) {
        const auto code = static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        key(code, 0, code);
      }
    }

    // Types `text` into the field of a grid cell, replacing its text, and commits it with Enter.
    void setCell(std::size_t row, std::size_t column, std::string_view text) {
      view->select({.row = row, .column = column});
      settle();
      type(text);
      TEST_CHECK(view->editingCell());
      key(XKB_KEY_Return);
      TEST_CHECK(!view->editingCell());
    }

    void press(TableEdit edit) {
      const auto at = center(view->editBar().button(edit));
      clock += 1000;
      dispatcher.pointerEnter(at.x, at.y, ++serial);
      dispatcher.pointerButton(at.x, at.y, BTN_LEFT, true, ++serial, clock, false);
      dispatcher.pointerButton(at.x, at.y, BTN_LEFT, false, ++serial, clock, false);
      settle();
    }

    std::string bytes() const { return session->fileBytes(); }
  };

  // At most 4000 rounds of the main context, until `done`.
  void until(const std::function<bool()>& done) {
    for (int round = 0; round < 4000 && !done(); ++round) {
      for (int i = 0; i < 64 && g_main_context_iteration(nullptr, FALSE) != FALSE; ++i) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(done());
  }

  // Comma, tab and semicolon files read whole, each with its dialect detected once and its grid.
  void testReadsDialects() {
    const Folder folder;
    struct Case {
      const char* name;
      std::string bytes;
      bool tabs;
      formats::Delimiter delimiter;
    };
    const std::vector<Case> cases{
        {"comma.csv", "name,size\nalpha,12\nbeta,7\n", false, formats::Delimiter::Comma},
        {"tabs.tsv", "name\tsize, in bytes\nalpha\t12\nbeta\t7\n", true, formats::Delimiter::Tab},
        {"semi.csv", "name;size\nalpha;12\nbeta;7\n", false, formats::Delimiter::Semicolon},
    };
    for (const auto& item : cases) {
      const auto table = read(folder.write(item.name, item.bytes), item.tabs);
      TEST_CHECK(table.has_value());
      if (!table) {
        std::println("{}: {}", item.name, table.error());
        continue;
      }
      TEST_CHECK(table->session.dialect().delimiter == item.delimiter);
      TEST_CHECK(table->session.bytes() == item.bytes && table->identity.size == item.bytes.size());
      TEST_CHECK(table->grid != nullptr && table->grid->rowCount == 2 && table->grid->columnCount == 2);
      TEST_CHECK(table->grid->firstSourceRow == 1 && formats::gridCellText(*table->grid, 1, 0) == "beta");
      TEST_CHECK(!table->session.modified());
    }
  }

  // Each reason a delimited file is not edited, as one sentence.
  void testRefusals() {
    const Folder folder;
    TEST_CHECK(refusal(folder.write("open.csv", "a,b\n\"never closed,1\n")).contains("never closed"));
    std::string tall = "n\n";
    for (std::size_t row = 0; row <= formats::kMaxGridRows; ++row) {
      tall += std::format("{}\n", row);
    }
    TEST_CHECK(refusal(folder.write("tall.csv", tall)).contains("more rows or columns"));
    std::string broad;
    for (std::size_t column = 0; column <= formats::kMaxGridColumns; ++column) {
      broad += std::format("{}c{}", column == 0 ? "" : ",", column);
    }
    TEST_CHECK(refusal(folder.write("broad.csv", broad + "\n" + broad + "\n")).contains("more rows or columns"));
    TEST_CHECK(refusal(folder.write("latin.csv", "name\ncaf\xE9\n")).contains("not UTF-8"));
    TEST_CHECK(
        refusal(folder.write("keys.csv", "k\n\"-----BEGIN OPENSSH PRIVATE KEY-----\"\n")).contains("private key")
    );
    TEST_CHECK(refusal(folder.write("big.csv", std::string(editing::kTableByteLimit + 1, 'a'))).contains("1 MiB"));
    if (::geteuid() != 0) {
      TEST_CHECK(refusal(folder.write("locked.csv", "a,b\n1,2\n", 0444)).contains("read-only"));
    }
    TEST_CHECK(refusal(folder.write("header.csv", "name,size\n")).contains("no rows to edit"));
    TEST_CHECK(refusal(folder.write("empty.csv", "")).contains("no rows to edit"));
    TEST_CHECK(refusal(folder.temp.path() / "gone.csv").starts_with("This file cannot be read"));
  }

  // Cells, rows and columns changed through the view, undone and redone; untouched bytes stay as they were.
  void testEditsThroughTheView() {
    const Folder folder;
    const std::string original = "\xEF\xBB\xBFname,size\r\nalpha,12\r\n\"beta, b\",7\r\n";
    auto table = read(folder.write("mark.csv", original));
    TEST_CHECK(table.has_value());
    Editing editing(std::move(*table));
    TEST_CHECK(editing.session->focusArea() == &editing.view->canvas());
    TEST_CHECK(!editing.view->editBar().available(TableEdit::Undo));

    editing.view->select({.row = 0, .column = 1});
    editing.settle();
    editing.key(XKB_KEY_F2);
    TEST_CHECK(editing.view->editingCell());
    TEST_CHECK(editing.session->focusArea() == &editing.view->canvas().editor().fieldArea());
    TEST_CHECK(editing.dispatcher.focusedArea() == editing.session->focusArea());
    TEST_CHECK(editing.view->canvas().editor().field().value() == "12");
    editing.key(XKB_KEY_Escape); // the field's own Escape cancels it
    TEST_CHECK(!editing.view->editingCell() && editing.escapes == 0 && editing.edits == 0);

    const std::string celled = "\xEF\xBB\xBFname,size\r\nalpha,9 kB\r\n\"beta, b\",7\r\n";
    editing.setCell(0, 1, "9 kB");
    TEST_CHECK(editing.bytes() == celled && editing.edits == 1 && editing.session->modified());
    TEST_CHECK(formats::gridCellText(*editing.view->table(), 0, 1) == "9 kB");
    TEST_CHECK(editing.view->editBar().available(TableEdit::Undo));
    editing.setCell(1, 0, "a,b"); // a cell that holds the delimiter is quoted
    const std::string quoted = "\xEF\xBB\xBFname,size\r\nalpha,9 kB\r\n\"a,b\",7\r\n";
    TEST_CHECK(editing.bytes() == quoted && editing.edits == 2);

    editing.view->select({.row = 0, .column = 0});
    editing.press(TableEdit::InsertRowBelow);
    const std::string rowed = "\xEF\xBB\xBFname,size\r\nalpha,9 kB\r\n,\r\n\"a,b\",7\r\n";
    TEST_CHECK(editing.bytes() == rowed && editing.edits == 3);
    TEST_CHECK(editing.view->table()->rowCount == 3);
    TEST_CHECK(editing.dispatcher.focusedArea() == &editing.view->canvas()); // the bar gives the keyboard back
    editing.view->select({.row = 1, .column = 0});
    editing.press(TableEdit::DeleteRow);
    TEST_CHECK(editing.bytes() == quoted && editing.view->table()->rowCount == 2);
    editing.press(TableEdit::InsertColumnRight);
    const std::string columned = "\xEF\xBB\xBFname,,size\r\nalpha,,9 kB\r\n\"a,b\",,7\r\n";
    TEST_CHECK(editing.bytes() == columned && editing.view->table()->columnCount == 3);
    editing.view->select({.row = 0, .column = 1});
    editing.press(TableEdit::DeleteColumn);
    TEST_CHECK(editing.bytes() == quoted && editing.edits == 6);

    for (const auto& before : {columned, quoted, rowed, quoted, celled, original}) {
      editing.key('z', KeyMod::Ctrl);
      TEST_CHECK(editing.bytes() == before);
    }
    TEST_CHECK(!editing.session->modified() && !editing.view->editBar().available(TableEdit::Undo));
    TEST_CHECK(editing.view->editBar().available(TableEdit::Redo));
    editing.key('y', KeyMod::Ctrl);
    TEST_CHECK(editing.bytes() == celled && editing.session->modified());
    editing.key('Z', KeyMod::Ctrl | KeyMod::Shift);
    TEST_CHECK(editing.bytes() == quoted);
    TEST_CHECK(formats::gridCellText(*editing.view->table(), 1, 0) == "a,b");
    TEST_CHECK(editing.edits == 14);

    // A save marks the revision written; a change after it is unsaved again.
    editing.session->saved(editing.session->revision());
    TEST_CHECK(!editing.session->modified());
    editing.key('s', KeyMod::Ctrl);
    TEST_CHECK(editing.saves == 1);
    editing.key(XKB_KEY_Escape);
    TEST_CHECK(editing.escapes == 1);
    TEST_CHECK(editing.refusals.empty());

    // A field left open commits when the session is asked to, as before a save or a leave.
    editing.view->select({.row = 0, .column = 0});
    editing.type("omega");
    TEST_CHECK(editing.view->editingCell());
    editing.session->commitField();
    TEST_CHECK(!editing.view->editingCell() && editing.session->modified());
    TEST_CHECK(editing.bytes() == "\xEF\xBB\xBFname,size\r\nomega,9 kB\r\n\"a,b\",7\r\n");
  }

  // Changes the session or the grid refuses: the sentence is told, and nothing changes.
  void testRefusedChanges() {
    const Folder folder;
    std::string full = "name,text\n";
    for (std::size_t row = 0; full.size() + 120 < editing::kTableByteLimit; ++row) {
      full += std::format("{},{}\n", row % 10, std::string(110, 'x'));
    }
    auto large = read(folder.write("full.csv", full));
    TEST_CHECK(large.has_value());
    Editing near(std::move(*large));
    near.press(TableEdit::InsertColumnRight);
    TEST_CHECK(near.refusals.size() == 1 && near.refusals.front().contains("larger than 1 MiB"));
    TEST_CHECK(near.bytes() == full && near.edits == 0 && !near.session->modified());

    std::string tall = "n\n";
    for (std::size_t row = 0; row < formats::kMaxGridRows; ++row) {
      tall += std::format("{}\n", row);
    }
    auto most = read(folder.write("most.csv", tall));
    TEST_CHECK(most.has_value());
    Editing rows(std::move(*most));
    rows.press(TableEdit::InsertRowBelow);
    TEST_CHECK(rows.refusals.size() == 1 && rows.refusals.front().contains("more rows"));
    TEST_CHECK(rows.bytes() == tall && rows.edits == 0);
    TEST_CHECK(!rows.view->editBar().available(TableEdit::Undo));
  }

  // One change to the cell at `row` and `column` of a table of `bytes`, its new grid included, measured in processor
  // time and held to a frame.
  void expectChangeWithinAFrame(const std::string& bytes, std::size_t row, std::size_t column) {
    const Folder folder;
    TEST_CHECK(bytes.size() > kInlineGridBytes - 4096 && bytes.size() <= kInlineGridBytes);
    auto table = read(folder.write("frame.csv", bytes));
    TEST_CHECK(table.has_value());
    Editing editing(std::move(*table));
    const auto* before = editing.view->table().get();
    editing.view->select({.row = row, .column = column});
    editing.settle();
    editing.type("renamed");
    const double start = threadMs();
    TEST_CHECK(editing.session->key({.sym = XKB_KEY_Return, .utf32 = 0, .modifiers = 0, .pressed = true}));
    const double spent = threadMs() - start;
    std::println(
        "a change to a table of {} bytes, {} columns, with its grid: {:.2f} ms of processor time", bytes.size(),
        editing.view->table()->columnCount, spent
    );
    TEST_CHECK(editing.view->table().get() != before); // shown at once
    TEST_CHECK(formats::gridCellText(*editing.view->table(), row, column) == "renamed");
    TEST_CHECK(TEST_WITHIN_BUDGET(spent, kChangeBudgetMs));
  }

  // The table of kInlineGridBytes with 4 columns.
  void testInlineGridFitsAFrame() {
    std::string bytes = "id,name,size,when\n";
    for (std::size_t row = 0; bytes.size() + 40 <= kInlineGridBytes; ++row) {
      bytes += std::format("{},file-{:05},{},2024-01-{:02}\n", row, row, row * 37 % 9973, (row % 28) + 1);
    }
    expectChangeWithinAFrame(bytes, 4000, 1);
  }

  // The same bytes as kMaxGridColumns columns, so a row is wide and the grid has few rows.
  void testWideInlineGridFitsAFrame() {
    const auto line = [](std::size_t row) {
      std::string text;
      for (std::size_t column = 0; column < formats::kMaxGridColumns; ++column) {
        text += (column == 0 ? "" : ",") + std::format("r{}c{}", row, column);
      }
      return text + "\n";
    };
    std::string bytes = line(0);
    for (std::size_t row = 1; bytes.size() + line(row).size() <= kInlineGridBytes; ++row) {
      bytes += line(row);
    }
    expectChangeWithinAFrame(bytes, 60, 200);
  }

  std::string largeCsvBytes() {
    std::string bytes = "id,name\n";
    for (std::size_t row = 0; bytes.size() <= kInlineGridBytes + 4096; ++row) {
      bytes += std::format("{},{}\n", row, std::string(40, 'n'));
    }
    return bytes;
  }

  // A larger table gets its grid from a worker: the change shows once it arrives, and grids arrive in order.
  void testLargerGridArrives() {
    const Folder folder;
    auto table = read(folder.write("large.csv", largeCsvBytes()));
    TEST_CHECK(table.has_value());
    Editing editing(std::move(*table));
    const auto rows = editing.view->table()->rowCount;
    const auto first = editing.view->table();
    editing.setCell(2, 1, "short");
    TEST_CHECK(editing.view->table() == first && editing.edits == 1); // the change is made; its grid comes later
    until([&] { return editing.view->table() != first; });
    TEST_CHECK(formats::gridCellText(*editing.view->table(), 2, 1) == "short");

    std::vector<std::size_t> seen;
    editing.view->select({.row = 0, .column = 0});
    for (int i = 0; i < 3; ++i) {
      editing.press(TableEdit::InsertRowBelow);
    }
    until([&] {
      seen.push_back(editing.view->table()->rowCount);
      return editing.view->table()->rowCount == rows + 3;
    });
    TEST_CHECK(std::ranges::is_sorted(seen));
    for (int i = 0; i < 50; ++i) {
      g_main_context_iteration(nullptr, FALSE);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(editing.view->table()->rowCount == rows + 3); // no older grid replaces the newest
    TEST_CHECK(editing.refusals.empty());
  }

  // A worker grid arriving while a cell field is open waits until the field closes instead of cancelling typed text.
  void testPendingGridWhileFieldOpen() {
    const Folder folder;
    auto table = read(folder.write("large-field.csv", largeCsvBytes()));
    TEST_CHECK(table.has_value());
    Editing editing(std::move(*table));
    const auto first = editing.view->table();

    editing.setCell(2, 1, "short");
    editing.view->select({.row = 5, .column = 1});
    editing.settle();
    editing.type("hello");
    TEST_CHECK(editing.view->editingCell());
    TEST_CHECK(editing.view->canvas().editor().field().value() == "hello");

    until([&] { return editing.session->gridPending(); });
    TEST_CHECK(editing.view->editingCell());
    TEST_CHECK(editing.view->canvas().editor().field().value() == "hello");
    TEST_CHECK(editing.view->table() == first);

    editing.type("abc");
    editing.key(XKB_KEY_Return);
    TEST_CHECK(!editing.view->editingCell());
    TEST_CHECK(!editing.session->gridPending());
    const auto middle = editing.view->table();
    TEST_CHECK(middle != first);
    TEST_CHECK(formats::gridCellText(*middle, 2, 1) == "short");

    until([&] { return editing.view->table() != middle; });
    TEST_CHECK(formats::gridCellText(*editing.view->table(), 2, 1) == "short");
    TEST_CHECK(formats::gridCellText(*editing.view->table(), 5, 1) == "helloabc");

    // Escape cancels the open field while a grid is pending and applies that pending grid once closed.
    const auto beforeEscape = editing.view->table();
    editing.setCell(3, 1, "pending-row");
    editing.view->select({.row = 6, .column = 1});
    editing.settle();
    editing.type("discard");
    TEST_CHECK(editing.view->editingCell());
    until([&] { return editing.session->gridPending(); });
    TEST_CHECK(editing.view->editingCell() && editing.view->table() == beforeEscape);

    editing.key(XKB_KEY_Escape);
    TEST_CHECK(!editing.view->editingCell());
    TEST_CHECK(!editing.session->gridPending());
    TEST_CHECK(editing.escapes == 0);
    TEST_CHECK(editing.view->table() != beforeEscape);
    TEST_CHECK(formats::gridCellText(*editing.view->table(), 3, 1) == "pending-row");
    TEST_CHECK(formats::gridCellText(*editing.view->table(), 6, 1) == std::string(40, 'n'));

    // Committing an unchanged field while a grid is pending applies the pending grid too.
    const auto beforeCommit = editing.view->table();
    editing.press(TableEdit::InsertRowBelow);
    editing.view->select({.row = 8, .column = 1});
    editing.settle();
    editing.key(XKB_KEY_F2);
    TEST_CHECK(editing.view->editingCell());
    until([&] { return editing.session->gridPending(); });
    editing.session->commitField();
    TEST_CHECK(!editing.view->editingCell());
    TEST_CHECK(!editing.session->gridPending());
    TEST_CHECK(editing.view->table() != beforeCommit);
    TEST_CHECK(editing.refusals.empty());
  }

  // Dropping a view after its node was already destroyed does not touch the view and keeps the selection for
  // makeView().
  void testDropViewAfterViewDestroyed() {
    const Folder folder;
    auto table = read(folder.write("drop.csv", "name,size\nalpha,12\nbeta,7\n"));
    TEST_CHECK(table.has_value());
    NoClipboard clipboard;
    int frames = 0;
    int edits = 0;
    int saves = 0;
    int focuses = 0;
    const PreviewEditEvents events{
        .edited = [&] { ++edits; },
        .saveRequested = [&] { ++saves; },
        .escape = [] {},
        .refused = [](const std::string&) {},
        .frameNeeded = [&] { ++frames; },
        .modifiers = [] { return 0U; },
    };
    TableEditSession session(std::move(*table), [&](InputArea&) { ++focuses; });

    auto first = session.makeView(clipboard, events);
    auto* firstView = dynamic_cast<TableView*>(first.get());
    TEST_CHECK(firstView != nullptr);
    firstView->select({.row = 1, .column = 1});
    TEST_CHECK((firstView->selectedSource() == TableCell{.row = 2, .column = 1}));

    first.reset();
    session.dropView();
    TEST_CHECK(session.focusArea() == nullptr);

    auto second = session.makeView(clipboard, events);
    auto* secondView = dynamic_cast<TableView*>(second.get());
    TEST_CHECK(secondView != nullptr);
    TEST_CHECK((secondView->selectedCell() == TableCell{.row = 1, .column = 1}));
    TEST_CHECK((secondView->selectedSource() == TableCell{.row = 2, .column = 1}));

    // A view outliving dropView() ignores late callbacks through its expired token.
    session.dropView();
    const int framesBefore = frames;
    secondView->onFrameNeeded();
    secondView->onSaveRequested();
    secondView->onSelectionChanged({.row = 1, .column = 0});
    secondView->onFocus(&secondView->canvas());
    TEST_CHECK(secondView->cellText({.row = 2, .column = 1}).empty());
    secondView->onCellEdited({.row = 2, .column = 1}, "ignored");
    secondView->onEditRequest({.edit = TableEdit::InsertRowBelow, .source = {.row = 2, .column = 1}});
    TEST_CHECK(frames == framesBefore && edits == 0 && saves == 0 && focuses == 0 && !session.modified());
    second.reset();

    auto third = session.makeView(clipboard, events);
    auto* thirdView = dynamic_cast<TableView*>(third.get());
    TEST_CHECK(thirdView != nullptr);
    TEST_CHECK((thirdView->selectedSource() == TableCell{.row = 2, .column = 1}));
    third.reset();
    session.dropView();
  }
} // namespace

int main() {
  testReadsDialects();
  testRefusals();
  testEditsThroughTheView();
  testRefusedChanges();
  testInlineGridFitsAFrame();
  testWideInlineGridFitsAFrame();
  testLargerGridArrives();
  testPendingGridWhileFieldOpen();
  testDropViewAfterViewDestroyed();
  std::println("preview-edit-table: all checks passed");
  return 0;
}
