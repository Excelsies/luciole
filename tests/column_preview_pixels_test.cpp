#include "app/page/workbench_preview.h"
#include "column_preview_fixture.h"
#include "column_preview_rig.h"
#include "config/settings.h"
#include "imaging/text_tile_service.h"
#include "png_writer.h"
#include "render/animation/animation_manager.h"
#include "render/scene/image_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/sheet_preview.h"
#include "thumbnails/thumbnail_service.h"
#include "thumbnails/thumbnail_worker.h"
#include "ui/controls/animated_image_view.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/mesh_view.h"
#include "ui/controls/table_view.h"
#include "ui/controls/text_tile_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/list_rows.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <glib.h>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The whole column preview, header, facts and content, drawn offscreen with real text for one file of each kind
// the preview draws itself, at 1x and 1.5x, and at the preview sizes 70, 100, 150 and 300 percent.
namespace {
  using luciole::ColumnPreviewKind;
  using luciole::ColumnPreviewStatus;
  using luciole::WorkbenchPreview;
  using luciole::test::preview::Rig;
  using luciole::test::tiles::bytesOf;
  using luciole::test::tiles::distance;
  using Clock = std::chrono::steady_clock;
  using Pixel = std::array<std::uint8_t, 4>;

  constexpr int kWidth = 460;
  constexpr int kHeight = 420;

  void keep(const ::test::GlPixels& pixels, std::string_view name, float scale) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("column-preview-{}-{}x.png", name, scale);
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // Pixels that differ from the view's background in rows [top, bottom) and columns [left, right).
  std::size_t inkIn(const ::test::GlPixels& pixels, int left, int right, int top, int bottom) {
    const auto background = bytesOf(palette.surfaceContainerLow);
    std::size_t count = 0;
    for (int y = std::max(0, top); y < std::min(bottom, pixels.height); ++y) {
      for (int x = std::max(0, left); x < std::min(right, pixels.width); ++x) {
        count += distance(pixels.at(x, y), background) > 60 ? 1 : 0;
      }
    }
    return count;
  }

