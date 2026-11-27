#include "png_writer.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/preview_service.h"
#include "ui/browser/file_icon.h"
#include "ui/controls/button.h"
#include "ui/controls/file_preview_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/column_preview_page.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/status_badge.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// The page's own preview as a column slot shows it, laid out with the toolkit's text shaping and drawn offscreen:
// all its text at the one size its owner gives, below the list's, and nothing the slot's header already shows.
namespace {
  using luciole::ColumnPreviewPage;

  constexpr int kWidth = 260;
  constexpr int kHeight = 560;
  constexpr float kListTextSize = Style::fontSizeBody;

  luciole::DirectoryEntry pdfEntry() {
    luciole::DirectoryEntry entry{
        .id = 7,
        .place = luciole::Location::fromPath("/nowhere/page.pdf"),
        .name = "page.pdf",
        .size = 618,
        .modifiedSeconds = 1'700'000'000
    };
    entry.type = luciole::FileType::Regular;
    entry.contentType = "application/pdf";
    return entry;
  }

  // A blue picture 120 wide and 160 tall, as the page renders a PDF's first page.
  std::shared_ptr<const luciole::FileIconImage> bluePage() {
    auto pixels = std::make_shared<LoadedImageFile>();
    pixels->width = 120;
    pixels->height = 160;
    pixels->sourceWidth = 120;
    pixels->sourceHeight = 160;
    pixels->rgba.resize(static_cast<std::size_t>(120 * 160 * 4));
    for (std::size_t i = 0; i < pixels->rgba.size(); i += 4) {
      pixels->rgba[i] = 0;
      pixels->rgba[i + 1] = 0;
      pixels->rgba[i + 2] = 255;
      pixels->rgba[i + 3] = 255;
    }
    auto image = std::make_shared<luciole::FileIconImage>();
    image->pixels = std::move(pixels);
    image->ready = true;
    image->sourceKey = "page";
    return image;
  }

