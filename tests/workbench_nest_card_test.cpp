#include "test_check.h"
#include "workbench/nest_card.h"
#include "workbench/scene_shapes.h"

#include <algorithm>
#include <cstddef>
#include <print>
#include <string>

// The peek card laid over the Nest's scene: which of the scene's labels it hides, and the order of the result.
namespace {
  using namespace luciole::workbench;

  constexpr TreemapRect kCard{.x = 100, .y = 100, .w = 200, .h = 100};

  // A 12 px label whose box is `width` wide, centered on (cx, cy).
  ShapeLabel label(NodeId node, std::string text, float cx, float cy, float width = 40, float rotation = 0) {
    constexpr float kHeight = 16;
    return {
        .node = node,
        .text = std::move(text),
        .x = cx - width / 2,
        .y = cy - kHeight / 2,
        .width = width,
        .height = kHeight,
        .align = LabelAlign::Start,
        .fontSize = 12,
        .rotation = rotation,
        .color = {},
        .icon = {},
    };
  }

  bool shows(const SceneShapes& out, const std::string& text) {
    return std::ranges::any_of(out.labels, [&text](const ShapeLabel& each) { return each.text == text; });
  }

  SceneShapes over(SceneShapes scene) {
    SceneShapes card;
    card.labels.push_back(label(9, "card", 150, 110));
    return nestWithCard(std::move(scene), std::move(card), kCard);
  }

  void labelsUnderTheCardGoWithTheirTile() {
    SceneShapes scene;
    scene.labels.push_back(label(1, "under", 150, 150));
    scene.labels.push_back(label(1, "measure", 150, 240)); // its tile's name is under the card
    scene.labels.push_back(label(2, "beside", 150, 240));
    const auto out = over(scene);
    TEST_CHECK(!shows(out, "under") && !shows(out, "measure") && shows(out, "beside"));
  }

  void aLineThatGrazesTheEdgeKeepsItsText() {
    SceneShapes scene;
    // Glyphs reach 3.6 px either side of the middle: 1 px over the bottom edge, then 2 px.
    scene.labels.push_back(label(1, "grazing", 150, 202.6F));
    scene.labels.push_back(label(2, "reaching", 150, 201.6F));
    // Along the line the whole room counts.
    scene.labels.push_back(label(3, "long", 80, 150, 50));
    scene.labels.push_back(label(4, "short", 80, 150, 30));
    const auto out = over(scene);
    TEST_CHECK(shows(out, "grazing") && !shows(out, "reaching"));
    TEST_CHECK(!shows(out, "long") && shows(out, "short"));
  }

  void turnedLabelsTurnTheirBox() {
    SceneShapes scene;
    // Standing upright left of the card: lying flat its room would reach into it.
    scene.labels.push_back(label(1, "upright", 90, 150, 80, 90));
    scene.labels.push_back(label(2, "flat", 90, 150, 80));
    // Standing upright above the card, reaching down into it.
    scene.labels.push_back(label(3, "down", 150, 80, 80, -90));
    const auto out = over(scene);
    TEST_CHECK(shows(out, "upright") && !shows(out, "flat") && !shows(out, "down"));
  }

  void anIconCountsLikeItsText() {
    SceneShapes scene;
    auto named = label(1, "named", 150, 212);
    named.icon = {.category = FileCategory::Folder, .size = 14, .left = -20, .top = -14, .color = {}};
    scene.labels.push_back(named);
    auto plain = label(2, "plain", 150, 212);
    scene.labels.push_back(plain);
    const auto out = over(scene);
    TEST_CHECK(!shows(out, "named") && shows(out, "plain"));
  }

  void theCardLiesOnTopAndKeepsItsLabels() {
    SceneShapes scene;
    for (std::size_t i = 0; i < kMaxSceneLabels; ++i) {
      scene.labels.push_back(label(static_cast<NodeId>(i), "far", 500, 500));
    }
    scene.shapes.push_back(rectShape({.node = 1, .role = 0}, 0, 0, 10, 10));
    SceneShapes card;
    card.shapes.push_back(rectShape({.node = 2, .role = 0}, 0, 0, 10, 10));
    card.labels.push_back(label(9, "card", 150, 110));
    const auto out = nestWithCard(std::move(scene), std::move(card), kCard);
    TEST_CHECK(out.shapes.size() == 2 && out.shapes[0].id.node == 1 && out.shapes[1].id.node == 2);
    TEST_CHECK(out.labels.size() == kMaxSceneLabels && out.labels.front().text == "card");
  }
} // namespace

int main() {
  labelsUnderTheCardGoWithTheirTile();
  aLineThatGrazesTheEdgeKeepsItsText();
  turnedLabelsTurnTheirBox();
  anIconCountsLikeItsText();
  theCardLiesOnTopAndKeepsItsLabels();
  std::println("workbench nest card checks passed");
  return 0;
}
