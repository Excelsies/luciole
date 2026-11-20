#include "app/page/workbench_session.h"
#include "browser/browser_controller.h"
#include "model/details_columns.h"
#include "png_writer.h"
#include "scene_capture.h"
#include "text_clear.h"
#include "text_tile_view_rig.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/file_view.h"
#include "ui/browser/status_pill.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_header.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench/column_plan.h"
#include "workbench_view_rig.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The center at ordinary window sizes, laid out with the toolkit's text shaping over a real list in a file pane
// built as the page builds it: list columns are left out whole and never scroll sideways, the status pill lies in
// the list's own place, and header titles are never cut inside a word.
namespace {
  using namespace luciole;
  using namespace luciole::test::view;
  namespace wb = luciole::workbench;
  using ::test::GlPixels;

  // The page around the view: the sidebar on its left, the tabs and the toolbar above it, in logical px.
  constexpr float kPageLeft = 218;
  constexpr float kPageTop = 95;
  constexpr float kGutter = Style::scrollbarWidth + Style::scrollbarGap;
  constexpr std::string_view kPillText = "13 items · 1 selected · 692.3 KiB";
  constexpr std::string_view kFreeSpace = "881.3 GiB free";
  constexpr std::array<std::string_view, 4> kHeaderTexts{"Name", "Type", "Size", "Modified"};

  struct Window {
    int width = 0;
    int height = 0;
  };
  constexpr std::array<Window, 4> kWindows{{{1100, 900}, {1400, 900}, {1920, 1080}, {2560, 1440}}};