  luciole::PreviewSnapshot pdfSnapshot() {
    return {
        .entry = pdfEntry(),
        .status = luciole::PreviewStatus::Ready,
        .text = {},
        .message = {},
        .image = bluePage(),
        .imageUpgradeFailed = false,
        .metadata =
            {
                {.label = "Format", .value = "PDF 1.4"},
                {.label = "Pages", .value = "1"},
                {.label = "Producer", .value = "a writer", .preview = false},
            },
        .totals = {},
    };
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

  struct Shown {
    std::vector<const Label*> labels;
    std::vector<const Button*> buttons;
    const luciole::FileIcon* picture = nullptr;
    ::test::GlPixels pixels;

    // Every label, button and picture on screen.
    void collect(const Node& node) {
      if (!node.visible()) {
        return;
      }
      if (const auto* label = dynamic_cast<const Label*>(&node)) {
        labels.push_back(label);
      }
      if (const auto* button = dynamic_cast<const Button*>(&node)) {
        buttons.push_back(button);
      }
      if (const auto* icon = dynamic_cast<const luciole::FileIcon*>(&node)) {
        picture = icon;
      }
      for (const auto& child : node.children()) {
        collect(*child);
      }
    }

    const Label* badge = nullptr; // the status badge's label, when its badge shows
    const Flex* badgePill = nullptr;

    const Label* find(std::string_view text) const {
      for (const auto* label : labels) {
        if (label->text() == text) {
          return label;
        }
      }
      return nullptr;
    }
  };

  Shown show(::test::SceneCapture& capture, ColumnPreviewPage& page, float scale, std::string_view name) {
    Shown shown;
    shown.pixels = capture.capture(page, kWidth, kHeight, scale);
    TEST_CHECK(shown.pixels.width > 0);
    if (!name.empty()) {
      keepForViewing(shown.pixels, std::format("column-preview-page-{}-{}x.png", name, scale));
    }
    shown.collect(page);
    if (page.statusBadge().pill->visible()) {
      shown.badge = page.statusBadge().label;
      shown.badgePill = page.statusBadge().pill;
    }
    return shown;
  }

  // The size the slot's header draws at beside a list of the default size.
  float slotTextSize() { return luciole::quietTextSize(kListTextSize, 0); }

  void textBelowTheList(::test::SceneCapture& capture, luciole::FileIconCache& icons, float scale) {
    ColumnPreviewPage page(icons);
    page.setTextSize(slotTextSize());
    page.sync(pdfSnapshot(), kWidth, kHeight);
    const auto shown = show(capture, page, scale, "pdf");
    TEST_CHECK(!shown.labels.empty());
    for (const auto* label : shown.labels) {
      if (label->text().empty()) {
        continue;
      }
      TEST_NEAR(label->fontSize(), slotTextSize(), 0.001F);
      TEST_CHECK(label->fontSize() < kListTextSize);
    }
    // The facts the preview found, under a heading of the same size; facts meant for Properties stay out. The file's
    // place is the last, as in the side preview's Details.
    TEST_CHECK(shown.find("Details") != nullptr);
    TEST_CHECK(shown.find("Location") != nullptr && shown.find("/nowhere/page.pdf") != nullptr);
    TEST_CHECK(shown.badge == nullptr); // a file without a status shows no badge
    TEST_CHECK(shown.find("Pages") != nullptr && shown.find("1") != nullptr);
    TEST_CHECK(shown.find("Format") != nullptr && shown.find("PDF 1.4") != nullptr);
    TEST_CHECK(shown.find("Producer") == nullptr && shown.find("a writer") == nullptr);
    // The slot's header names the file and holds Open: neither repeats, and nothing closes the slot.
    TEST_CHECK(shown.find("page.pdf") == nullptr && shown.find("Preview") == nullptr);
    TEST_CHECK(shown.buttons.empty());
    // The picture is sized as the page asks for its pixels: the slot's width less the docked pane's margins and the
    // scrollbar, 260 - 24 - 3 - 8 = 225, in the page's shape, 225 * 160 / 120 tall.
    TEST_CHECK(shown.picture != nullptr);
    if (shown.picture != nullptr) {
      TEST_NEAR(shown.picture->width(), 225, 0.5F);
      TEST_NEAR(shown.picture->height(), 300, 0.5F);
    }
    // The view stays inside the slot it was given.
    TEST_NEAR(page.width(), kWidth, 0.01F);
    TEST_NEAR(page.height(), kHeight, 0.01F);
  }

  void followsTheTextSize(::test::SceneCapture& capture, luciole::FileIconCache& icons) {
    ColumnPreviewPage page(icons);
    page.setTextSize(12);
    for (const float invalid :
         {0.0F, -3.0F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
      page.setTextSize(invalid);
    }
    TEST_NEAR(page.textSize(), 12, 0.001F);
    auto snapshot = pdfSnapshot();
    snapshot.image = nullptr;
    snapshot.metadata.clear();
    snapshot.message = "Truncated";
    snapshot.text = "first line\nsecond line";
    page.sync(snapshot, kWidth, kHeight);
    const auto shown = show(capture, page, 1, "words");
    const auto* message = shown.find("Truncated");
    const auto* text = shown.find("first line\nsecond line");
    TEST_CHECK(message != nullptr && text != nullptr);
    TEST_NEAR(message->fontSize(), 12, 0.001F);
    TEST_NEAR(text->fontSize(), 12, 0.001F);
    // The status is the badge in the Details heading, not a line of its own; the file's place keeps the card.
    TEST_CHECK(shown.badge == message && shown.badgePill != nullptr);
    TEST_CHECK(shown.find("Details") != nullptr && shown.find("Location") != nullptr);
    TEST_CHECK(shown.find("Format") == nullptr);

    // A later size reaches every label.
    page.setTextSize(11);
    page.sync(pdfSnapshot(), kWidth, kHeight);
    const auto again = show(capture, page, 1, "");
    for (const auto* label : again.labels) {
      if (!label->text().empty()) {
        TEST_NEAR(label->fontSize(), 11, 0.001F);
      }
    }
    TEST_CHECK(again.find("Truncated") == nullptr && again.badge == nullptr);
  }

  // More facts than rows: the first ones show and the view does not grow past its slot.
  void manyFacts(::test::SceneCapture& capture, luciole::FileIconCache& icons) {
    ColumnPreviewPage page(icons);
    page.setTextSize(slotTextSize());
    auto snapshot = pdfSnapshot();
    snapshot.metadata.clear();
    for (std::size_t i = 0; i < ColumnPreviewPage::kMaxFacts + 8; ++i) {
      snapshot.metadata.push_back({.label = std::format("Fact {}", i), .value = std::format("value {}", i)});
    }
    page.sync(snapshot, kWidth, kHeight);
    const auto shown = show(capture, page, 1, "many");
    TEST_CHECK(shown.find("Fact 0") != nullptr);
    TEST_CHECK(shown.find(std::format("Fact {}", ColumnPreviewPage::kMaxFacts - 1)) != nullptr);
    TEST_CHECK(shown.find(std::format("Fact {}", ColumnPreviewPage::kMaxFacts)) == nullptr);
    TEST_NEAR(page.height(), kHeight, 0.01F);
  }

  // Each status is the badge in the Details heading, an error one in the error color; the heading keeps its room.
  void statusBadges(::test::SceneCapture& capture, luciole::FileIconCache& icons) {
    Palette colors = luciole::test::tiles::testPalette();
    setPalette(colors);
    ColumnPreviewPage page(icons);
    page.setTextSize(slotTextSize());
    for (const auto& [message, status, problem] :
         {std::tuple<std::string_view, luciole::PreviewStatus, bool>{
              "Remote file", luciole::PreviewStatus::Unavailable, false
          },
          {"No text preview", luciole::PreviewStatus::Unavailable, false},
          {"Not UTF-8 text", luciole::PreviewStatus::Unavailable, false},
          {"Low resolution", luciole::PreviewStatus::Ready, false},
          {"File changed", luciole::PreviewStatus::Unavailable, false},
          {"Empty", luciole::PreviewStatus::Ready, false},
          {"Read error", luciole::PreviewStatus::Error, true}}) {
      auto snapshot = pdfSnapshot();
      snapshot.message = std::string(message);
      snapshot.status = status;
      page.sync(snapshot, kWidth, kHeight);
      const auto shown = show(capture, page, 1, std::format("badge-{}", message));
      TEST_CHECK(shown.badge != nullptr && shown.badge->text() == message);
      const auto expected = problem ? colors.error : colors.onSurfaceVariant;
      TEST_NEAR(shown.badge->color().r, expected.r, 0.001F);
      TEST_NEAR(shown.badge->color().g, expected.g, 0.001F);
      // The pill is in the Details heading's row, right of the heading, inside the page.
      const auto* heading = shown.find("Details");
      TEST_CHECK(heading != nullptr);
      float hx = 0, hy = 0, bx = 0, by = 0;
      Node::absolutePosition(heading, hx, hy);
      Node::absolutePosition(shown.badgePill, bx, by);
      TEST_CHECK(bx >= hx + heading->width() - 0.5F && std::abs(by - hy) < 14);
      TEST_CHECK(bx + shown.badgePill->width() <= kWidth);
    }
  }

  // The badge mirrors the one the side preview builds inline: this fails when that one is restyled.
  void badgeMatchesTheSidePreview(::test::SceneCapture& capture, luciole::FileIconCache& icons) {
    setPalette(luciole::test::tiles::testPalette());
    FilePreviewView side(icons, false, [] {}, [] {}, [] {});
    auto snapshot = pdfSnapshot();
    snapshot.image = nullptr;
    snapshot.message = "Truncated";
    side.sync(snapshot, 300, 600);
    const auto sidePixels = capture.capture(side, 300, 600, 1);
    Shown sideShown;
    sideShown.collect(side);
    const auto* text = sideShown.find("Truncated");
    TEST_CHECK(text != nullptr && text->parent() != nullptr);
    const Node* pill = text->parent();
    float x = 0;
    float y = 0;
    Node::absolutePosition(pill, x, y);
    const int left = static_cast<int>(std::lround(x));
    const int top = static_cast<int>(std::lround(y));
    const int width = static_cast<int>(std::lround(pill->width()));
    const int height = static_cast<int>(std::lround(pill->height()));
    TEST_NEAR(pill->height(), luciole::kStatusBadgeHeight, 0.01F);
    TEST_NEAR(text->fontSize(), Style::fontSizeCaption, 0.001F);

    auto root = std::make_unique<Flex>();
    root->setDirection(FlexDirection::Vertical);
    root->setAlign(FlexAlign::Start);
    root->setPadding(static_cast<float>(top), 0, 0, static_cast<float>(left));
    root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    luciole::StatusBadge badge;
    root->addChild(luciole::makeStatusBadge(badge));
    luciole::showStatusBadge(badge, "Truncated", false, Style::fontSizeCaption);
    const auto mine = capture.capture(*root, 300, 600, 1);
    TEST_NEAR(badge.pill->width(), static_cast<float>(width), 1.0F);
    TEST_NEAR(badge.pill->height(), static_cast<float>(height), 0.5F);
    int differing = 0;
    for (int row = top; row < top + height; ++row) {
      for (int column = left; column < left + width; ++column) {
        const auto a = sidePixels.at(column, row);
        const auto b = mine.at(column, row);
        differing += std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > 6 ? 1 : 0;
      }
    }
    std::println("badge pixels differing from the side preview's: {}", differing);
    TEST_CHECK(differing == 0);
    keepForViewing(sidePixels, "badge-side-preview.png");
    keepForViewing(mine, "badge-workbench.png");
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
  luciole::FileIconCache icons;
  for (const float scale : {1.0F, 1.5F}) {
    textBelowTheList(*capture, icons, scale);
  }
  followsTheTextSize(*capture, icons);
  statusBadges(*capture, icons);
  badgeMatchesTheSidePreview(*capture, icons);
  manyFacts(*capture, icons);
  std::println("column-preview-page: all checks passed");
  return 0;
}
