#include "test_check.h"
#include "workbench/circle_pack.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <expected>
#include <format>
#include <limits>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using luciole::test::WorkbenchRow;
  using luciole::test::workbenchTree;

  constexpr double kExact = 1e-12;
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInfinity = std::numeric_limits<double>::infinity();

  std::vector<double> radiiFrom(const std::vector<double>& weights) {
    std::vector<double> radii;
    radii.reserve(weights.size());
    for (const auto weight : weights) {
      radii.push_back(std::sqrt(weight));
    }
    return radii;
  }

  template <typename Make> std::vector<double> decreasing(std::size_t count, Make make) {
    std::vector<double> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      values.push_back(make(static_cast<double>(i)));
    }
    std::ranges::sort(values, std::greater{});
    return values;
  }

  std::vector<Circle> packed(const std::vector<double>& radii) {
    const auto result = packCircles(radii);
    TEST_CHECK(result.has_value());
    return result.value_or(std::vector<Circle>{});
  }

  bool failsWith(const std::expected<std::vector<Circle>, PackError>& result, PackError expected) {
    return !result.has_value() && result.error() == expected;
  }

  // Every circle stays inside the unit circle and no two overlap.
  void checkPacking(const std::vector<Circle>& circles) {
    for (std::size_t i = 0; i < circles.size(); ++i) {
      TEST_CHECK(std::hypot(circles[i].x, circles[i].y) + circles[i].r <= 1 + 1e-6);
      for (std::size_t j = i + 1; j < circles.size(); ++j) {
        const double distance = std::hypot(circles[i].x - circles[j].x, circles[i].y - circles[j].y);
        TEST_CHECK(distance >= circles[i].r + circles[j].r - 1e-6);
      }
    }
  }

  void checkCircle(const Circle& actual, double x, double y, double r) {
    TEST_NEAR(actual.x, x, kExact);
    TEST_NEAR(actual.y, y, kExact);
    TEST_NEAR(actual.r, r, kExact);
  }

  void testEmptyOneTwo() {
    TEST_CHECK(packed({}).empty());
    const auto one = packed({3});
    TEST_CHECK(one.size() == 1);
    TEST_NEAR(one[0].r, 1.0, 1e-9);
    checkPacking(packed({2, 1}));
  }

  void testMixedSizesDoNotOverlap() {
    checkPacking(packed(radiiFrom(decreasing(61, [](double i) { return 1000 / (1 + i * i * 0.3); }))));
  }

  void testEqualCircles() {
    const auto circles = packed(std::vector<double>(60, 1.0));
    checkPacking(circles);
    TEST_CHECK(circles[0].r > 0.1);
  }

  void testHeavySkew() {
    checkPacking(packed(radiiFrom(decreasing(61, [](double i) { return std::pow(10.0, 9 - i * 0.15); }))));
  }

  void testSameInputSameOutput() {
    const auto radii = radiiFrom(decreasing(45, [](double i) { return 500 / (i + 1); }));
    const auto first = packed(radii);
    const auto second = packed(std::vector<double>(radii));
    TEST_CHECK(first == second);
  }

  void testRejectsNonPositiveRadius() {
    TEST_CHECK(failsWith(packCircles(std::vector<double>{1, 0}), PackError::BadRadius));
    TEST_CHECK(failsWith(packCircles(std::vector<double>{-1}), PackError::BadRadius));
    TEST_CHECK(failsWith(packCircles(std::vector<double>{1, kNan}), PackError::BadRadius));
    TEST_CHECK(failsWith(packCircles(std::vector<double>{kInfinity, 1}), PackError::BadRadius));
    TEST_CHECK(!failsWith(packCircles(std::vector<double>{1}), PackError::BadRadius));
  }

  void testPacksQuickly() {
    const auto radii = radiiFrom(decreasing(61, [](double i) { return 1000 / (1 + i); }));
    const std::clock_t start = std::clock();
    for (int run = 0; run < 20; ++run) {
      TEST_CHECK(packCircles(radii).has_value());
    }
    const double perPack = 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC / 20;
    std::println("  pack(61) {:.2f} ms", perPack);
    TEST_CHECK(perPack < 25);
  }

  // The enclosing circle touches at least two circles, or the only one.
  void testEnclosingCircleIsTight() {
    const std::vector<std::vector<double>> cases{
        {3, 1},
        decreasing(61, [](double i) { return 1000 / (1 + i * i * 0.3); }),
        decreasing(30, [](double i) { return 100 / (1 + i); }),
        std::vector<double>(37, 1.0),
    };
    for (const auto& weights : cases) {
      const auto circles = packed(radiiFrom(weights));
      std::vector<double> gaps;
      gaps.reserve(circles.size());
      for (const auto& circle : circles) {
        gaps.push_back(1 - (std::hypot(circle.x, circle.y) + circle.r));
      }
      std::ranges::sort(gaps);
      TEST_CHECK(gaps[0] > -1e-6);
      TEST_CHECK(gaps[0] < 1e-6 && gaps[1] < 1e-3);
    }
    TEST_NEAR(packed({5})[0].r, 1.0, 1e-9);
  }

  void testExactPositions() {
    const auto two = packed({2, 1});
    checkCircle(two[0], -0.33333330702514502, 0, 0.66666665351257248);
    checkCircle(two[1], 0.66666667324371376, 0, 0.33333332675628624);
    const auto three = packed({3, 2, 1});
    checkCircle(three[0], -0.40000000542626335, 2.1705054244864303e-8, 0.59999999457373598);
    checkCircle(three[1], 0.59999998552996336, 2.1705054244864303e-8, 0.39999999638249067);
    checkCircle(three[2], 0.23999998878572176, -0.47999997395393446, 0.19999999819124534);
    const auto five = packed({5, 4, 3, 2, 1});
    checkCircle(five[0], -0.43941364301419400, 0.094570820574618977, 0.55052475651452348);
    checkCircle(five[1], 0.55153091871194837, 0.094570820574618977, 0.44041980521161878);
    checkCircle(five[2], 0.14781276393463103, -0.56196826148564483, 0.33031485390871407);
    checkCircle(five[3], -0.39105769051281725, -0.67464541471102579, 0.22020990260580939);
    checkCircle(five[4], 0.12334499697842996, 0.44059729927905683, 0.11010495130290469);
    // The overlap slack is absolute, so tiny radii can settle a circle elsewhere.
    const auto tiny = packed({0.005, 0.004, 0.003, 0.002, 0.001});
    checkCircle(tiny[3], -0.39105769758158959, -0.67464541895459196, 0.22020990238832538);
    checkCircle(tiny[4], -0.68273267299712781, -0.51961764333497695, 0.11010495119416269);
    const auto small = packed(decreasing(30, [](double i) { return 0.01 / (1 + i); }));
    checkPacking(small);
    checkCircle(small[5], -0.39322779263195101, -0.71593211591202122, 0.10715322185549087);
    checkCircle(small[17], -0.96427016934359722, -0.0048244507323706928, 0.035717740618496953);
    checkCircle(small[29], -0.86914002293197157, 0.34542894268705171, 0.021430644371098172);
    const auto mixed = packed(radiiFrom(decreasing(61, [](double i) { return 1000 / (1 + i * i * 0.3); })));
    checkCircle(mixed[0], -0.29709529081483016, 0.051834738662332770, 0.47013443798492016);
    checkCircle(mixed[1], 0.58537432615716745, 0.051834738662332770, 0.41233517898707744);
    // NOLINTNEXTLINE(modernize-use-std-numbers): a measured position, not a constant.
    checkCircle(mixed[2], 0.19379942845388809, -0.56342717984256874, 0.31696457342866680);
    checkCircle(mixed[30], -0.64980285130540461, 0.61173676925685050, 0.028558633269841829);
    checkCircle(mixed[60], -0.045152301952151419, 0.46559868042721492, 0.014299116944827297);
  }

  // Radii too far apart for double precision leave a circle without a spot; a wide but workable spread packs.
  void testExtremeSpreadReportsNoSpot() {
    TEST_CHECK(failsWith(packCircles(std::vector<double>{1e15, 1, 1}), PackError::NoSpot));
    TEST_CHECK(failsWith(packCircles(std::vector<double>{1e300, 1e-300, 1e-300}), PackError::NoSpot));
    const auto wide = packed({1e15, 1e7, 1});
    TEST_NEAR(wide[0].r, 0.99999995478113812, kExact);
    TEST_NEAR(wide[2].r, 9.9999995478113812e-16, 1e-27);
  }

  // Ids: 0 top, 1 home, 2 etc, 3 docs, 4 music, 5 notes.txt, 6 sub, 7 .cache, 8..77 f00..f69 (in docs),
  // 78..82 s0..s4 (in sub), 83 song.ogg (in music), 84 void (an empty folder in sub).
  constexpr NodeId kTop = 0;
  constexpr NodeId kHome = 1;
  constexpr NodeId kDocs = 3;
  constexpr NodeId kNotes = 5;
  constexpr NodeId kSub = 6;
  constexpr NodeId kCache = 7;
  constexpr NodeId kFirstFile = 8;
  constexpr NodeId kVoid = 84;

  Tree atlasTree() {
    std::vector<std::string> names;
    names.reserve(75);
    for (int i = 0; i < 70; ++i) {
      names.push_back(std::format("f{:02}", i));
    }
    for (int i = 0; i < 5; ++i) {
      names.push_back(std::format("s{}", i));
    }
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true, .bytes = 9000},
        {.parent = 0, .name = "home", .folder = true, .bytes = 6000},
        {.parent = 0, .name = "etc", .folder = true, .bytes = 800},
        {.parent = 1, .name = "docs", .folder = true, .bytes = 3000},
        {.parent = 1, .name = "music", .folder = true, .bytes = 1500},
        {.parent = 1, .name = "notes.txt", .bytes = 200},
        {.parent = 3, .name = "sub", .folder = true, .bytes = 2000},
        {.parent = 3, .name = ".cache", .folder = true, .bytes = 5000},
    };
    for (int i = 0; i < 70; ++i) {
      rows.push_back({.parent = 3, .name = names[static_cast<std::size_t>(i)], .bytes = 1000.0 - 10.0 * i});
    }
    for (int i = 0; i < 5; ++i) {
      rows.push_back({.parent = 6, .name = names[static_cast<std::size_t>(70 + i)], .bytes = 500.0 - 50.0 * i});
    }
    rows.push_back({.parent = 4, .name = "song.ogg", .bytes = 900});
    rows.push_back({.parent = 6, .name = "void", .folder = true});
    return workbenchTree(rows, 0);
  }

  void checkItem(const std::optional<PackedItem>& item, double x, double y, double r) {
    TEST_CHECK(item.has_value());
    TEST_NEAR(item->x, x, kExact);
    TEST_NEAR(item->y, y, kExact);
    TEST_NEAR(item->r, r, kExact);
  }

  void checkItem(const PackedItem& item, NodeId node, double x, double y, double r) {
    TEST_CHECK(item.node == node);
    TEST_CHECK(!item.more);
    checkItem(std::optional(item), x, y, r);
  }

  void testFolderPackingShowsSixtyAndMore() {
    const auto tree = atlasTree();
    PackingCache cache;
    const std::vector<NodeId> path{kTop, kHome, kDocs};
    const auto docs = cache.packing(tree, kDocs, {}, path);
    TEST_CHECK(!docs->failed);
    TEST_CHECK(docs->items.size() == 61);
    checkItem(docs->items[0], kSub, 0.032983114160962688, 0.050274103122416883, 0.12916849408119777);
    checkItem(docs->items[1], kFirstFile, 0.27078249971659119, 0.050274103122416883, 0.10863089147443071);
    checkItem(docs->items[2], kFirstFile + 1, 0.17151002228929299, -0.14267510954335599, 0.10835856320688572);
    checkItem(docs->items[59], kFirstFile + 57, -0.77158477704038975, -0.078054562554144141, 0.087996245309515206);
    checkItem(docs->items[60], kFirstFile + 58, -0.68016382042358103, -0.38448139808601184, 0.087481324169999639);
    TEST_CHECK(docs->items[30].more);
    TEST_CHECK(docs->items[30].node == kNoNode);
    TEST_CHECK(std::ranges::count_if(docs->items, &PackedItem::more) == 1);
    std::vector<NodeId> rest;
    for (NodeId id = kFirstFile + 59; id < kFirstFile + 70; ++id) {
      rest.push_back(id);
    }
    TEST_CHECK(docs->rest == rest);
    TEST_CHECK(!docs->placement(kCache).has_value());
    TEST_CHECK(!docs->placement(kFirstFile + 65).has_value());
  }

  void testAncestorPackings() {
    const auto tree = atlasTree();
    PackingCache cache;
    const std::vector<NodeId> path{kTop, kHome, kDocs};
    const auto home = cache.packing(tree, kHome, {}, path);
    TEST_CHECK(home->items.size() == 3 && home->rest.empty());
    checkItem(home->items[0], kDocs, -0.41112633327522963, 0.060000024078883434, 0.48887364989528187);
    checkItem(home->items[1], 4, 0.48887365831002588, 0.060000024078883434, 0.41112634168997358);
    checkItem(home->items[2], kNotes, 0.099231655862800666, -0.47249640797756237, 0.24870192401552679);
    const auto top = cache.packing(tree, kTop, {}, path);
    checkItem(top->items[0], kHome, -0.33905614706357790, 0.059999999999999998, 0.56094385293642213);
    checkItem(top->items[1], 2, 0.56094384748407933, 0.059999999999999998, 0.33905614161123510);
  }

  // A node on the forced path is packed even when it ranks past the first 60 or is hidden.
  void testForcedNodesArePacked() {
    const auto tree = atlasTree();
    PackingCache cache;
    const NodeId late = kFirstFile + 65;
    const auto forced = cache.packing(tree, kDocs, {}, std::vector<NodeId>{kTop, kHome, kDocs, late});
    TEST_CHECK(forced->items.size() == 62);
    checkItem(forced->placement(late), -0.76021236404783943, 0.29424220702895865, 0.083600537073675540);
    TEST_CHECK(forced->rest.size() == 10);
    TEST_CHECK(!std::ranges::contains(forced->rest, late));
    const auto hidden = cache.packing(tree, kDocs, {}, std::vector<NodeId>{kTop, kHome, kDocs, kCache});
    TEST_CHECK(hidden->items.size() == 62);
    checkItem(hidden->placement(kCache), 0.0043594492278607620, 0.064736155675752874, 0.15724146508094256);
    const auto direct = packFolder(tree, kDocs, {}, std::vector<NodeId>{kCache});
    checkItem(direct.placement(kCache), 0.0043594492278607620, 0.064736155675752874, 0.15724146508094256);
  }

  void testMoreWeight() {
    TEST_NEAR(moreWeight(std::vector<double>{4, 1, 9}, 100), 4.0, 0.0);
    TEST_NEAR(moreWeight(std::vector<double>{4, 1, 9}, 0.5), 1.0, 0.0);
    TEST_NEAR(moreWeight(std::vector<double>{4, 1, 9}, 2), 2.0, 0.0);
    TEST_NEAR(moreWeight(std::vector<double>{4, 1, 9, 16}, 100), 9.0, 0.0);
    TEST_NEAR(moreWeight(std::vector<double>{}, 7), 7.0, 0.0);
  }

  void testCacheKeepsPackingsUntilSettingsChange() {
    const auto tree = atlasTree();
    PackingCache cache;
    LayoutSettings settings;
    const auto first = cache.packing(tree, kSub, settings, {});
    TEST_CHECK(cache.packing(tree, kSub, settings, {}) == first);
    TEST_CHECK(cache.size() == 1);
    settings.colorBy = luciole::WorkbenchColor::Bytes;
    TEST_CHECK(cache.packing(tree, kSub, settings, {}) == first);
    settings.emphasis = 1;
    const auto heavier = cache.packing(tree, kSub, settings, {});
    TEST_CHECK(heavier != first);
    TEST_CHECK(cache.size() == 1);
    TEST_CHECK(heavier->items[0].r > first->items[0].r);
    settings.showHidden = true;
    TEST_CHECK(cache.packing(tree, kSub, settings, {}) != heavier);
    settings.sizeBy = luciole::WorkbenchMeasure::Equal;
    const auto equal = cache.packing(tree, kSub, settings, {});
    TEST_NEAR(equal->items[0].r, equal->items[5].r, kExact);
    cache.clear();
    TEST_CHECK(cache.size() == 0);
    TEST_CHECK(cache.packing(tree, kSub, settings, {}) != equal);
    // A NaN emphasis is the same setting each time, so the packing is kept.
    settings.emphasis = kNan;
    const auto unweighted = cache.packing(tree, kSub, settings, {});
    TEST_CHECK(cache.packing(tree, kSub, settings, {}) == unweighted);
    settings.emphasis = 0.5;
    TEST_CHECK(cache.packing(tree, kSub, settings, {}) != unweighted);
  }

  void testCacheEvictsOldest() {
    std::vector<std::string> names;
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "root", .folder = true}};
    names.reserve(kAtlasMaxCached + 1);
    for (std::size_t i = 0; i <= kAtlasMaxCached; ++i) {
      names.push_back(std::format("d{}", i));
    }
    for (const auto& name : names) {
      rows.push_back({.parent = 0, .name = name, .folder = true});
    }
    const auto tree = workbenchTree(rows, 0);
    PackingCache cache;
    const auto oldest = cache.packing(tree, 1, {}, {});
    const auto second = cache.packing(tree, 2, {}, {});
    for (NodeId id = 3; id <= kAtlasMaxCached + 1; ++id) {
      static_cast<void>(cache.packing(tree, id, {}, {}));
    }
    TEST_CHECK(cache.size() == kAtlasMaxCached);
    TEST_CHECK(cache.packing(tree, 2, {}, {}) == second);
    TEST_CHECK(cache.packing(tree, 1, {}, {}) != oldest);
    TEST_CHECK(cache.size() == kAtlasMaxCached);
  }

  void testEmptyAndUnknownFolders() {
    const auto tree = atlasTree();
    PackingCache cache;
    const auto empty = cache.packing(tree, kVoid, {}, {});
    TEST_CHECK(empty->items.empty() && empty->rest.empty() && !empty->failed);
    TEST_CHECK(cache.packing(tree, kNotes, {}, {})->items.empty());
    TEST_CHECK(cache.packing(tree, 9999, {}, {})->items.empty());
    TEST_CHECK(cache.packing(tree, kNoNode, {}, std::vector<NodeId>{kNoNode, 9999})->items.empty());
    const auto unknownExtra = packFolder(tree, kSub, {}, std::vector<NodeId>{9999});
    TEST_CHECK(unknownExtra.items.size() == 6);
    // Only the folder's own children are packed: never the top for an unknown folder, nor a stranger.
    const auto noFolder = cache.packing(tree, kNoNode, {}, std::vector<NodeId>{kTop});
    TEST_CHECK(noFolder->items.empty() && noFolder->rest.empty() && !noFolder->placement(kTop));
    TEST_CHECK(packFolder(tree, kNoNode, {}, std::vector<NodeId>{kTop}).items.empty());
    const auto stranger = packFolder(tree, kSub, {}, std::vector<NodeId>{kNotes});
    TEST_CHECK(stranger.items.size() == 6);
    TEST_CHECK(!stranger.placement(kNotes));
    const auto one = cache.packing(tree, 4, {}, {});
    TEST_CHECK(one->items.size() == 1);
    checkItem(one->items[0], 83, 0, 0.06, 0.9);
  }

  void testUnpackableWeightsFail() {
    const auto tree = workbenchTree(
        {
            {.parent = -1, .name = "root", .folder = true},
            {.parent = 0, .name = "huge", .bytes = 1e30},
            {.parent = 0, .name = "a", .bytes = 0},
            {.parent = 0, .name = "b", .bytes = 0},
        },
        0
    );
    LayoutSettings settings;
    settings.emphasis = 1;
    PackingCache cache;
    const auto packing = cache.packing(tree, 0, settings, {});
    TEST_CHECK(packing->failed);
    TEST_CHECK(packing->items.empty());
    TEST_CHECK(!packing->placement(1).has_value());
  }
} // namespace

int main() {
  testEmptyOneTwo();
  testMixedSizesDoNotOverlap();
  testEqualCircles();
  testHeavySkew();
  testSameInputSameOutput();
  testRejectsNonPositiveRadius();
  testPacksQuickly();
  testEnclosingCircleIsTight();
  testExactPositions();
  testExtremeSpreadReportsNoSpot();
  testFolderPackingShowsSixtyAndMore();
  testAncestorPackings();
  testForcedNodesArePacked();
  testMoreWeight();
  testCacheKeepsPackingsUntilSettingsChange();
  testCacheEvictsOldest();
  testEmptyAndUnknownFolders();
  testUnpackableWeightsFail();
  std::println("workbench circle pack checks passed");
  return 0;
}