  struct Rect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float right() const { return x + width; }
    float bottom() const { return y + height; }
  };

  Rect box(const ::Node& node) {
    Rect rect{.width = node.width(), .height = node.height()};
    ::Node::absolutePosition(&node, rect.x, rect.y);
    return rect;
  }

  bool inside(const Rect& inner, const Rect& outer) {
    constexpr float kSlack = 0.01F;
    return inner.x >= outer.x - kSlack
        && inner.y >= outer.y - kSlack
        && inner.right() <= outer.right() + kSlack
        && inner.bottom() <= outer.bottom() + kSlack;
  }

  bool apart(const Rect& a, const Rect& b) {
    return a.right() <= b.x + 0.01F || b.right() <= a.x + 0.01F || a.bottom() <= b.y || b.bottom() <= a.y;
  }

  // A file pane as the page builds it: the column header over the list, both short of the scrollbar's gutter.
  struct FilePane {
    BrowserController browser{{}, false};
    ThumbnailService thumbnails;
    std::unique_ptr<FileViewAdapter> adapter;
    Flex* pane = nullptr;
    Flex* viewport = nullptr;
    VirtualGridView* grid = nullptr;
    std::array<Button*, 4> headers{}; // Name, Type, Size, Modified

    std::unique_ptr<Flex> build(FileIconCache& icons) {
      adapter = std::make_unique<FileViewAdapter>(
          browser, icons, thumbnails, ViewMode::List, [] { return 0U; }, [] { return false; }
      );
      auto made = std::make_unique<Flex>();
      pane = made.get();
      pane->setDirection(FlexDirection::Vertical);
      pane->setAlign(FlexAlign::Stretch);
      pane->setGap(Style::spaceXs);
      pane->setPadding(0, Style::spaceSm, Style::spaceSm, Style::spaceSm);
      pane->setFill(colorSpecFromRole(ColorRole::Surface)); // the page body's
      auto bar = std::make_unique<Flex>();
      bar->setDirection(FlexDirection::Horizontal);
      bar->setAlign(FlexAlign::Center);
      bar->setPadding(Style::radiusMd, 0);
      auto port = std::make_unique<Flex>();
      viewport = port.get();
      viewport->setDirection(FlexDirection::Horizontal);
      viewport->setAlign(FlexAlign::Center);
      viewport->setClipChildren(true);
      viewport->setFlexGrow(1);
      viewport->setMinHeight(32);
      for (std::size_t i = 0; i < headers.size(); ++i) {
        auto header = std::make_unique<Button>();
        header->setText(kHeaderTexts[i]);
        header->setControlHeight(Style::controlHeightSm);
        header->setContentAlign(ButtonContentAlign::Start);
        header->setVariant(ButtonVariant::Ghost);
        header->setPadding(0, Style::spaceSm);
        headers[i] = static_cast<Button*>(viewport->addChild(std::move(header)));
      }
      headers[0]->setGlyph("arrow-up"); // sorted by name
      bar->addChild(std::move(port));
      bar->addChild(ui::box({.width = kGutter}));
      pane->addChild(std::move(bar));
      pane->addChild(std::make_unique<Separator>());
      auto list = std::make_unique<VirtualGridView>();
      grid = list.get();
      grid->setColumns(1);
      grid->setCellHeight(36.0F);
      grid->setSquareCells(false);
      grid->setColumnGap(0);
      grid->setRowGap(2.0F);
      grid->setAdapter(adapter.get());
      grid->setFlexGrow(1.0F);
      grid->scrollView().setReserveScrollbarGutter(true);
      pane->addChild(std::move(list));
      return made;
    }

    // What the page does in Workbench once the pane has its width: the columns that fit, on the header and rows.
    DetailsColumns fit() {
      const auto viewportWidth = grid->scrollView().contentViewportWidth(true);
      const auto header = wb::listHeader(ViewMode::Workbench, viewportWidth, {}, kDefaultDetailsColumnWidths);
      TEST_CHECK(!header.resizable);
      const auto& columns = header.columns;
      for (std::size_t i = 0; i < headers.size(); ++i) {
        const bool shown = i == 0 || columns.visible(static_cast<DetailsColumn>(i - 1));
        headers[i]->setVisible(shown);
        headers[i]->setParticipatesInLayout(shown);
        const float allocation = i == 0 ? columns.nameWidth : static_cast<float>(kDefaultDetailsColumnWidths[i]);
        headers[i]->setMinWidth(allocation);
        headers[i]->setMaxWidth(allocation);
      }
      adapter->setDetailsColumns(columns);
      grid->setHorizontalOverflow(columns.contentWidth, 0);
      grid->notifyDataChanged();
      return columns;
    }

    // The pane's place: a change of folder slides the pane, not its place.
    Rect place() const {
      Rect rect{.width = pane->width(), .height = pane->height()};
      ::Node::absolutePosition(pane->parent(), rect.x, rect.y);
      rect.x += pane->x();
      rect.y += pane->y();
      return rect;
    }
  };

  // Natural text widths, laid out by the capture rig as the scene's labels are: a free label in a scene of its own.
  class Measure {
  public:
    Measure(::test::SceneCapture& capture, float scale) : m_capture(capture), m_scale(scale) {
      m_root.setDirection(FlexDirection::Horizontal);
      m_root.setAlign(FlexAlign::Start);
      m_label = static_cast<Label*>(m_root.addChild(std::make_unique<Label>()));
      m_label->setMaxLines(1);
    }

    float textScale() const { return m_capture.textScale(); }
    float width(std::string_view value, const Label& like) { return width(value, like.fontSize(), like.font().weight); }
    float width(std::string_view value, float size, FontWeight weight = FontWeight::Normal) {
      constexpr int kRoom = 4000;
      (void)m_label->setText(value);
      m_label->setFontSize(size);
      m_label->setFontWeight(weight);
      (void)m_capture.capture(m_root, kRoom, 2, m_scale);
      TEST_CHECK(m_label->width() < kRoom);
      return m_label->width();
    }

  private:
    ::test::SceneCapture& m_capture;
    float m_scale;
    Flex m_root;
    Label* m_label = nullptr;
  };

  // The page body: the view over all of it, then the status pill placed over the list's place, as the page lays
  // them out.
  class Body final : public ::Node {
  public:
    Body(std::unique_ptr<::Node> view, const FilePane& files) : m_files(files) {
      m_view = addChild(std::move(view));
      addChild(buildStatusPill(m_pill));
      m_pill.pill->setVisible(true);
      (void)m_pill.status->setText(kPillText);
      (void)m_pill.freeSpace->setText(kFreeSpace);
    }

    const StatusPill& pill() const noexcept { return m_pill; }
    std::unique_ptr<::Node> release() { return removeChild(m_view); }

  protected:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      setPosition(rect.x, rect.y);
      setSize(rect.width, rect.height);
      m_view->arrange(renderer, {0, 0, rect.width, rect.height});
      const auto place = m_files.place();
      arrangeStatusPill(renderer, m_pill, {place.x, place.y, place.width, place.height}, true);
    }

  private:
    const FilePane& m_files;
    ::Node* m_view = nullptr;
    StatusPill m_pill;
  };

  void keepForViewing(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  std::string tooltipOf(const Label& label) {
    const auto content = label.tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  // Every shown header and value fits its column, and the row content is no wider than the list's viewport.
  void expectColumnsFit(FilePane& files, const DetailsColumns& columns, Measure& measure) {
    const float viewport = files.grid->scrollView().contentViewportWidth(true);
    TEST_NEAR(files.viewport->width(), viewport, 0.5);
    TEST_CHECK(columns.contentWidth <= viewport + 0.5F);
    TEST_CHECK(columns.visible(DetailsColumn::Size) || !columns.visible(DetailsColumn::Modified));
    TEST_CHECK(columns.visible(DetailsColumn::Modified) || !columns.visible(DetailsColumn::Type));
    const auto port = box(*files.viewport);
    for (const auto* header : files.headers) {
      if (!shown(header)) {
        continue;
      }
      const auto* label = header->label();
      TEST_CHECK(box(*header).right() <= port.right() + 0.5F);
      TEST_CHECK(measure.width(label->text(), *label) <= label->width() + 0.5F);
    }
    const auto list = box(*files.grid);
    std::size_t values = 0;
    for (const auto* label : visible<Label>(*files.grid)) {
      if (label->text().empty()) {
        continue;
      }
      TEST_CHECK(box(*label).right() <= list.x + viewport + 0.5F);
      // The detail cells start where the name column ends.
      if (box(*label).x >= list.x + columns.nameWidth - 0.5F) {
        ++values;
        // Cells keep their pixel widths, as in the page's List view, so a larger system font may ellipsize a value.
        if (measure.textScale() == 1.0F && measure.width(label->text(), *label) > label->width() + 0.5F) {
          std::println("  cut value: '{}' in {:.1f} px", label->text(), label->width());
          TEST_CHECK(false);
        }
      }
    }
    TEST_CHECK(values > 0 || !columns.visible(DetailsColumn::Size));
  }

  // The pill as the page builds it, and each of its parts, lies inside the list's place and clear of every column
  // ahead and preview.
  void expectPillInside(const FilePane& files, const WorkbenchView& view, const Body& body) {
    const auto place = files.place();
    const auto& pill = body.pill();
    std::vector<const ::Node*> parts{pill.pill, pill.status};
    if (shown(pill.freeSpace) && pill.freeSpace->participatesInLayout()) {
      parts.push_back(pill.freeSpace);
    }
    std::println(
        "  pill {:.0f}..{:.0f} in list {:.0f}..{:.0f}, status {:.0f} px, free space {}", box(*pill.pill).x,
        box(*pill.pill).right(), place.x, place.right(), pill.status->width(), parts.size() > 2 ? "shown" : "hidden"
    );
    for (const auto* part : parts) {
      const auto rect = box(*part);
      TEST_CHECK(inside(rect, place));
      for (std::size_t slot = 0; slot < wb::kColumnsAhead; ++slot) {
        const auto* column = view.center().column(slot);
        if (column != nullptr && shown(column)) {
          TEST_CHECK(apart(rect, box(*column)));
        }
      }
    }
  }

  // Each header shows the longest of its title with the reason, the title, and the pane's name that fits whole;
  // only the name may be cut.
  void expectTitlesFit(const WorkbenchView& view, Measure& measure) {
    constexpr std::array<std::string_view, 3> kNames{"Instrument", "Jump ahead", "Ribbon"};
    for (const auto pane : {wb::Pane::Instrument, wb::Pane::Jump, wb::Pane::Ribbon}) {
      const auto& label = *view.shell().header(pane).titleLabel();
      if (!shown(&label)) {
        continue;
      }
      const std::string text = label.text();
      const std::string whole = label.hasTooltip() ? tooltipOf(label) : text;
      const auto reason = whole.find(" (closed: ");
      const std::string bare = whole.substr(0, reason);
      const auto name = kNames[static_cast<std::size_t>(pane)];
      const float room = label.width();
      std::println(
          "  {}: '{}' in {:.1f} px; whole {:.1f}, title {:.1f}", name, text, room, measure.width(whole, label),
          measure.width(bare, label)
      );
      TEST_CHECK(text == whole || text == bare || text == name);
      if (text != name) {
        TEST_CHECK(measure.width(text, label) <= room + 0.5F);
      }
      if (text != whole) {
        TEST_CHECK(measure.width(whole, label) > room);
      }
      if (text == name && bare != name) {
        TEST_CHECK(measure.width(bare, label) > room);
      }
    }
  }

  void checkWindow(Rig& rig, FilePane& files, Body& body, ::test::SceneCapture& capture, Window window, float scale) {
    const auto width = static_cast<int>(std::lround((static_cast<float>(window.width) / scale) - kPageLeft));
    const auto height = static_cast<int>(std::lround((static_cast<float>(window.height) / scale) - kPageTop));
    std::println("{}x{} at {}x: view {}x{}", window.width, window.height, scale, width, height);
    Measure measure(capture, scale);
    rig.view->update();
    (void)capture.capture(body, width, height, scale);
    const auto columns = files.fit();
    rig.view->update();
    const auto pixels = capture.capture(body, width, height, scale);
    TEST_CHECK(pixels.width == static_cast<int>(std::lround(static_cast<float>(width) * scale)));
    keepForViewing(pixels, std::format("workbench-center-{}x{}-{}x.png", window.width, window.height, scale));
    std::println(
        "  list {:.0f} px, viewport {:.0f}, content {:.0f}: type {} size {} modified {}", files.pane->width(),
        files.grid->scrollView().contentViewportWidth(true), columns.contentWidth, columns.visible(DetailsColumn::Type),
        columns.visible(DetailsColumn::Size), columns.visible(DetailsColumn::Modified)
    );
    expectColumnsFit(files, columns, measure);
    expectPillInside(files, *rig.view, body);
    expectTitlesFit(*rig.view, measure);
    ::test::expectTextClear(body, measure.textScale());
  }

  // The row under a point of the list, as the view's hover reads it: a folder, a file, and none below the rows.
  void expectListEntries(const FilePane& files, const fs::path& folder) {
    const auto& model = files.browser.model();
    const auto at = [&](const std::string& name) -> std::optional<WorkbenchEntry> {
      for (const auto* label : visible<Label>(*files.grid)) {
        if (label->text() == name) {
          const auto point = middle(*label);
          return listEntryAt(model, *files.grid, point.x, point.y);
        }
      }
      TEST_CHECK(false);
      return std::nullopt;
    };
    const auto music = at("Music");
    TEST_CHECK(music && music->location == Location::fromPath((folder / "Music").native()) && music->folder);
    const auto notes = at("notes.txt");
    TEST_CHECK(notes && notes->location == Location::fromPath((folder / "notes.txt").native()) && !notes->folder);
    const auto list = box(*files.grid);
    TEST_CHECK(!listEntryAt(model, *files.grid, list.x + 40, list.bottom() - 4));
  }

  // Ten folders deep, with files whose sizes and names are those of an ordinary folder.
  fs::path makeFolder(const Fixture& fixture) {
    auto folder = fixture.root;
    for (const char* level : {"home", "user", "Projects", "app", ".cache", "tmp", "native", "harness", "fixture"}) {
      folder /= level;
    }
    Fixture::write(folder / "Documents" / "letter.txt", 20);
    Fixture::write(folder / "Music" / "song.ogg", 4000);
    Fixture::write(folder / "Pictures" / "wide.png", 9800);
    Fixture::write(folder / "DejaVuSans-Bold.ttf", 708920);
    Fixture::write(folder / "config.json", 107);
    Fixture::write(folder / "notes.txt", 31);
    Fixture::write(folder / "README.md", 67);
    return folder;
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  Rig rig;
  rig.view->setPalette(workbenchPalette(palette));
  const auto folder = makeFolder(rig.fixture);
  FilePane files;
  auto pane = files.build(rig.icons);
  (void)rig.view->setList(std::move(pane), files.grid->focusArea());
  files.browser.open(Location::fromPath(folder.native()));
  rig.open(fs::relative(folder, rig.fixture.root));
  rig.until([&] { return rig.loaded() && files.browser.model().snapshot().status == DirectoryStatus::Ready; });
  TEST_CHECK(rig.view->center().ancestors().size() >= 10);
  (void)rig.view->setPreview(std::make_unique<::Node>()); // the page's preview slot
  rig.view->setCursor(WorkbenchEntry{.location = Location::fromPath((folder / "notes.txt").native()), .folder = false});
  Body body(rig.root.removeChild(rig.view), files);
  // The system font at 16 pt is 21.33 px against the 14 px the toolkit's text sizes are written for.
  for (const float textScale : {1.0F, 16.0F * 96.0F / 72.0F / 14.0F}) {
    capture->setTextScale(textScale);
    std::println("text scale {:.2f}", textScale);
    for (const float scale : {1.0F, 1.5F}) {
      for (const auto window : kWindows) {
        checkWindow(rig, files, body, *capture, window, scale);
      }
    }
  }
  capture->setTextScale(1.0F);
  expectListEntries(files, folder);
  rig.root.addChild(body.release()); // the rig lets go of the view's sources
  std::println("workbench-center-fit: all checks passed");
  return 0;
}
