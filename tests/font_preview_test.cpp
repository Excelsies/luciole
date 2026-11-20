#include "font_fixture.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/font_facts.h"
#include "thumbnails/font_preview.h"
#include "thumbnails/metadata_probe.h"
#include "thumbnails/preview_metadata.h"
#include "thumbnails/preview_service.h"
#include "thumbnails/thumbnail_provider.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <random>
#include <thread>

namespace {
  using namespace luciole::test::fonts;
  using luciole::GioPtr;
  constexpr std::string_view kSuite = "font-preview";
  constexpr luciole::FontSheetColors kColors{.text = 0xE6D2B4, .surface = 0x1A334D};
  constexpr std::string_view kVariant = "e6d2b4:1a334d";

  int distance(const LoadedImageFile& image, std::size_t pixel, std::uint32_t color) {
    int sum = 0;
    for (std::size_t c = 0; c < 3; ++c) {
      const auto expected = static_cast<int>((color >> (16 - (c * 8))) & 0xFFU);
      sum += std::abs(static_cast<int>(image.rgba[(pixel * 4) + c]) - expected);
    }
    return sum;
  }
  // Ink weighted by coverage, so antialiased edges count in proportion and the share does not depend on size.
  double inkShare(const LoadedImageFile& image) {
    const double full =
        distance(LoadedImageFile{.rgba = {0xE6, 0xD2, 0xB4, 0xFF}, .width = 1, .height = 1}, 0, kColors.surface);
    double ink = 0;
    for (std::size_t i = 0; i < image.rgba.size() / 4; ++i) {
      ink += std::min(1.0, distance(image, i, kColors.surface) / full);
    }
    return ink / static_cast<double>(std::max<std::size_t>(1, image.rgba.size() / 4));
  }
  void checkSheet(const LoadedImageFile& image, int side, bool textInk = true) {
    TEST_CHECK(image.width == side && image.height >= 1 && image.height <= side);
    TEST_CHECK(image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4);
    TEST_CHECK(distance(image, 0, kColors.surface) == 0 && image.rgba[3] == 255);
    std::size_t text = 0;
    for (std::size_t i = 0; i < image.rgba.size() / 4; ++i) {
      text += distance(image, i, kColors.text) < distance(image, i, kColors.surface) ? 1U : 0U;
    }
    TEST_CHECK(inkShare(image) > 0.01 && (!textInk || text > static_cast<std::size_t>(side) / 4));
  }
  // Leftmost and rightmost ink columns of the first drawn line.
  std::pair<int, int> firstLineInk(const LoadedImageFile& image) {
    int top = -1, left = image.width, right = -1;
    const int band = std::max(4, image.width / 32);
    for (int y = 0; y < image.height && (top < 0 || y < top + band); ++y) {
      for (int x = 0; x < image.width; ++x) {
        if (distance(image, (static_cast<std::size_t>(y) * image.width) + x, kColors.surface) != 0) {
          top = top < 0 ? y : top;
          left = std::min(left, x);
          right = std::max(right, x);
        }
      }
    }
    return {left, right};
  }

  void sheetChecks(const std::string& kind, const fs::path& path) {
    const bool colored = kind.starts_with("color");
    const bool latin = !colored && kind != "non-Latin" && kind != "symbol";
    const auto small = luciole::renderFontSheet(path, 256, kColors),
               large = luciole::renderFontSheet(path, 768, kColors);
    TEST_CHECK(small.has_value() && large.has_value());
    checkSheet(*small, 256, !colored);
    checkSheet(*large, 768, !colored);
    // The layout scales with the sheet, so ink covers about the same share at every size.
    std::println("{}: {} ink share {:.3f} at 256, {:.3f} at 768", kSuite, kind, inkShare(*small), inkShare(*large));
    TEST_CHECK(inkShare(*small) < inkShare(*large) * 1.2 && inkShare(*large) < inkShare(*small) * 1.2);
    // Left-to-right samples start at the left margin; right-to-left ones end at the right margin.
    const auto [left, right] = firstLineInk(*large);
    const int margin = 2 * 12 * 768 / 320;
    TEST_CHECK(kind == "non-Latin" ? right >= 768 - margin : (!latin || left <= margin));
  }

