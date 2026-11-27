#include "app/page/workbench_preview.h"
#include "column_preview_fixture.h"
#include "column_preview_rig.h"
#include "config/settings.h"
#include "imaging/text_tile_service.h"
#include "png_writer.h"
#include "render/animation/animation_manager.h"
#include "render/core/image_decoder.h"
#include "render/scene/image_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/sheet_preview.h"
#include "thumbnails/thumbnail_service.h"
#include "thumbnails/thumbnail_worker.h"
#include "ui/controls/animated_image_view.h"
#include "ui/controls/button.h"
#include "ui/controls/file_preview_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/media_view.h"
#include "ui/controls/mesh_view.h"
#include "ui/controls/table_view.h"
#include "ui/controls/text_tile_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/column_preview_page.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/list_rows.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <glib.h>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Pictures of every Workbench preview surface beside the side preview's, for looking at: written into the folder
// named by LUCIOLE_CAPTURE_DIR as <surface>-<set>.png, where <set> is LUCIOLE_SHOT_SET ("after" by default). Without
// the folder it only checks that each surface is shown.
namespace {
  using luciole::ColumnPreviewKind;
  using luciole::ColumnPreviewStatus;
  using luciole::WorkbenchPreview;
  using luciole::test::tiles::bytesOf;
  using luciole::test::tiles::distance;
  using luciole::test::preview::Rig;
  using Clock = std::chrono::steady_clock;
  using Pixel = std::array<std::uint8_t, 4>;

  constexpr int kWidth = 460;
  constexpr int kHeight = 420;

  const std::string& shotSet() {
    static const std::string set = [] {
      const char* value = std::getenv("LUCIOLE_SHOT_SET");
      return std::string(value != nullptr && value[0] != '\0' ? value : "after");
    }();
    return set;
  }

