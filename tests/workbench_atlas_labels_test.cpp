#include "test_check.h"
#include "workbench/atlas_labels.h"
#include "workbench_atlas_fixture.h"

#include <print>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::atlas;

  SceneItem bubbleAt(double x, double y, double r, BubbleRole role = BubbleRole::Bubble, double fade = 1) {
    return {.node = kFirstFile, .role = role, .x = x, .y = y, .r = r, .fade = fade, .depth = 1, .clickable = true};
  }

  void testChordAndLabelWidth() {
    TEST_NEAR(chord(5, 3), 8.0, 1e-12);
    TEST_NEAR(chord(5, 6), 0.0, 0.0);
    TEST_NEAR(labelWidth(5, 3), 1.52, 1e-12);
    TEST_NEAR(labelWidth(100, 0), 182.0, 1e-12);
  }

  void testLabelPlans() {
    const ViewSize view{600, 600};
    auto more = bubbleAt(100, 100, 27, BubbleRole::More);
    more.node = kNoNode;
    auto plan = labelPlan(more, view);
    TEST_CHECK(plan.kind == LabelKind::More && plan.moreLine);
    TEST_NEAR(plan.alpha, 0.5, 1e-12);
    TEST_CHECK(plan.x == 100 && plan.y == 100);
    more.r = 20;
    TEST_CHECK(labelPlan(more, view).kind == LabelKind::None);

    TEST_CHECK(labelPlan(bubbleAt(100, 100, 80, BubbleRole::Ancestor), view).kind == LabelKind::None);
    auto quiet = bubbleAt(100, 100, 80);
    quiet.clickable = false;
    TEST_CHECK(labelPlan(quiet, view).kind == LabelKind::None);
    TEST_CHECK(labelPlan(bubbleAt(100, 100, 25), view).kind == LabelKind::None);

    auto self = bubbleAt(300, 300, 286, BubbleRole::Self);
    self.kids = true;
    plan = labelPlan(self, view);
    TEST_CHECK(plan.kind == LabelKind::Title);
    TEST_NEAR(plan.alpha, 1.0, 0.0);
    TEST_NEAR(plan.y, 38.31, 1e-9);
    TEST_NEAR(plan.room, 210.77559576350362, 1e-9);
    TEST_NEAR(plan.fontSize, 14.0, 0.0);
    self.kids = false;
    self.empty = true;
    plan = labelPlan(self, view);
    TEST_CHECK(plan.kind == LabelKind::EmptyNote);
    TEST_NEAR(plan.alpha, 0.9, 1e-12);
    TEST_NEAR(plan.room, 485.9027063106358, 1e-9);

    auto folder = bubbleAt(300, 300, 150, BubbleRole::Sibling, 0.6);
    folder.kids = true;
    plan = labelPlan(folder, view);
    TEST_CHECK(plan.kind == LabelKind::Title);
    TEST_NEAR(plan.alpha, 0.468, 1e-12);
    TEST_NEAR(plan.y, 162.75, 1e-9);
    TEST_NEAR(plan.room, 101.03615162421515, 1e-9);
    TEST_NEAR(plan.fontSize, 13.0, 0.0);

    plan = labelPlan(bubbleAt(100, 100, 40), view);
    TEST_CHECK(plan.kind == LabelKind::Center && !plan.icon && !plan.backdrop);
    TEST_NEAR(plan.alpha, 1.0, 0.0);
    TEST_NEAR(plan.fontSize, 12.0, 0.0);
    TEST_NEAR(plan.metricAlpha, 0.0, 0.0);
    TEST_CHECK(plan.x == 100 && plan.y == 100 && plan.r == 40);

    auto sibling = bubbleAt(100, 100, 60, BubbleRole::Sibling, 0.6);
    sibling.kids = true;
    plan = labelPlan(sibling, view);
    TEST_CHECK(plan.kind == LabelKind::Center && plan.icon && plan.backdrop);
    TEST_NEAR(plan.alpha, 0.468, 1e-12);
    TEST_NEAR(plan.fontSize, 13.0, 0.0);
    TEST_NEAR(plan.metricAlpha, 1.0, 0.0);
    TEST_NEAR(labelPlan(bubbleAt(100, 100, 48), view).metricAlpha, 0.5, 1e-12);
    // The metric line shows only once its strength passes 0.05.
    TEST_NEAR(labelPlan(bubbleAt(100, 100, 44.3), view).metricAlpha, 0.0, 0.0);
    TEST_NEAR(labelPlan(bubbleAt(100, 100, 44.4), view).metricAlpha, 0.0, 0.0);
    TEST_NEAR(labelPlan(bubbleAt(100, 100, 44.8), view).metricAlpha, 0.1, 1e-12);
    TEST_NEAR(labelPlan(bubbleAt(300, 300, 150), view).fontSize, 14.0, 0.0);

    plan = labelPlan(bubbleAt(-20, 300, 50), view);
    TEST_CHECK(plan.kind == LabelKind::Center);
    TEST_CHECK(plan.x == 15 && plan.y == 300 && plan.r == 15);
    TEST_NEAR(plan.alpha, 1.0, 0.0);
    TEST_NEAR(plan.fontSize, 12.0, 0.0);
    TEST_CHECK(labelPlan(bubbleAt(-30, -30, 50), view).kind == LabelKind::None);
    // A corner cut: the visible part's center must stay well inside the circle.
    TEST_CHECK(labelPlan(bubbleAt(-10, -10, 50), view).kind == LabelKind::None);
    plan = labelPlan(bubbleAt(10, 10, 50), view);
    TEST_CHECK(plan.kind == LabelKind::Center && plan.x == 30 && plan.y == 30 && plan.r == 30);
    // A folder with children under 110 px gets a centered label over them, not a title.
    auto smallFolder = bubbleAt(300, 300, 105);
    smallFolder.kids = true;
    plan = labelPlan(smallFolder, view);
    TEST_CHECK(plan.kind == LabelKind::Center && plan.backdrop);
  }
} // namespace

int main() {
  testChordAndLabelWidth();
  testLabelPlans();
  std::println("workbench atlas labels checks passed");
  return 0;
}
