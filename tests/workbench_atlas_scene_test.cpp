#include "test_check.h"
#include "workbench/atlas_scene.h"
#include "workbench_atlas_fixture.h"

#include <algorithm>
#include <cstddef>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::atlas;

  void testFadeRamp() {
    TEST_NEAR(fadeRamp(30, 39, 49), 0.0, 0.0);
    TEST_NEAR(fadeRamp(39, 39, 49), 0.0, 0.0);
    TEST_NEAR(fadeRamp(44, 39, 49), 0.5, 1e-15);
    TEST_NEAR(fadeRamp(49, 39, 49), 1.0, 0.0);
    TEST_NEAR(fadeRamp(80, 39, 49), 1.0, 0.0);
    TEST_NEAR(fadeRamp(kNan, 39, 49), 0.0, 0.0);
  }

  void checkItem(const SceneItem& item, BubbleRole role, NodeId node, double x, double y, double r) {
    TEST_CHECK(item.role == role);
    TEST_CHECK(item.node == node);
    TEST_NEAR(item.x, x, kExact);
    TEST_NEAR(item.y, y, kExact);
    TEST_NEAR(item.r, r, kExact);
  }

  std::size_t countRole(const AtlasScene& scene, BubbleRole role) {
    return static_cast<std::size_t>(std::ranges::count(scene.items, role, &SceneItem::role));
  }

  void testSceneAtRest() {
    World world;
    const auto scene = buildScene(world.source, kDocs, restCamera({600, 600}), {600, 600});
    TEST_CHECK(scene.items.size() == 66);
    TEST_CHECK(countRole(scene, BubbleRole::Ancestor) == 2);
    TEST_CHECK(countRole(scene, BubbleRole::Sibling) == 2);
    TEST_CHECK(countRole(scene, BubbleRole::Self) == 1);
    TEST_CHECK(countRole(scene, BubbleRole::Bubble) == 60);
    TEST_CHECK(countRole(scene, BubbleRole::More) == 1);
    const auto& items = scene.items;
    checkItem(items[0], BubbleRole::Ancestor, kTop, 894.12403640857553, 202.32383380249698, 1042.9176331056617);
    TEST_CHECK(items[0].level == 2 && items[0].depth == -2 && items[0].clickable && !items[0].huge);
    checkItem(items[1], BubbleRole::Ancestor, kHome, 540.51640202310375, 264.89889178883669, 585.01823540962380);
    TEST_CHECK(items[1].level == 1 && items[1].depth == -1);
    checkItem(items[2], BubbleRole::Sibling, kMusic, 826.51640694588241, 300, 240.51640694588241);
    TEST_NEAR(items[2].fade, 0.6, 1e-15);
    TEST_CHECK(items[2].depth == 1 && items[2].clickable && items[2].kids && items[2].labeledKids);
    checkItem(items[3], BubbleRole::Sibling, kNotes, 598.56873023273442, -11.520123043582544, 145.49516073054181);
    TEST_CHECK(!items[3].kids);
    checkItem(items[4], BubbleRole::Self, kDocs, 300, 300, 286);
    TEST_CHECK(items[4].fade == 1 && items[4].depth == 0 && items[4].kids && items[4].labeledKids && !items[4].empty);
    checkItem(items[5], BubbleRole::Bubble, kSub, 309.43317065003532, 314.37839349301123, 36.942189307222563);
    TEST_CHECK(items[5].clickable && !items[5].kids && items[5].depth == 1);
    checkItem(items[6], BubbleRole::Bubble, kFirstFile, 377.44379491894506, 314.37839349301123, 31.068434961687185);
    checkItem(items[35], BubbleRole::More, kNoNode, 142.09002658947205, 351.33859135570196, 28.621650452380308);
    TEST_CHECK(items[35].folder == kDocs && items[35].restCount == 11 && items[35].clickable);
    checkItem(
        items[65], BubbleRole::Bubble, kFirstFile + 58, 105.47314735885581, 190.03832014740061, 25.019658712619897
    );
    TEST_CHECK((scene.camera == Camera{.cx = 0, .cy = 0, .scale = 286}));
    TEST_CHECK(scene.view.width == 600 && scene.view.height == 600);
  }

  void testHitAndFocus() {
    World world;
    const auto scene = buildScene(world.source, kDocs, restCamera({600, 600}), {600, 600});
    TEST_CHECK(hitTest(scene, 300, 300) == 5U);
    TEST_CHECK(hitTest(scene, 5, 5) == 0U);
    TEST_CHECK(hitTest(scene, 380, 314) == 6U);
    TEST_CHECK(findFocus(scene, world.tree, kFirstS) == 5U);
    TEST_CHECK(findFocus(scene, world.tree, kFirstFile) == 6U);
    TEST_CHECK(findFocus(scene, world.tree, kDocs) == 4U);
    TEST_CHECK(findFocus(scene, world.tree, kMusic) == 2U);
    TEST_CHECK(findFocus(scene, world.tree, kSong) == 2U);
    TEST_CHECK(!findFocus(scene, world.tree, kFirstFile + 65));
    TEST_CHECK(!findFocus(scene, world.tree, kTop));
    TEST_CHECK(!findFocus(scene, world.tree, kHome));
    TEST_CHECK(!findFocus(scene, world.tree, kNoNode));
  }

  void testZoomedIntoAChildFolder() {
    World world;
    const auto sub = world.packings.packing(world.tree, kDocs, world.settings, std::vector<NodeId>{kTop, kHome, kDocs})
                         ->placement(kSub);
    const auto scene = buildScene(world.source, kDocs, {.cx = sub->x, .cy = sub->y, .scale = 250 / sub->r}, {600, 600});
    TEST_CHECK(scene.items.size() == 16);
    checkItem(scene.items[3], BubbleRole::Bubble, kSub, 300, 300, 250);
    TEST_CHECK(scene.items[3].kids && scene.items[3].labeledKids);
    checkItem(
        scene.items[4], BubbleRole::Grandchild, kFirstS, 292.01689881232784, 324.17451789488143, 78.931333548859769
    );
    TEST_CHECK(scene.items[4].depth == 2 && scene.items[4].clickable && scene.items[4].fade == 1);
    checkItem(
        scene.items[8], BubbleRole::Grandchild, kFirstS + 4, 144.78979343535116, 305.37253870173572, 69.491491445353603
    );
    checkItem(
        scene.items[9], BubbleRole::Grandchild, kVoid, 206.03376918274500, 365.99698434374704, 16.683609220375502
    );
    TEST_CHECK(!scene.items[9].empty && !scene.items[9].kids);
  }

  // Children fade in between 39 and 49 px of radius and get stronger and clickable from 110 px.
  void testChildrenFadeInWithRadius() {
    World world;
    const auto sub = world.packings.packing(world.tree, kDocs, world.settings, std::vector<NodeId>{kTop, kHome, kDocs})
                         ->placement(kSub);
    const auto childOf = [&](double radius) {
      const auto scene =
          buildScene(world.source, kDocs, {.cx = sub->x, .cy = sub->y, .scale = radius / sub->r}, {600, 600});
      const auto found = std::ranges::find(scene.items, kFirstS, &SceneItem::node);
      TEST_CHECK(found != scene.items.end());
      return *found;
    };
    const auto faint = childOf(46);
    TEST_NEAR(faint.fade, 0.294, 1e-12);
    TEST_CHECK(!faint.clickable && faint.role == BubbleRole::Grandchild);
    TEST_NEAR(faint.r, 14.523365372990195, kExact);
    const auto almost = childOf(105);
    TEST_NEAR(almost.fade, 0.565, 1e-12);
    TEST_CHECK(!almost.clickable);
    const auto labeled = childOf(110.5);
    TEST_NEAR(labeled.fade, 0.7245, 1e-12);
    TEST_CHECK(labeled.clickable);
    checkItem(labeled, BubbleRole::Grandchild, kFirstS, 296.47146927504889, 310.68513690953762, 34.887649428596021);
  }

  void testSiblingChildren() {
    World world;
    const auto scene = buildScene(world.source, kDocs, {.cx = 1.6, .cy = 0, .scale = 300}, {600, 600});
    TEST_CHECK(scene.items.size() == 18);
    checkItem(scene.items[2], BubbleRole::Sibling, kMusic, 372.28993735582071, 300, 252.28993735582068);
    checkItem(
        scene.items[3], BubbleRole::SiblingChild, kSong, 372.28993735582071, 315.13739624134922, 227.06094362023862
    );
    TEST_NEAR(scene.items[3].fade, 0.6, 1e-15);
    TEST_CHECK(scene.items[3].depth == 2 && scene.items[3].clickable);
    checkItem(scene.items[5], BubbleRole::Self, kDocs, -180, 300, 300);
    checkItem(
        scene.items[6], BubbleRole::Bubble, kFirstFile + 14, -32.588035318971805, 315.12462534622227, 31.384619431443813
    );
  }

  // Ancestor rims too large to draw cover the view, and the parent's other children are left out.
  void testHugeAncestors() {
    World world;
    const auto scene = buildScene(world.source, kDocs, {.cx = 0, .cy = 0, .scale = 40000}, {600, 600});
    TEST_CHECK(scene.items.size() == 5);
    TEST_CHECK(scene.items[0].huge && scene.items[1].huge);
    TEST_CHECK(countRole(scene, BubbleRole::Sibling) == 0);
    checkItem(scene.items[3], BubbleRole::Bubble, kSub, scene.items[3].x, scene.items[3].y, 5166.7397632479106);
    checkItem(
        scene.items[4], BubbleRole::Grandchild, kFirstS + 3, scene.items[4].x, scene.items[4].y, 1492.4279262948396
    );
    TEST_CHECK(hitTest(scene, 590, 590) == 4U);
    TEST_CHECK(hitTest(scene, -1e6, -1e6) == 1U);
  }

  void testFileAndEmptyFolderAsCurrent() {
    World world;
    const auto file = buildScene(world.source, kNotes, restCamera({600, 600}), {600, 600});
    const std::vector<BubbleRole> fileRoles{BubbleRole::Ancestor,     BubbleRole::Ancestor,     BubbleRole::Sibling,
                                            BubbleRole::SiblingChild, BubbleRole::SiblingChild, BubbleRole::Sibling,
                                            BubbleRole::SiblingChild, BubbleRole::Self};
    std::vector<BubbleRole> roles;
    roles.reserve(file.items.size());
    for (const auto& item : file.items) {
      roles.push_back(item.role);
    }
    TEST_CHECK(roles == fileRoles);
    TEST_CHECK(!file.items.back().kids && !file.items.back().empty);
    const auto empty = buildScene(world.source, kVoid, restCamera({600, 600}), {600, 600});
    TEST_CHECK(empty.items.size() == 7);
    TEST_CHECK(countRole(empty, BubbleRole::Ancestor) == 4);
    TEST_CHECK(empty.items.back().role == BubbleRole::Self);
    TEST_CHECK(empty.items.back().empty && !empty.items.back().kids);
  }

  void testDegenerateScenes() {
    World world;
    const Camera rest = restCamera({600, 600});
    TEST_CHECK(buildScene(world.source, 9999, rest, {600, 600}).items.empty());
    TEST_CHECK(buildScene(world.source, kNoNode, rest, {600, 600}).items.empty());
    TEST_CHECK(buildScene(world.source, kDocs, rest, {0, 600}).items.empty());
    TEST_CHECK(buildScene(world.source, kDocs, rest, {600, -1}).items.empty());
    TEST_CHECK(buildScene(world.source, kDocs, rest, {kNan, 600}).items.empty());
    TEST_CHECK(buildScene(world.source, kDocs, rest, {kInfinity, 600}).items.empty());
    TEST_CHECK(buildScene(world.source, kDocs, {.cx = kNan, .cy = 0, .scale = 286}, {600, 600}).items.empty());
    TEST_CHECK(buildScene(world.source, kDocs, {.cx = 0, .cy = 0, .scale = 0}, {600, 600}).items.empty());
    TEST_CHECK(buildScene(world.source, kDocs, {.cx = 0, .cy = 0, .scale = kInfinity}, {600, 600}).items.empty());
    const auto scene = buildScene(world.source, kDocs, rest, {0, 600});
    TEST_CHECK(!hitTest(scene, 0, 0));
    TEST_CHECK(!findFocus(scene, world.tree, kDocs));
    // A parent link that loops back stays bounded.
    auto looped = atlasTree();
    looped.nodes[kTop].parent = kDocs;
    PackingCache packings;
    const AtlasSource source{.tree = looped, .settings = world.settings, .packings = packings};
    const auto loopScene = buildScene(source, kDocs, rest, {600, 600});
    TEST_CHECK(loopScene.items.size() <= 2 * looped.nodes.size() + 8);
    TEST_CHECK(treeDistance(looped, kSub, kMusic) <= 2 * looped.nodes.size());
    // A folder listed as its own child nests without end toward (0, 0.6); the walk stops at 2n + 8 items.
    Tree selfNested;
    selfNested.nodes.push_back({.name = "loop", .childCount = 1, .bytes = 1, .folder = true});
    selfNested.children = {0};
    selfNested.root = 0;
    PackingCache nestedPackings;
    const AtlasSource nested{.tree = selfNested, .settings = world.settings, .packings = nestedPackings};
    const auto nestedScene = buildScene(nested, 0, {.cx = 0, .cy = 0.6, .scale = 1e9}, {600, 600});
    TEST_CHECK(nestedScene.items.size() == 2 * selfNested.nodes.size() + 8);
  }

  // A folder shows what it holds from 44 px of radius on, faintly at first.
  void testContentsFromFortyFourPixels() {
    World world;
    const auto sub = world.packings.packing(world.tree, kDocs, world.settings, std::vector<NodeId>{kTop, kHome, kDocs})
                         ->placement(kSub);
    const auto sceneAt = [&](double radius) {
      return buildScene(world.source, kDocs, {.cx = sub->x, .cy = sub->y, .scale = radius / sub->r}, {600, 600});
    };
    const auto below = sceneAt(43.9);
    TEST_CHECK(std::ranges::find(below.items, kFirstS, &SceneItem::node) == below.items.end());
    const auto subBelow = std::ranges::find(below.items, kSub, &SceneItem::node);
    TEST_CHECK(subBelow != below.items.end() && !subBelow->kids && !subBelow->empty);
    const auto from = sceneAt(44);
    const auto first = std::ranges::find(from.items, kFirstS, &SceneItem::node);
    TEST_CHECK(first != from.items.end());
    TEST_NEAR(first->fade, 0.21, 1e-12);
  }

  // A scene built with a margin keeps circles just beyond the view, in the view's own coordinates.
  void testSceneAround() {
    World world;
    const ViewSize view{.width = 600, .height = 400};
    const Camera camera{.cx = 0.3, .cy = -0.2, .scale = 700};
    const auto plain = buildScene(world.source, kHome, camera, view);
    const auto around = buildSceneAround(world.source, kHome, camera, view, 0.25);
    TEST_CHECK(around.view.width == 600 && around.view.height == 400);
    TEST_CHECK(around.camera == camera);
    TEST_CHECK(around.items.size() > plain.items.size());
    for (const auto& item : plain.items) {
      const auto same = std::ranges::find_if(around.items, [&item](const SceneItem& other) {
        return other.node == item.node && other.role == item.role && other.folder == item.folder;
      });
      TEST_CHECK(same != around.items.end());
      TEST_NEAR(same->x, item.x, 1e-9);
      TEST_NEAR(same->y, item.y, 1e-9);
      TEST_NEAR(same->r, item.r, 1e-9);
    }
    const bool beyond = std::ranges::any_of(around.items, [&view](const SceneItem& item) {
      return item.x + item.r < 0
          || item.x - item.r > view.width
          || item.y + item.r < 0
          || item.y - item.r > view.height;
    });
    TEST_CHECK(beyond);
    for (const auto& item : around.items) {
      TEST_CHECK(item.x + item.r >= -0.25 * 600 - 2 && item.x - item.r <= 1.25 * 600 + 2);
      TEST_CHECK(item.y + item.r >= -0.25 * 400 - 2 && item.y - item.r <= 1.25 * 400 + 2);
    }
    // No margin builds the plain scene; a margin that cannot be used reads as none.
    TEST_CHECK(buildSceneAround(world.source, kHome, camera, view, 0).items.size() == plain.items.size());
    TEST_CHECK(buildSceneAround(world.source, kHome, camera, view, kNan).items.size() == plain.items.size());
    TEST_CHECK(buildSceneAround(world.source, kHome, camera, view, -1).items.size() == plain.items.size());
    TEST_CHECK(buildSceneAround(world.source, kHome, camera, {0, 400}, 0.25).items.empty());
  }
} // namespace

int main() {
  testFadeRamp();
  testSceneAtRest();
  testHitAndFocus();
  testZoomedIntoAChildFolder();
  testChildrenFadeInWithRadius();
  testSiblingChildren();
  testHugeAncestors();
  testFileAndEmptyFolderAsCurrent();
  testDegenerateScenes();
  testContentsFromFortyFourPixels();
  testSceneAround();
  std::println("workbench atlas scene checks passed");
  return 0;
}