  void isolationChecks(const fs::path& copy) {
    FcConfig* current = FcConfigGetCurrent();
    const auto applicationFonts = [] {
      const FcFontSet* set = FcConfigGetFonts(nullptr, FcSetApplication);
      return set ? set->nfont : 0;
    };
    const auto before = applicationFonts();
    TEST_CHECK(luciole::renderFontSheet(copy, 256, kColors).has_value());
    TEST_CHECK(FcConfigGetCurrent() == current && applicationFonts() == before);
    const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> byFile(
        FcPatternBuild(nullptr, FC_FILE, FcTypeString, copy.c_str(), nullptr), &FcPatternDestroy
    );
    const std::unique_ptr<FcObjectSet, decltype(&FcObjectSetDestroy)> objects(
        FcObjectSetBuild(FC_FILE, nullptr), &FcObjectSetDestroy
    );
    const std::unique_ptr<FcFontSet, decltype(&FcFontSetDestroy)> listed(
        FcFontList(nullptr, byFile.get(), objects.get()), &FcFontSetDestroy
    );
    TEST_CHECK(listed && listed->nfont == 0);
  }

  // The real helper process through the provider, with its time, memory and output limits.
  std::expected<LoadedImageFile, std::string> viaHelper(const fs::path& path, int side, int maxFileMiB = 64) {
    const luciole::Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    TEST_CHECK(file.valid());
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const luciole::ThumbnailOptions options{.maxFileMiB = maxFileMiB};
    const auto begin = std::chrono::steady_clock::now();
    auto result = luciole::renderThumbnailProvider(
        file.get(), "font/ttf", side, options, cancel.get(), luciole::ThumbnailPurpose::Preview, kVariant
    );
    TEST_CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(9));
    return result;
  }

  void hostileChecks(const fs::path& root, const fs::path& truetype) {
    const auto good = readBytes(truetype);
    std::mt19937 random(7);
    Bytes noise(64U * 1024U);
    std::ranges::generate(noise, [&] { return static_cast<std::uint8_t>(random()); });
    const std::vector<std::pair<std::string, Bytes>> cases{
        {"truncated.ttf", Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2))},
        {"empty.ttf", {}},
        {"noise.ttf", noise},
    };
    for (const auto& [name, bytes] : cases) {
      const auto path = root / name;
      writeBytes(path, bytes);
      TEST_CHECK(!luciole::renderFontSheet(path, 256, kColors));
      const auto helper = viaHelper(path, 256);
      TEST_CHECK(!helper && !helper.error().empty());
      std::println("{}: helper refused {}: {}", kSuite, name, helper.error());
    }
    auto padded = good;
    padded.resize(std::max<std::size_t>(good.size(), 1536U * 1024U), 0);
    writeBytes(root / "padded.ttf", padded);
    TEST_CHECK(!viaHelper(root / "padded.ttf", 256, 1));
    TEST_CHECK(!viaHelper(truetype, 2048));
    const auto drawn = viaHelper(truetype, 512);
    TEST_CHECK(drawn.has_value());
    checkSheet(*drawn, 512);
  }

  void wait(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  luciole::DirectoryEntry entry(const fs::path& path, const std::string& mime) {
    const auto location = luciole::Location::fromPath(path.native());
    const GioPtr<GFileInfo> info(g_file_query_info(
        luciole::gioFile(location).get(),
        "standard::size,time::modified,id::file,id::filesystem,etag::value,time::changed,time::changed-nsec,time::"
        "changed-usec",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info != nullptr);
    luciole::DirectoryEntry result{
        .id = 1,
        .place = location,
        .name = path.filename().native(),
        .type = luciole::FileType::Regular,
        .size = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_STANDARD_SIZE),
        .modifiedSeconds = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED),
        .contentType = mime,
        .contentRevision = luciole::readChangeRevision(info.get()),
    };
    result.setEtag(g_file_info_get_etag(info.get()));
    result.setIdentity(luciole::readFileIdentity(info.get()));
    return result;
  }

  // A size limit above the font cap still draws the sheet, and the sheet and the facts read the same sizes.
  void capChecks(const fs::path& root, const fs::path& truetype) {
    TEST_CHECK(luciole::fontByteCap(64) == 64ULL * 1024 * 1024 && luciole::fontByteCap(0) == 1024 * 1024);
    TEST_CHECK(luciole::fontByteCap(1024) == 256ULL * 1024 * 1024);
    const auto drawn = viaHelper(truetype, 512, 1024);
    TEST_CHECK(drawn.has_value());
    if (drawn) {
      checkSheet(*drawn, 512);
    }
    auto padded = readBytes(truetype);
    padded.resize(65U * 1024U * 1024U, 0);
    const auto large = root / "Large font.ttf";
    writeBytes(large, padded);
    padded = {};
    TEST_CHECK(viaHelper(large, 256, 1024).has_value());
    const luciole::ThumbnailOptions options{.maxFileMiB = 1024};
    const auto fields = luciole::probePreviewMetadata(entry(large, "font/ttf"), options, nullptr);
    TEST_CHECK(std::ranges::contains(fields, "Family", &luciole::MetadataField::label));
    fs::remove(large);
  }

  // The preview pane path: ThumbnailService and PreviewService, with the theme colors taken from the palette.
  void previewChecks(const fs::path& root, const fs::path& source, const std::string& mime) {
    Palette themed = palette;
    themed.surface = rgbaHex(0x1A334DFF);
    themed.onSurface = rgbaHex(0xE6D2B4FF);
    setPalette(themed);
    TEST_CHECK(luciole::fontSheetVariant(mime) == kVariant && luciole::fontSheetVariant("image/png").empty());
    luciole::ThumbnailService thumbnails;
    luciole::PreviewService preview(thumbnails);
    thumbnails.setChangedCallback([&] { preview.thumbnailsChanged(); });
    preview.show(entry(source, mime), 400);
    wait([&] { return !preview.busy(); });
    const auto& snapshot = preview.snapshot();
    TEST_CHECK(snapshot.status == luciole::PreviewStatus::Ready && snapshot.image && snapshot.image->pixels);
    checkSheet(*snapshot.image->pixels, 512);
    const auto row = [&](std::string_view label) {
      const auto found = std::ranges::find(snapshot.metadata, label, &luciole::MetadataField::label);
      return found == snapshot.metadata.end() ? std::string() : found->value;
    };
    const auto facts = luciole::readFontFacts(readBytes(source));
    TEST_CHECK(facts && row("Family") == facts->family && row("Glyphs") == std::to_string(facts->glyphs));
    TEST_CHECK(!row("Format").empty() && !row("Scripts").empty());
    // Theme-colored sheets stay out of the shared thumbnail cache.
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    wait([&] { return std::chrono::steady_clock::now() >= settle; });
    std::error_code error;
    for (const auto& item : fs::recursive_directory_iterator(root / "cache", error)) {
      TEST_CHECK(item.path().extension() != ".png");
    }
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--font-preview-worker") {
    return luciole::runFontPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--preview-metadata-worker") {
    return luciole::runPreviewMetadataWorker(std::span(args).subspan(1));
  }
  TEST_CHECK(luciole::parseFontSheetVariant(kVariant) == kColors);
  TEST_CHECK(!luciole::parseFontSheetVariant("e6d2b4") && !luciole::parseFontSheetVariant("e6d2bx:1a334d"));
  const auto fonts = findFonts(kSuite);
  for (const auto& [kind, path] : fonts) {
    sheetChecks(kind, path);
  }
  const auto truetype = fonts.find("TrueType");
  if (truetype == fonts.end()) {
    std::println("{}: skipped WOFF, WOFF2, isolation, helper and preview checks: no TrueType font", kSuite);
    return 0;
  }
  const luciole::test::TempFolder temporary("luciole-font-preview");
  const fs::path& root = temporary.path();
  ::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
  const auto [woff, woff2] = webFonts(kSuite, root, truetype->second);
  const auto woffSheet = luciole::renderFontSheet(woff, 300, kColors);
  TEST_CHECK(woffSheet.has_value());
  checkSheet(*woffSheet, 300);
  if (woff2) {
    const auto facts = luciole::readFontFacts(readBytes(*woff2));
    if (!facts && facts.error() == "This FreeType build cannot read WOFF2 fonts.") {
      std::println("{}: skipped WOFF2 sheet: {}", kSuite, facts.error());
    } else {
      const auto sheet = luciole::renderFontSheet(*woff2, 256, kColors);
      TEST_CHECK(sheet.has_value());
      checkSheet(*sheet, 256);
    }
  }
  const auto copy = root / "Copied font.ttf";
  fs::copy_file(truetype->second, copy);
  isolationChecks(copy);
  hostileChecks(root, truetype->second);
  capChecks(root, truetype->second);
  previewChecks(root, copy, "font/ttf");
  if (const auto opentype = fonts.find("OpenType"); opentype != fonts.end()) {
    const auto otf = root / "Template clash.otf";
    fs::copy_file(opentype->second, otf);
    previewChecks(root, otf, "application/vnd.oasis.opendocument.formula-template");
  }
  std::println("font preview checks passed");
}