  // Pixels within a small distance of `color` in rows [top, bottom).
  std::size_t colored(const ::test::GlPixels& pixels, const Pixel& color, int top, int bottom) {
    std::size_t count = 0;
    for (int y = std::max(0, top); y < std::min(bottom, pixels.height); ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        count += distance(pixels.at(x, y), color) < 24 ? 1 : 0;
      }
    }
    return count;
  }

  // The content's top edge in buffer pixels.
  int contentTop(const luciole::ColumnPreviewView& view, float scale) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(view.content(), x, y);
    return static_cast<int>(std::ceil(y * scale));
  }

  // Every label below `node` uses text smaller than the list's, and so do the text and table views. The rule holds at
  // the default preview size; larger sizes are the user's choice.
  void textBelowTheList(const Node& node, float list) {
    if (const auto* label = dynamic_cast<const Label*>(&node)) {
      TEST_CHECK(label->fontSize() < list);
    }
    if (const auto* table = dynamic_cast<const luciole::TableView*>(&node)) {
      TEST_CHECK(table->textSize() < list);
    }
    for (const auto& child : node.children()) {
      textBelowTheList(*child, list);
    }
  }

  void capturesEachKind(Rig& rig) {
    struct Case {
      std::string_view file;
      std::string_view name;
      ColumnPreviewKind kind;
    };
    constexpr std::array kCases{
        Case{"style.css", "css", ColumnPreviewKind::Text},       Case{"notes.md", "markdown", ColumnPreviewKind::Text},
        Case{"data.csv", "csv", ColumnPreviewKind::Table},       Case{"book.xlsx", "xlsx", ColumnPreviewKind::Table},
        Case{"spin.gif", "gif", ColumnPreviewKind::Animation},   Case{"cube.stl", "model", ColumnPreviewKind::Mesh},
        Case{"pack.zip", "archive", ColumnPreviewKind::Archive}, Case{"blob.bin", "binary", ColumnPreviewKind::Binary},
    };
    for (const float scale : {1.0F, 1.5F}) {
      for (const auto& item : kCases) {
        rig.show(item.file);
        const auto pixels = rig.settle(kWidth, kHeight, scale);
        keep(pixels, item.name, scale);
        TEST_CHECK(rig.preview->kind() == item.kind);
        TEST_CHECK(pixels.width == static_cast<int>(std::lround(kWidth * scale)));
        const int top = contentTop(*rig.view, scale);
        TEST_CHECK(top > 0 && top < pixels.height / 3);
        TEST_CHECK(inkIn(pixels, 0, pixels.width, 0, top) > 200); // name, kind, facts and Open
        const auto content = inkIn(pixels, 0, pixels.width, top, pixels.height);
        std::println("{} at {}x: content from row {}, {} pixels of ink", item.name, scale, top, content);
        TEST_CHECK(content > 300);
        TEST_CHECK(rig.preview->previewSize() == luciole::kDefaultPreviewSize);
        textBelowTheList(*rig.view, Style::fontSizeBody);
        if (item.file == "style.css") { // CSS colors carry swatches of their own color
          TEST_CHECK(colored(pixels, {53, 132, 228, 255}, top, pixels.height) > 20);
          TEST_CHECK(colored(pixels, {145, 65, 172, 255}, top, pixels.height) > 20);
        }
        if (item.kind == ColumnPreviewKind::Binary) { // the hex rows fit the slot: nothing scrolls sideways
          const auto* hex = dynamic_cast<const TextTileView*>(rig.view->content());
          TEST_CHECK(hex != nullptr && hex->maxScrollX() == 0 && !hex->horizontalBar().Node::visible());
        }
        if (item.kind == ColumnPreviewKind::Animation) {
          const auto frame = pixels.at(pixels.width / 2, top + (pixels.height - top) / 2);
          const bool flat = distance(frame, {224, 27, 36, 255}) < 24
              || distance(frame, {53, 132, 228, 255}) < 24
              || distance(frame, {46, 194, 126, 255}) < 24;
          TEST_CHECK(flat);
        }
      }
    }
  }

  // Hidden, the preview costs nothing: the playing image goes with its texture and its timer, and the slot shown
  // again brings back the same file, drawn as before.
  void hiddenCostsNothing(Rig& rig) {
    rig.show("spin.gif");
    (void)rig.settle(kWidth, kHeight, 1);
    TEST_CHECK(dynamic_cast<const AnimatedImageView*>(rig.view->content())->playing());
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() >= 0); // its next frame is due
    rig.preview->hide();
    for (int round = 0; round < 20; ++round) {
      (void)rig.frame(kWidth, kHeight, 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_CHECK(rig.view->content() == nullptr && rig.preview->load() == nullptr);
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() == -1); // nothing is left to wake the main loop
    rig.show("style.css");
    const auto before = rig.settle(kWidth, kHeight, 1);
    rig.preview->hide();
    TEST_CHECK(rig.view->content() == nullptr && rig.view->nameLabel().text() == "style.css");
    rig.show("style.css"); // what the page's next frame does once it is shown again
    const auto after = rig.settle(kWidth, kHeight, 1);
    keep(after, "shown-again", 1);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < before.rgba.size() && i < after.rgba.size(); ++i) {
      differing += before.rgba[i] != after.rgba[i] ? 1 : 0;
    }
    TEST_CHECK(before.rgba.size() == after.rgba.size() && differing == 0);
  }

  // Waits until the folder lists the file.
  void waitForRow(Rig& rig, std::string_view name) {
    const auto location = luciole::Location::fromPath((rig.files.folder() / name).native());
    for (int round = 0; round < 4000 && !luciole::rowOf(rig.browser.model(), location); ++round) {
      Rig::pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(luciole::rowOf(rig.browser.model(), location).has_value());
  }

  void addFile(Rig& rig, std::string_view name, std::string_view bytes) {
    luciole::test::preview::writeFile(rig.files.folder() / name, bytes);
    waitForRow(rig, name);
  }

  // An archive lists and counts its own members, not a folder its paths imply.
  void archiveListsItsMembers(Rig& rig) {
    luciole::test::table::writeZip(
        rig.files.folder() / "loose.zip",
        {{"docs/", ""},
         {"docs/readme.txt", "hello archive\n"},
         {"src/main.cpp", std::string(5000, 'c')},
         {"data.bin", std::string(7000, 'd')}}
    );
    waitForRow(rig, "loose.zip");
    rig.show("loose.zip");
    const auto pixels = rig.settle(kWidth, kHeight, 1);
    keep(pixels, "archive-members", 1);
    const auto* archive = dynamic_cast<const luciole::TableView*>(rig.view->content());
    TEST_CHECK(archive != nullptr && archive->table() && archive->table()->rowCount == 4);
    TEST_CHECK(luciole::formats::gridCellText(*archive->table(), 0, 0) == "docs/");
    TEST_CHECK(luciole::formats::gridCellText(*archive->table(), 2, 0) == "src/main.cpp");
    TEST_CHECK(rig.view->factsLabel().text().contains("4 entries"));
  }

  // Files no reader takes say why in a sentence of the previews' own voice, never in a reader's raw words.
  void failuresAreSentences(Rig& rig) {
    std::string claims(84, ' '); // a binary STL that claims a billion triangles
    const std::uint32_t billion = 1'000'000'000;
    std::memcpy(claims.data() + 80, &billion, 4);
    addFile(rig, "claims.stl", claims + std::string(100, '\0'));
    addFile(rig, "broken.xlsx", "PK\x03\x04 not a workbook at all");
    addFile(rig, "broken.zip", "PK\x03\x04" + std::string(300, '\x7F'));
    const auto failure = [&](std::string_view name) {
      rig.show(name);
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      ::test::GlPixels pixels;
      while (Clock::now() < deadline
             && (!rig.preview->load() || rig.preview->load()->status() != ColumnPreviewStatus::Failed)) {
        pixels = rig.frame(kWidth, kHeight, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      pixels = rig.frame(kWidth, kHeight, 1);
      keep(pixels, std::format("failed-{}", name), 1);
      const auto text = rig.view->message();
      std::println("{}: {}", name, text);
      TEST_CHECK(text.starts_with("This ") && text.ends_with('.'));
      TEST_CHECK(rig.view->content() == nullptr); // the sentence stands in place of the content
      // The header's badge names it in the words of the side preview's badge.
      TEST_CHECK(rig.view->statusBadge().pill->visible());
      TEST_CHECK(rig.view->statusBadge().label->text() == "Read error");
      return text;
    };
    TEST_CHECK(failure("claims.stl") == "This file is damaged or is not an STL model.");
    TEST_CHECK(failure("broken.xlsx") == "This spreadsheet could not be read.");
    TEST_CHECK(failure("broken.zip").starts_with("This archive"));
  }

  // The badge beside the kind: "Empty" for an empty file, "Truncated" for text cut at its line limit, "Not UTF-8
  // text" for hex, none for text shown whole; the sentence the content says stays.
  void badgesNameTheState(Rig& rig) {
    std::string many;
    for (int line = 0; line < 2600; ++line) {
      many += std::format("line {}\n", line);
    }
    addFile(rig, "empty.txt", "");
    addFile(rig, "many-lines.txt", many);
    const auto badge = [&](std::string_view name, std::string_view expected) {
      rig.show(name);
      const auto pixels = rig.settle(kWidth, kHeight, 1);
      keep(pixels, std::format("badge-{}", name), 1);
      TEST_CHECK(rig.view->statusBadge().pill->visible() == !expected.empty());
      TEST_CHECK(rig.view->statusBadge().label->text() == expected);
      if (!expected.empty()) { // the badge sits at the right end of the kind's line, inside the view
        float x = 0;
        float y = 0;
        Node::absolutePosition(rig.view->statusBadge().pill, x, y);
        TEST_CHECK(x > rig.view->kindLabel().x() && x + rig.view->statusBadge().pill->width() <= kWidth + 0.01F);
      }
    };
    badge("notes.md", "");
    badge("empty.txt", "Empty");
    badge("many-lines.txt", "Truncated");
    badge("blob.bin", "Not UTF-8 text");
    badge("notes.md", ""); // the next file drops it
  }

  // Text, a table and an archive's listing sit on a page card like the side preview's: inset from the pane by 8
  // pixels, with an outline and rounded corners. A refused archive's sentence is plain centered text.
  void pagesSitOnACard(Rig& rig) {
    const auto surface = bytesOf(palette.surfaceContainerLow);
    const auto outline = bytesOf(palette.outlineVariant);
    for (const auto* file : {"style.css", "data.csv", "pack.zip"}) {
      rig.show(file);
      const auto pixels = rig.settle(kWidth, kHeight, 1);
      const Node* card = rig.view->content()->parent();
      TEST_CHECK(card != nullptr && card->parent() != nullptr);
      float x = 0;
      float y = 0;
      Node::absolutePosition(card, x, y);
      TEST_NEAR(x, Style::spaceSm, 0.01F);
      TEST_NEAR(card->width(), kWidth - (2 * Style::spaceSm), 0.01F);
      const int top = static_cast<int>(y);
      // The outline runs along the card's top edge; the corner pixel is the pane, rounded away.
      const auto edge = pixels.at(kWidth / 2, top);
      TEST_CHECK(distance(edge, outline) < distance(edge, surface));
      TEST_CHECK(distance(pixels.at(static_cast<int>(x), top), surface) < 24);
      TEST_CHECK(distance(pixels.at(static_cast<int>(x) + 40, top - 4), surface) == 0);
    }
  }

  // At the default preview size the preview's text stays below the list's at narrow, middle and wide columns, and in
  // later slots.
  void textAtEveryWidth(Rig& rig) {
    TEST_CHECK(rig.preview->previewSize() == luciole::kDefaultPreviewSize);
    rig.show("book.xlsx");
    for (const int width : {220, 460, 960}) {
      (void)rig.settle(width, kHeight, 1);
      textBelowTheList(*rig.view, Style::fontSizeBody);
      TEST_NEAR(rig.view->textSize(), luciole::quietTextSize(Style::fontSizeBody, 0), 0.0F);
    }
    for (const float list : {10.0F, 14.0F, 22.0F}) {
      rig.preview->setListTextSize(list);
      (void)rig.settle(kWidth, kHeight, 1);
      textBelowTheList(*rig.view, list);
    }
    rig.preview->setListTextSize(Style::fontSizeBody);
  }

  // A text file, a table, a picture and a model at each preview size: the content follows the size and the header
  // does not. The picture is 200 by 120: fitted and never enlarged at 100%, so a wide slot shows it at its own size.
  void capturesEachSize(Rig& rig) {
    TEST_CHECK(::test::writeBytes(rig.files.folder() / "small.png", ::test::stillPng(200, 120, {46, 194, 126, 255})));
    waitForRow(rig, "small.png");
    const float header = luciole::quietTextSize(Style::fontSizeBody, 0);
    for (const auto* file : {"style.css", "data.csv", "small.png", "cube.stl"}) {
      rig.show(file);
      for (const int percent : {70, 100, 150, 300}) {
        rig.preview->setPreviewSize(percent);
        const auto pixels = rig.settle(kWidth, kHeight, 1);
        keep(pixels, std::format("{}-{}", file, percent), 1);
        const float factor = static_cast<float>(percent) / 100;
        TEST_NEAR(rig.view->textSize(), header, 0.0F);
        TEST_CHECK(rig.view->sizeButton().label()->text() == std::format("{}%", percent));
        if (const auto* tiles = dynamic_cast<const TextTileView*>(rig.view->content())) {
          TEST_NEAR(tiles->textSize(), header * factor, 1e-4F);
        } else if (const auto* table = dynamic_cast<const luciole::TableView*>(rig.view->content())) {
          TEST_NEAR(table->textSize(), header * factor, 1e-4F);
        } else if (const auto* picture = dynamic_cast<const AnimatedImageView*>(rig.view->content())) {
          TEST_NEAR(picture->image().width(), std::min(200.0F, picture->width()) * factor, 0.01F);
        } else {
          const auto* mesh = dynamic_cast<const MeshView*>(rig.view->content());
          TEST_CHECK(mesh != nullptr && mesh->previewScale() == factor);
        }
      }
    }
    // A wide slot shows the picture at its own 200 by 120, never enlarged.
    rig.preview->setPreviewSize(100);
    rig.show("small.png");
    const auto wide = rig.settle(960, kHeight, 1);
    keep(wide, "small.png-wide", 1);
    const auto* picture = dynamic_cast<const AnimatedImageView*>(rig.view->content());
    TEST_CHECK(picture != nullptr && picture->width() > 900);
    TEST_NEAR(picture->image().width(), 200, 0.01F);
    TEST_NEAR(picture->image().height(), 120, 0.01F);
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--mesh-preview-worker") {
    return luciole::runMeshPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(std::span(args).subspan(1));
  }
  if (!args.empty()) {
    return 2; // never run the checks again from a helper call this test does not serve
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  g_setenv("TZ", "UTC", TRUE);
  setPalette(luciole::test::tiles::testPalette());
  // Static, so a failed check, which exits, still removes the folder.
  static const luciole::test::preview::PreviewFiles files;
  {
    Rig rig(*capture, files);
    capturesEachKind(rig);
    textAtEveryWidth(rig);
    capturesEachSize(rig);
    hiddenCostsNothing(rig);
    archiveListsItsMembers(rig);
    failuresAreSentences(rig);
    badgesNameTheState(rig);
    pagesSitOnACard(rig);
  }
  std::println("column-preview-pixels: all checks passed");
  return 0;
}