  void write(const ::test::GlPixels& pixels, std::string_view name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("{}-{}.png", name, shotSet());
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  void addFile(Rig& rig, std::string_view name, std::string_view bytes) {
    luciole::test::preview::writeFile(rig.files.folder() / name, bytes);
    const auto location = luciole::Location::fromPath((rig.files.folder() / name).native());
    for (int round = 0; round < 4000 && !luciole::rowOf(rig.browser.model(), location); ++round) {
      Rig::pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(luciole::rowOf(rig.browser.model(), location).has_value());
  }

  void addPicture(Rig& rig, std::string_view name, int width, int height, std::array<std::uint8_t, 4> color) {
    const auto bytes = ::test::stillPng(width, height, color);
    addFile(rig, name, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  }

  // The slot for `file`, until its content is shown or it has failed.
  void workbench(Rig& rig, std::string_view file, std::string_view shot) {
    rig.show(file);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    ::test::GlPixels pixels;
    while (Clock::now() < deadline) {
      pixels = rig.frame(kWidth, kHeight, 1);
      const auto* load = rig.preview->load();
      if (load != nullptr && load->status() != ColumnPreviewStatus::Loading && rig.shown()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (int i = 0; i < 4; ++i) {
      pixels = rig.frame(kWidth, kHeight, 1);
    }
    write(pixels, std::format("workbench-{}", shot));
  }

  // The side preview of the same file: its text, status and facts; a picture's own pixels are drawn through the
  // application's shared texture cache, which needs a display, so the picture box stays empty here.
  void sidePreview(
      ::test::SceneCapture& capture, luciole::FileIconCache& icons, Rig& rig, std::string_view file,
      std::string_view message, std::string_view shot
  ) {
    const auto& model = rig.browser.model();
    luciole::PreviewSnapshot snapshot;
    for (const auto id : model.rows()) {
      if (model.entry(id)->name == file) {
        snapshot.entry = *model.entry(id);
      }
    }
    TEST_CHECK(snapshot.entry.has_value());
    snapshot.status = luciole::PreviewStatus::Ready;
    snapshot.message = std::string(message);
    std::ifstream in(rig.files.folder() / file, std::ios::binary);
    std::string text(2000, '\0');
    in.read(text.data(), 2000);
    text.resize(static_cast<std::size_t>(in.gcount()));
    snapshot.text = text.find('\0') == std::string::npos && !shot.starts_with("picture") ? text : std::string{};
    FilePreviewView view(icons, false, [] {}, [] {}, [] {});
    view.sync(snapshot, 300, 560, std::string(model.typeLabel(0)));
    write(capture.capture(view, 300, 560, 1), std::format("side-{}", shot));
  }

  void pageShot(::test::SceneCapture& capture, luciole::FileIconCache& icons, Rig& rig) {
    const auto& model = rig.browser.model();
    luciole::PreviewSnapshot snapshot;
    for (const auto id : model.rows()) {
      if (model.entry(id)->name == "paper.pdf") {
        snapshot.entry = *model.entry(id);
      }
    }
    TEST_CHECK(snapshot.entry.has_value());
    snapshot.status = luciole::PreviewStatus::Ready;
    snapshot.metadata = {{.label = "Format", .value = "PDF 1.4"}, {.label = "Pages", .value = "1"}};
    luciole::ColumnPreviewPage page(icons);
    page.setTextSize(luciole::quietTextSize(Style::fontSizeBody, 0));
    page.sync(snapshot, kWidth, kHeight);
    write(capture.capture(page, kWidth, kHeight, 1), "workbench-still-page");
  }

  void videoStill(::test::SceneCapture& capture) {
    Flex root;
    root.setDirection(FlexDirection::Vertical);
    root.setAlign(FlexAlign::Stretch);
    root.setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    auto media = std::make_unique<MediaView>(ColorRole::SurfaceContainerLow, luciole::AudioOutput::Silent);
    media->setFlexGrow(1);
    auto* view = static_cast<MediaView*>(root.addChild(std::move(media)));
    auto image = std::make_shared<LoadedImageFile>();
    image->width = 160;
    image->height = 90;
    image->rgba.resize(std::size_t{160} * 90 * 4);
    for (int y = 0; y < 90; ++y) {
      for (int x = 0; x < 160; ++x) {
        auto* p = &image->rgba[(static_cast<std::size_t>(y) * 160 + static_cast<std::size_t>(x)) * 4];
        p[0] = x < 80 ? 240 : 30;
        p[1] = x < 80 ? 140 : 170;
        p[2] = x < 80 ? 40 : 160;
        p[3] = 255;
      }
    }
    view->setPoster(image);
    ::test::GlPixels pixels;
    for (int i = 0; i < 20; ++i) {
      Rig::pump();
      pixels = capture.capture(root, kWidth, kHeight, 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    write(pixels, "workbench-video-still");
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
    return 2;
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  g_setenv("TZ", "UTC", TRUE);
  setPalette(luciole::test::tiles::testPalette());
  static const luciole::test::preview::PreviewFiles files;
  luciole::FileIconCache icons;
  {
    Rig rig(*capture, files);
    addPicture(rig, "landscape.png", 400, 240, {230, 120, 40, 255});
    addPicture(rig, "portrait.png", 240, 400, {60, 160, 220, 255});
    addPicture(rig, "tiny.png", 24, 16, {120, 200, 90, 255});
    addFile(rig, "empty.txt", "");
    std::string many;
    for (int line = 0; line < 2600; ++line) {
      many += std::format("line {}\n", line);
    }
    addFile(rig, "many-lines.txt", many);
    // An ar archive whose name table is past the preview's limit; the file is sparse.
    {
      const auto path = rig.files.folder() / "huge-table.a";
      constexpr std::uint64_t table = 20ULL * 1024 * 1024;
      std::string header = "!<arch>\n//";
      header.resize(8 + 48, ' ');
      header += std::to_string(table);
      header.resize(8 + 58, ' ');
      header += "`\n";
      {
        std::ofstream out(path, std::ios::binary);
        out << header;
      }
      std::filesystem::resize_file(path, header.size() + table);
    }
    addFile(rig, "broken.zip", "PK\x03\x04" + std::string(300, '\x7F'));
    // Let the folder list the sparse file too.
    for (int round = 0; round < 200; ++round) {
      Rig::pump();
    }
    const struct {
      std::string_view file;
      std::string_view shot;
      std::string_view status;
    } cases[] = {
        {"landscape.png", "picture-landscape", ""},
        {"portrait.png", "picture-portrait", ""},
        {"tiny.png", "picture-tiny", ""},
        {"style.css", "text", ""},
        {"empty.txt", "empty-file", "Empty"},
        {"many-lines.txt", "truncated-text", "Truncated"},
        {"pack.zip", "archive-listing", ""},
        {"huge-table.a", "archive-too-large", "Too large"},
        {"broken.zip", "archive-damaged", "Read error"},
    };
    for (const auto& item : cases) {
      workbench(rig, item.file, item.shot);
      sidePreview(*capture, icons, rig, item.file, item.status, item.shot);
    }
    pageShot(*capture, icons, rig);
  }
  videoStill(*capture);
  std::println("workbench-preview-shots: all surfaces shown");
  return 0;
}
