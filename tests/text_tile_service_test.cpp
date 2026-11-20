#include "formats/styled_text.h"
#include "imaging/text_tile_service.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <glib.h>
#include <memory>
#include <numeric>
#include <optional>
#include <print>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace luciole;
  using Clock = std::chrono::steady_clock;

  std::shared_ptr<const formats::StyledDocument> document(std::string_view text, std::string_view name) {
    return std::make_shared<const formats::StyledDocument>(formats::styledText(text, formats::fileKind(name)));
  }

  // Lines of CSS, each with a color code.
  std::string cssLines(int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
      out += std::format(".rule{} {{ color: #{:02x}{:02x}80; }}\n", i, (i * 7) % 256, (i * 13) % 256);
    }
    return out;
  }

  TextTileOptions options(double width = 400.0, double scale = 1.0) {
    TextTileOptions out{.width = width, .scale = scale, .theme = {}, .fonts = {}};
    out.theme.roles.fill(rgbHex(0xE0E0E0));
    out.theme.background = rgbHex(0x101010);
    return out;
  }

  std::uint64_t checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
      hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
  }

  struct Received {
    std::vector<std::uint64_t> layoutRevisions;
    std::optional<TextTileGeometry> geometry;
    std::vector<std::pair<std::uint64_t, std::size_t>> tiles; // revision, index
    std::vector<TextTile> pixels;
    std::size_t errors = 0;
    bool offThread = false;
  };

  TextTileEvents recorder(Received& into) {
    const auto main = std::this_thread::get_id();
    return {
        .laidOut =
            [&into, main](std::uint64_t revision, std::expected<TextTileGeometry, std::string> geometry) {
              into.offThread = into.offThread || std::this_thread::get_id() != main;
              into.layoutRevisions.push_back(revision);
              TEST_CHECK(geometry.has_value());
              into.geometry = *geometry;
            },
        .tile =
            [&into, main](std::uint64_t revision, std::size_t index, std::expected<TextTile, std::string> tile) {
              into.offThread = into.offThread || std::this_thread::get_id() != main;
              into.tiles.emplace_back(revision, index);
              if (tile) {
                into.pixels.push_back(std::move(*tile));
              } else {
                ++into.errors;
              }
            },
    };
  }

  // Runs the default main context until `done` holds; fails after ten seconds.
  template <typename Done> void pumpUntil(const Done& done) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!done()) {
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      TEST_CHECK(Clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Runs the default main context for a while, to let anything that would arrive arrive.
  void pumpFor(std::chrono::milliseconds span) {
    const auto end = Clock::now() + span;
    while (Clock::now() < end) {
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void testLayoutAndTilesArriveOnTheMainContext() {
    TextTileService service;
    Received got;
    const auto css = document(cssLines(40), "style.css");
    auto handle = service.open(css, options(), recorder(got));
    TEST_CHECK(handle.valid() && handle.revision() == 1);
    pumpUntil([&] { return got.geometry.has_value(); });
    TextTileLayout direct(css, options());
    TEST_CHECK(got.layoutRevisions == std::vector<std::uint64_t>{1});
    TEST_CHECK(got.geometry->width == direct.width() && got.geometry->height == direct.height());
    TEST_CHECK(got.geometry->tileCount == direct.tileCount() && got.geometry->tileCount >= 3);
    TEST_CHECK(!got.geometry->truncated);
    const std::array<std::size_t, 3> order{2, 0, 1};
    handle.request(order);
    pumpUntil([&] { return got.tiles.size() == 3; });
    TEST_CHECK((got.tiles == std::vector<std::pair<std::uint64_t, std::size_t>>{{1, 2}, {1, 0}, {1, 1}}));
    const auto expected = direct.renderTile(0);
    TEST_CHECK(expected.has_value());
    const auto& tile = got.pixels[1];
    TEST_CHECK(tile.top == 0 && tile.width == expected->width && tile.height == expected->height);
    TEST_CHECK(checksum(tile.rgba) == checksum(expected->rgba));
    TEST_CHECK(tile.swatches.size() == expected->swatches.size() && !tile.swatches.empty());
    TEST_CHECK(!got.offThread && got.errors == 0);
  }

  void testNewerRequestReplacesOlder() {
    TextTileService service;
    Received got;
    auto handle = service.open(document(cssLines(400), "style.css"), options(), recorder(got));
    pumpUntil([&] { return got.geometry.has_value(); });
    TEST_CHECK(got.geometry->tileCount >= 10);
    std::vector<std::size_t> first(10);
    std::ranges::iota(first, std::size_t{0});
    handle.request(first);
    const std::array<std::size_t, 1> second{5};
    handle.request(second);
    pumpUntil([&] { return std::ranges::contains(got.tiles, std::pair<std::uint64_t, std::size_t>{1, 5}); });
    pumpFor(std::chrono::milliseconds(150));
    // Only a tile already being drawn when the newer request came can still arrive.
    TEST_CHECK(got.tiles.size() <= 3);
    const std::set<std::size_t> seen(std::from_range, got.tiles | std::views::transform([](const auto& t) {
                                                        return t.second;
                                                      }));
    TEST_CHECK(seen.size() == got.tiles.size());
  }

  void testRelayoutDropsTheOlderRevision() {
    TextTileService service;
    Received got;
    const auto markdown = document("# Title\n\nA paragraph of prose that wraps at the requested width.\n", "a.md");
    auto handle = service.open(markdown, options(200), recorder(got));
    const std::array<std::size_t, 1> zero{0};
    handle.request(zero);
    TEST_CHECK(handle.relayout(options(600, 1.5)) == 2 && handle.revision() == 2);
    handle.request(zero);
    pumpUntil([&] { return !got.tiles.empty(); });
    pumpFor(std::chrono::milliseconds(100));
    TEST_CHECK(got.layoutRevisions == std::vector<std::uint64_t>{2});
    TEST_CHECK((got.tiles == std::vector<std::pair<std::uint64_t, std::size_t>>{{2, 0}}));
    TEST_CHECK(got.geometry->width >= 900); // 600 logical pixels at 1.5
  }

  void testCloseStopsEvents() {
    TextTileService service;
    Received got;
    auto handle = service.open(document(cssLines(400), "style.css"), options(), recorder(got));
    const std::array<std::size_t, 4> some{0, 1, 2, 3};
    handle.request(some);
    handle.close();
    TEST_CHECK(!handle.valid());
    handle.request(some); // a closed handle ignores requests
    TEST_CHECK(handle.relayout(options()) == 0);
    pumpFor(std::chrono::milliseconds(200));
    TEST_CHECK(got.layoutRevisions.empty() && got.tiles.empty());
    {
      Received dropped;
      auto scoped = service.open(document(cssLines(10), "style.css"), options(), recorder(dropped));
      scoped.request(some);
      // Leaving the scope closes the document.
    }
    pumpFor(std::chrono::milliseconds(100));
  }

  void testBoundsAndErrors() {
    TextTileService service;
    Received got;
    auto handle = service.open(document(cssLines(1990), "style.css"), options(), recorder(got));
    pumpUntil([&] { return got.geometry.has_value(); });
    TEST_CHECK(got.geometry->tileCount > kTextTileRequestLimit);
    std::vector<std::size_t> all(got.geometry->tileCount);
    std::ranges::iota(all, std::size_t{0});
    handle.request(all);
    pumpUntil([&] { return got.tiles.size() == kTextTileRequestLimit; });
    pumpFor(std::chrono::milliseconds(100));
    TEST_CHECK(got.tiles.size() == kTextTileRequestLimit && got.tiles.back().second == kTextTileRequestLimit - 1);
    const std::array<std::size_t, 1> past{got.geometry->tileCount};
    handle.request(past);
    pumpUntil([&] { return got.errors == 1; });
  }

  void testWorkerWaitsForTheMainContext() {
    TextTileService service;
    Received got;
    auto handle = service.open(document(cssLines(400), "style.css"), options(), recorder(got));
    std::vector<std::size_t> twenty(20);
    std::ranges::iota(twenty, std::size_t{0});
    handle.request(twenty);
    // Nothing is delivered while the main context is not run; the worker stops once the outbox is full.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    TEST_CHECK(got.layoutRevisions.empty() && got.tiles.empty());
    TEST_CHECK(g_main_context_iteration(nullptr, FALSE) != FALSE);
    TEST_CHECK(got.layoutRevisions.size() == 1);
    TEST_CHECK(got.layoutRevisions.size() + got.tiles.size() <= kTextTileOutboxLimit);
    pumpUntil([&] { return got.tiles.size() == twenty.size(); });
    TEST_CHECK(got.errors == 0);
  }

  void testTwoDocuments() {
    TextTileService service;
    Received a;
    Received b;
    auto first = service.open(document("{\"a\": 1}", "a.json"), options(), recorder(a));
    auto second = service.open(document("key = \"value\"\n", "a.toml"), options(300), recorder(b));
    const std::array<std::size_t, 1> zero{0};
    first.request(zero);
    second.request(zero);
    pumpUntil([&] { return a.tiles.size() == 1 && b.tiles.size() == 1; });
    TEST_CHECK(a.pixels[0].width != b.pixels[0].width);
  }

  void testServiceGoesFirst() {
    Received got;
    TextTileDocument handle;
    {
      TextTileService service;
      handle = service.open(document(cssLines(400), "style.css"), options(), recorder(got));
      const std::array<std::size_t, 2> some{0, 1};
      handle.request(some);
    }
    TEST_CHECK(!handle.valid());
    handle.close();
    pumpFor(std::chrono::milliseconds(50));
    TEST_CHECK(got.tiles.empty());
  }

  // Wall time from open to the first tile of a 2,000-line file, as a view would ask for it.
  void measureFirstTile() {
    TextTileService service;
    Received got;
    const auto css = document(cssLines(2000), "style.css");
    const auto start = Clock::now();
    auto handle = service.open(css, options(480, 1.0), recorder(got));
    pumpUntil([&] { return got.geometry.has_value(); });
    const auto laidOut = Clock::now();
    const std::array<std::size_t, 1> zero{0};
    handle.request(zero);
    pumpUntil([&] { return got.tiles.size() == 1; });
    const auto end = Clock::now();
    std::println(
        "2000 lines: layout {} ms, first tile {} ms after open ({} tiles of {} px)",
        std::chrono::duration_cast<std::chrono::milliseconds>(laidOut - start).count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), got.geometry->tileCount,
        got.geometry->width
    );
  }
} // namespace

int main() {
  testLayoutAndTilesArriveOnTheMainContext();
  testNewerRequestReplacesOlder();
  testRelayoutDropsTheOlderRevision();
  testCloseStopsEvents();
  testBoundsAndErrors();
  testWorkerWaitsForTheMainContext();
  testTwoDocuments();
  testServiceGoesFirst();
  measureFirstTile();
  std::println("text-tile-service: all checks passed");
  return 0;
}
