#include "app/page/workbench_session.h"
#include "browser/workbench_services.h"
#include "png_writer.h"
#include "scene_capture.h"
#include "status_tab_stand_in.h"
#include "temp_folder.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <glib.h>
#include <memory>
#include <print>
#include <string>

// At these window sizes the cursor row walked to the list's last shown row stays whole and above the status tab,
// which lies in the room the session keeps at the bottom of the file pane.
namespace {
  constexpr std::size_t kRows = 30;
  constexpr std::size_t kCursor = 20;
  constexpr float kRowHeight = 36; // as the page's list
  constexpr float kPageLeft = 218; // the sidebar
  constexpr float kPageTop = 95;   // the tabs and the toolbar

  struct Window {
    int width = 0;
    int height = 0;
    float scale = 1;
  };
  constexpr std::array<Window, 3> kWindows{{{1100, 700, 1}, {1400, 900, 1.5F}, {1400, 900, 2}}};

  // A list row: its name over the row's fill, the cursor row filled like a selection.
  class Rows final : public VirtualGridAdapter {
  public:
    std::size_t itemCount() const override { return kRows; }
    std::unique_ptr<Node> createTile() override {
      return ui::row(
          {.align = FlexAlign::Center,
           .paddingH = Style::spaceMd,
           .fill = colorSpecFromRole(ColorRole::Surface),
           .radius = Style::radiusMd},
          ui::label({.color = colorSpecFromRole(ColorRole::OnSurface), .maxLines = 1})
      );
    }
    void bindTile(Node& tile, std::size_t index, bool selected, bool) override {
      auto& row = static_cast<Flex&>(tile);
      row.setFill(colorSpecFromRole(selected ? ColorRole::SecondaryContainer : ColorRole::Surface));
      for (const auto& child : row.children()) { // the name, beside the row's own background
        if (auto* name = dynamic_cast<Label*>(child.get())) {
          (void)name->setText(std::format("file-{:02}.txt", index));
        }
      }
    }
  };

  // The page body: the Workbench view where the file pane was, then the status tab placed in the place the session
  // gives for it, as the page lays them out.
  class Body final : public Node {
  public:
    Body() {
      m_background = static_cast<Box*>(addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface)})));
      auto pane = ui::column(
          {.align = FlexAlign::Stretch,
           .gap = Style::spaceXs,
           .fill = colorSpecFromRole(ColorRole::Surface),
           .flexGrow = 1.0F},
          ui::virtualGridView(
              {.out = &m_list,
               .columns = 1,
               .cellHeight = kRowHeight,
               .squareCells = false,
               .columnGap = 0.0F,
               .rowGap = 2.0F,
               .adapter = &m_rows,
               .flexGrow = 1.0F}
          )
      );
      pane->setPadding(0, Style::spaceSm, Style::spaceSm, Style::spaceSm);
      m_list->scrollView().setReserveScrollbarGutter(true);
      m_pane = addChild(std::move(pane));
      addChild(buildStatusTab(m_tab));
      m_tab.tab->setVisible(true);
      (void)m_tab.status->setText("30 items · 1 selected · 49.3 KiB");
      (void)m_tab.freeSpace->setText("880.6 GiB free");
    }

    Node& pane() const noexcept { return *m_pane; }
    VirtualGridView& list() const noexcept { return *m_list; }
    const StatusTab& tab() const noexcept { return m_tab; }
    void setSession(luciole::WorkbenchSession* session) { m_session = session; }

  protected:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      setPosition(rect.x, rect.y);
      setSize(rect.width, rect.height);
      m_background->arrange(renderer, {0, 0, rect.width, rect.height});
      TEST_CHECK(m_session != nullptr);
      for (std::size_t round = 0; round < 2; ++round) { // the view plans its regions, then places them
        m_session->view().arrange(renderer, {0, 0, rect.width, rect.height});
      }
      const auto area = m_session->paneArea(*this);
      TEST_CHECK(area.has_value());
      // The room the tab asks for is the view's from the next arrange on.
      m_session->setTabRoom(arrangeStatusTab(renderer, m_tab, area.value_or(LayoutRect{}), true));
    }

  private:
    Rows m_rows;
    Box* m_background = nullptr; // the page body's fill
    Node* m_pane = nullptr;
    VirtualGridView* m_list = nullptr;
    StatusTab m_tab;
    luciole::WorkbenchSession* m_session = nullptr;
  };

  LayoutRect sceneBox(const Node& node) {
    LayoutRect rect{.width = node.width(), .height = node.height()};
    Node::absolutePosition(&node, rect.x, rect.y);
    return rect;
  }

  void keepForViewing(const ::test::GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  void checkWindow(::test::SceneCapture& capture, Body& body, Window window) {
    const auto width = static_cast<int>(std::lround((static_cast<float>(window.width) / window.scale) - kPageLeft));
    const auto height = static_cast<int>(std::lround((static_cast<float>(window.height) / window.scale) - kPageTop));
    auto& list = body.list();
    list.scrollView().setScrollOffset(0);
    list.setSelectedIndex(0, true);
    (void)capture.capture(body, width, height, window.scale);
    list.setSelectedIndex(kCursor, true); // the Down key from the top: the list scrolls only as far as it must
    const auto pixels = capture.capture(body, width, height, window.scale);
    TEST_CHECK(pixels.width == static_cast<int>(std::lround(static_cast<float>(width) * window.scale)));
    keepForViewing(pixels, std::format("workbench-list-tab-{}x{}-{}x.png", window.width, window.height, window.scale));
    const auto row = list.itemRect(kCursor);
    const auto tab = sceneBox(*body.tab().tab);
    TEST_CHECK(row.has_value());
    if (!row) {
      return;
    }
    std::println(
        "{}x{} at {}x: cursor row {:.0f}..{:.0f}, tab {:.0f}..{:.0f}, list ends at {:.0f}", window.width, window.height,
        window.scale, row->y, row->y + row->height, tab.y, tab.y + tab.height, sceneBox(list).y + list.height()
    );
    TEST_CHECK(list.scrollView().scrollOffset() > 0);
    TEST_NEAR(row->height, kRowHeight, 1e-3); // shown whole
    TEST_CHECK(row->y + row->height <= tab.y);
    TEST_CHECK(sceneBox(list).y + list.height() <= tab.y);
    // the tab keeps to the pane's place
    const auto pane = sceneBox(body.pane());
    TEST_CHECK(tab.x >= pane.x && tab.x + tab.width <= pane.x + pane.width);
    TEST_CHECK(tab.y + tab.height <= pane.y + pane.height);
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  luciole::test::TempFolder home{"luciole-workbench-list-tab"};
  for (const char* variable : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_DATA_DIRS"}) {
    g_setenv(variable, home.path().c_str(), TRUE);
  }
  setPalette(luciole::test::tiles::testPalette());
  luciole::WorkbenchServices services;
  luciole::FileIconCache icons;
  Body body;
  luciole::WorkbenchSession session(
      body.pane(), body.list().focusArea(), services.measureCache(), icons, [] { return 0U; },
      luciole::WorkbenchModelOptions{.ancestorLevels = 0}
  );
  session.keepTabRoom(body.list());
  session.update({}, {});
  body.setSession(&session);
  for (const auto window : kWindows) {
    checkWindow(*capture, body, window);
  }
  std::println("workbench list tab checks passed");
  return 0;
}
