#include "formats/table_grid.h"
#include "fs/cancel_token.h"
#include "fs/location.h"
#include "machine_slowdown.h"
#include "png_writer.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "table_fixture.h"
#include "test_check.h"
#include "thumbnails/sheet_preview.h"
#include "thumbnails/table_loader.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/table_view.h"
#include "ui/palette.h"
#include "workbench/color.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <gio/gio.h>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::test::table;
  namespace wb = luciole::workbench;
  using ::test::GlPixels;

  constexpr int kWidth = 720;
  constexpr int kHeight = 420;
  constexpr std::array<float, 2> kScales{1.0F, 1.5F};

  Color color(wb::Rgb rgb) {
    return rgba(
        static_cast<float>(rgb.r) / 255.0F, static_cast<float>(rgb.g) / 255.0F, static_cast<float>(rgb.b) / 255.0F, 1
    );
  }

  // The default dark palette, as the scene builders keep its roles.
  Palette darkPalette() {
    const wb::ColorPalette roles;
    Palette out{};
    out.surface = color(roles.surface);
    out.onSurface = color(roles.onSurface);
    out.onSurfaceVariant = color(roles.onSurfaceVariant);
    out.surfaceContainerLow = color(roles.surfaceContainerLow);
    out.surfaceContainer = color(roles.surfaceContainer);
    out.surfaceContainerHigh = color(roles.surfaceContainerHigh);
    out.surfaceContainerHighest = color(roles.surfaceContainerHighest);
    out.outlineVariant = color(roles.outlineVariant);
    out.primary = color(roles.primary);
    out.onPrimary = color(roles.surface);
    out.primaryContainer = color(roles.surfaceContainerHighest);
    out.onPrimaryContainer = color(roles.onSurface);
    out.secondary = color(roles.secondary);
    out.onSecondary = color(roles.surface);
    out.secondaryContainer = color(roles.surfaceContainerHigh);
    out.onSecondaryContainer = color(roles.onSurface);
    out.tertiary = color(roles.tertiary);
    out.onTertiary = color(roles.surface);
    out.hover = color(roles.surfaceContainerHigh);
    out.onHover = color(roles.onSurface);
    out.shadow = rgba(0, 0, 0, 1);
    out.scrim = rgba(0, 0, 0, 1);
    return out;
  }

  double threadSeconds() {
    timespec now{};
    TEST_CHECK(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return static_cast<double>(now.tv_sec) + (static_cast<double>(now.tv_nsec) * 1.0E-9);
  }

  // Processor time of this process and of the helpers it has waited for.
  double processSeconds() {
    const auto seconds = [](const timeval& time) {
      return static_cast<double>(time.tv_sec) + (static_cast<double>(time.tv_usec) * 1.0E-6);
    };
    rusage self{};
    rusage children{};
    TEST_CHECK(::getrusage(RUSAGE_SELF, &self) == 0 && ::getrusage(RUSAGE_CHILDREN, &children) == 0);
    return seconds(self.ru_utime) + seconds(self.ru_stime) + seconds(children.ru_utime) + seconds(children.ru_stime);
  }

  struct Loaded {
    std::shared_ptr<const formats::TableGrid> table;
    double wallMs = 0;
    double processorMs = 0;
  };

  // Loads a file through the table loader, running the main context until it answers.
  Loaded load(const std::filesystem::path& path, std::size_t sheet = 0) {
    std::optional<TableResult> result;
    const auto wallStart = std::chrono::steady_clock::now();
    const double processorStart = processSeconds();
    startTableLoad(
        {.location = Location::fromPath(path.string()),
         .name = path.filename().string(),
         .contentType = {},
         .sheetIndex = sheet},
        CancelToken{}, [&](TableResult value) { result = std::move(value); }
    );
    const auto deadline = wallStart + std::chrono::seconds(30);
    while (!result) {
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (!result->has_value()) {
      std::println("{}: {}", path.filename().string(), result->error());
    }
    TEST_CHECK(result->has_value());
    return {
        .table = **result,
        .wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallStart).count(),
        .processorMs = (processSeconds() - processorStart) * 1000.0,
    };
  }

  struct Region {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };

  Region regionOf(const Node& node, float scale) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return {
        .left = static_cast<int>(std::ceil(x * scale)),
        .top = static_cast<int>(std::ceil(y * scale)),
        .right = static_cast<int>(std::floor((x + node.width()) * scale)),
        .bottom = static_cast<int>(std::floor((y + node.height()) * scale)),
    };
  }

  std::uint32_t packed(std::array<std::uint8_t, 4> p) {
    return (std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U) | (std::uint32_t{p[2]} << 8U) | p[3];
  }

  // The most common color of a region and the share of pixels that differ from it.
  std::pair<std::uint32_t, double> background(const GlPixels& pixels, Region region) {
    std::unordered_map<std::uint32_t, std::size_t> counts;
    std::size_t total = 0;
    for (int y = std::max(region.top, 0); y < std::min(region.bottom, pixels.height); ++y) {
      for (int x = std::max(region.left, 0); x < std::min(region.right, pixels.width); ++x) {
        ++counts[packed(pixels.at(x, y))];
        ++total;
      }
    }
    std::pair<std::uint32_t, std::size_t> most{0, 0};
    for (const auto& [rgba, count] : counts) {
      most = count > most.second ? std::pair{rgba, count} : most;
    }
    return {most.first, total == 0 ? 0 : static_cast<double>(total - most.second) / static_cast<double>(total)};
  }

  void expectDrawn(const GlPixels& pixels, const Node& node, float scale, const char* what, double least) {
    const double share = background(pixels, regionOf(node, scale)).second;
    std::println("{}x {}: {:.3f} drawn", scale, what, share);
    TEST_CHECK(share >= least);
  }

  // The middle of the columns of a label's region that hold ink, as a share of its width: about 1 for text at the
  // right end, about 0 at the left.
  double inkPlace(const GlPixels& pixels, const Label& label, float scale) {
    const Region region = regionOf(label, scale);
    const auto plain = background(pixels, region).first;
    int first = region.right;
    int last = region.left;
    for (int y = region.top; y < region.bottom; ++y) {
      for (int x = region.left; x < region.right; ++x) {
        if (packed(pixels.at(x, y)) != plain) {
          first = std::min(first, x);
          last = std::max(last, x);
        }
      }
    }
    TEST_CHECK(first <= last);
    return (static_cast<double>(first + last) / 2 - region.left) / static_cast<double>(region.right - region.left);
  }

  const Label* labelWith(const Node& node, std::string_view text) {
    if (!node.visible()) {
      return nullptr;
    }
    if (const auto* label = dynamic_cast<const Label*>(&node); label != nullptr && label->text() == text) {
      return label;
    }
    for (const auto& child : node.children()) {
      if (const auto* found = labelWith(*child, text)) {
        return found;
      }
    }
    return nullptr;
  }

  std::size_t nodes(const Node& node) {
    std::size_t count = 1;
    for (const auto& child : node.children()) {
      count += nodes(*child);
    }
    return count;
  }

  // Updates and draws until the view asks for no more frames; returns the last capture.
  GlPixels settle(TableView& view, ::test::SceneCapture& capture, float scale, bool& frame) {
    GlPixels pixels;
    for (int pass = 0; pass < 8; ++pass) {
      frame = false;
      view.update();
      pixels = capture.capture(view, kWidth, kHeight, scale);
      if (!frame) {
        return pixels;
      }
    }
    TEST_CHECK(!frame);
    return pixels;
  }

  void keep(const GlPixels& pixels, std::string_view name, float scale) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("table-{}-{}x.png", name, scale);
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  std::string inventoryCsv() {
    std::string text = "Item,Category,Quantity,Unit price,Updated,In stock\n";
    // Quotes inside a quoted cell are doubled.
    const std::array<std::string_view, 6> items{"Desk lamp", "Notebook, A5", R"(Cable ""USB-C"")",
                                                "Chair",     "Monitor arm",  "Tea"};
    const std::array<std::string_view, 3> kinds{"Office", "Furniture", "Kitchen"};
    for (int i = 0; i < 24; ++i) {
      text += std::format(
          "\"{} {}\",{},{},{:.2f},2024-{:02}-{:02},{}\n", items[static_cast<std::size_t>(i) % items.size()], i + 1,
          kinds[static_cast<std::size_t>(i) % kinds.size()], (i * 37) % 900, 3.5 + (i * 12.25), (i % 12) + 1,
          (i % 27) + 1, i % 3 == 0 ? "no" : "yes"
      );
    }
    return text;
  }

  std::vector<ZipMember> budgetWorkbook() {
    std::string budget = "<row r=\"1\">"
        + textCell("A1", "Month")
        + textCell("B1", "Income")
        + textCell("C1", "Spent")
        + textCell("D1", "Paid")
        + textCell("E1", "Note")
        + "</row>";
    for (int i = 0; i < 18; ++i) {
      const int row = i + 2;
      budget += std::format(
          R"(<row r="{0}">{1}<c r="B{0}"><v>{2}</v></c><c r="C{0}"><v>{3}</v></c><c r="D{0}" t="b"><v>{4}</v></c>{5}</row>)",
          row, textCell(std::format("A{}", row), std::format("2024-{:02}", (i % 12) + 1)), 4200 + (i * 135.5),
          3100 - (i * 42.75), i % 2, i % 5 == 0 ? textCell(std::format("E{}", row), "review") : std::string{}
      );
    }
    return xlsxMembers({{"Budget", budget}, {"Notes", "<row r=\"1\">" + textCell("A1", "Second sheet") + "</row>"}});
  }

  void capturesTables(::test::SceneCapture& capture, const TemporaryFolder& folder) {
    const auto csvPath = folder.path() / "inventory.csv";
    writeFile(csvPath, inventoryCsv());
    const auto xlsxPath = folder.path() / "budget.xlsx";
    writeZip(xlsxPath, budgetWorkbook());
    std::string tall = "n,square\n";
    for (int i = 0; i < 10'500; ++i) {
      tall += std::format("{},{}\n", i, i * i);
    }
    const auto cutPath = folder.path() / "tall.csv";
    writeFile(cutPath, tall);

    TableView view;
    bool frame = false;
    view.onFrameNeeded = [&] { frame = true; };
    for (const float scale : kScales) {
      view.setTable(load(csvPath).table);
      view.select({.row = 2, .column = 3});
      auto pixels = settle(view, capture, scale, frame);
      keep(pixels, "csv", scale);
      expectDrawn(pixels, view.canvas(), scale, "csv grid", 0.05);
      const auto* name = labelWith(view, "Unit price");
      const auto* price = labelWith(view, "28.00");
      const auto* item = labelWith(view, "Chair 4");
      TEST_CHECK(name != nullptr && price != nullptr && item != nullptr);
      // Numbers sit at the right of their column and text at the left.
      TEST_CHECK(inkPlace(pixels, *price, scale) > 0.6);
      TEST_CHECK(inkPlace(pixels, *item, scale) < 0.4);
      // The selected cell is filled with the selection's color.
      float x = 0;
      float y = 0;
      Node::absolutePosition(price, x, y);
      const auto mark =
          pixels.at(static_cast<int>(std::lround((x - 3) * scale)), static_cast<int>(std::lround((y + 2) * scale)));
      const auto want = color(wb::ColorPalette{}.surfaceContainerHighest);
      TEST_NEAR(mark[0], std::lround(want.r * 255), 2);
      TEST_NEAR(mark[1], std::lround(want.g * 255), 2);

      view.setTable(load(xlsxPath).table);
      pixels = settle(view, capture, scale, frame);
      keep(pixels, "xlsx", scale);
      expectDrawn(pixels, view.canvas(), scale, "xlsx grid", 0.05);
      expectDrawn(pixels, view.sheetList(), scale, "sheet list", 0.05);
      TEST_CHECK(labelWith(view.sheetList(), "Notes") != nullptr);
      TEST_CHECK(inkPlace(pixels, *labelWith(view, "4200"), scale) > 0.6);
      TEST_CHECK(inkPlace(pixels, *labelWith(view, "TRUE"), scale) > 0.3);
      TEST_CHECK(inkPlace(pixels, *labelWith(view, "TRUE"), scale) < 0.7);

      // A sheet that fails to load shows its message under the sheet list, which stays.
      view.setMessage("This sheet could not be read.");
      pixels = settle(view, capture, scale, frame);
      keep(pixels, "sheet-message", scale);
      expectDrawn(pixels, view.sheetList(), scale, "sheet list beside a message", 0.05);
      expectDrawn(pixels, *labelWith(view, "This sheet could not be read."), scale, "message", 0.02);
      TEST_CHECK(!view.canvas().visible());
      view.setMessage({});

      view.setTable(load(cutPath).table);
      view.select({.row = 9'999, .column = 1});
      pixels = settle(view, capture, scale, frame);
      keep(pixels, "cut", scale);
      TEST_CHECK(view.notice() == "Showing the first 10,000 rows");
      expectDrawn(pixels, *labelWith(view, view.notice()), scale, "notice", 0.05);
      TEST_CHECK(labelWith(view, "99980001") != nullptr);
    }
  }

  void measures(::test::SceneCapture& capture, const TemporaryFolder& folder) {
    std::string csv;
    for (int column = 0; column < 50; ++column) {
      csv += std::format("{}column {}", column == 0 ? "" : ",", column);
    }
    csv += '\n';
    for (int row = 0; row < 5'000; ++row) {
      for (int column = 0; column < 50; ++column) {
        csv += column == 0 ? std::format("item {}", row) : std::format(",{}", (row * 31) + column);
      }
      csv += '\n';
    }
    const auto csvPath = folder.path() / "big.csv";
    writeFile(csvPath, csv);
    std::string sheet;
    for (int row = 0; row < 5'000; ++row) {
      sheet +=
          std::format("<row r=\"{}\">", row + 1) + textCell(std::format("A{}", row + 1), std::format("item {}", row));
      for (int column = 1; column < 30; ++column) {
        sheet += numberCell(
            formats::columnLetters(static_cast<std::size_t>(column)) + std::to_string(row + 1), (row * 31) + column
        );
      }
      sheet += "</row>";
    }
    const auto xlsxPath = folder.path() / "big.xlsx";
    writeZip(xlsxPath, xlsxMembers({{"Data", sheet}}));
    std::println("files: CSV {} bytes, XLSX {} bytes", csv.size(), std::filesystem::file_size(xlsxPath));

    TableView view;
    bool frame = false;
    view.onFrameNeeded = [&] { frame = true; };
    for (const auto& [path, what] : {std::pair{csvPath, "CSV 5,000 x 50"}, std::pair{xlsxPath, "XLSX 5,000 x 30"}}) {
      // Processor time, as the machine slowdown scales it, so that a busy machine does not fail a bound meant to catch
      // a slower reader.
      const double loadSlowdown = luciole::test::machineSlowdown();
      const auto loaded = load(path);
      TEST_CHECK(loaded.table->rowCount == 5'000);
      std::println("{}: loaded in {:.1f} ms, {:.1f} ms of processor time", what, loaded.wallMs, loaded.processorMs);
      TEST_CHECK(loaded.processorMs / loadSlowdown < 2'000);
      view.setTable(loaded.table);
      settle(view, capture, 1, frame);
      std::println("{}: {} nodes in the view, {} labels", what, nodes(view), view.canvas().labelCount());
      // Scroll frames: a wheel step, the update and the layout with drawing, as the main context runs them.
      InputDispatcher dispatcher;
      dispatcher.setSceneRoot(&view);
      float x = 0;
      float y = 0;
      Node::absolutePosition(&view.canvas(), x, y);
      x += view.canvas().width() / 2;
      y += view.canvas().height() / 2;
      dispatcher.pointerEnter(x, y, 1);
      constexpr int kFrames = 120;
      const double frameSlowdown = luciole::test::machineSlowdown();
      double updateTotal = 0;
      double frameTotal = 0;
      double frameMost = 0;
      for (int i = 0; i < kFrames; ++i) {
        const double start = threadSeconds();
        dispatcher.pointerAxis(x, y, i % 10 == 9 ? 1 : 0, 0, 37.0, 0, 0, 0);
        view.update();
        updateTotal += threadSeconds() - start;
        (void)capture.capture(view, kWidth, kHeight, 1);
        const double spent = threadSeconds() - start;
        frameTotal += spent;
        frameMost = std::max(frameMost, spent);
      }
      std::println(
          "{}: per scroll frame {:.3f} ms of update, {:.3f} ms with layout and drawing on average, {:.3f} ms at most; "
          "{} nodes after scrolling",
          what, updateTotal * 1000 / kFrames, frameTotal * 1000 / kFrames, frameMost * 1000, nodes(view)
      );
      TEST_CHECK(view.scrollY() > 0);
      TEST_CHECK(updateTotal / kFrames / frameSlowdown < 0.004);
    }
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(darkPalette());
  const TemporaryFolder folder;
  capturesTables(*capture, folder);
  measures(*capture, folder);
  std::println("table-view-pixels: all checks passed");
  return 0;
}
