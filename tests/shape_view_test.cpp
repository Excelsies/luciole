#include "core/input/key_modifiers.h"
#include "metrics_renderer.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/text_node.h"
#include "test_check.h"
#include "ui/controls/shape_batch_node.h"
#include "ui/controls/shape_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using test::MetricsRenderer;

  constexpr ShapeId kA{1, 0};
  constexpr ShapeId kB{2, 0};

  // Delegates to MetricsRenderer with another render scale.
  class ScaledMetrics final : public Renderer {
  public:
    explicit ScaledMetrics(float scale) : m_scale(scale) {}
    TextMetrics measureText(
        std::string_view text, float size, const TextFont& font, float width, int lines, TextAlign align,
        TextEllipsize ellipsize, bool markup
    ) override {
      return m_metrics.measureText(text, size, font, width, lines, align, ellipsize, markup);
    }
    TextMetrics measureFont(float size, const TextFont& font) override { return m_metrics.measureFont(size, font); }
    TextMetrics measureGlyph(char32_t glyph, float size) override { return m_metrics.measureGlyph(glyph, size); }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextureManager& textureManager() override { return m_metrics.textureManager(); }
    float renderScale() const noexcept override { return m_scale; }

  private:
    MetricsRenderer m_metrics;
    float m_scale = 1;
  };

  Shape box(ShapeId id, double x, double y, double width, double height) {
    auto shape = rectShape(id, x, y, width, height);
    shape.fill = {200, 100, 50, 255};
    return shape;
  }

  ShapeLabel label(NodeId node, std::string text, float x, float y) {
    return {
        .node = node,
        .text = std::move(text),
        .x = x,
        .y = y,
        .width = 60,
        .height = 16,
        .align = LabelAlign::Start,
        .fontSize = 12,
        .rotation = 0,
        .color = {20, 20, 20, 255},
    };
  }

  SceneShapes twoBoxes(double offset = 0) {
    SceneShapes scene;
    scene.shapes = {box(kA, 10 + offset, 10, 60, 40), box(kB, 100 + offset, 10, 60, 40)};
    scene.labels = {label(1, "first", 10, 20), label(2, "second", 100, 20)};
    return scene;
  }

  struct Harness {
    MetricsRenderer renderer;
    AnimationManager animations;
    ::Node root;
    ShapeView* view = nullptr;
    InputDispatcher dispatcher;
    std::uint32_t modifiers = 0;

    Harness() {
      root.setSize(400, 200);
      root.setAnimationManager(&animations);
      view = static_cast<ShapeView*>(root.addChild(std::make_unique<ShapeView>([this] { return modifiers; })));
      view->arrange(renderer, {.x = 0, .y = 0, .width = 300, .height = 120});
      dispatcher.setSceneRoot(&root);
    }

    void click(float x, float y, std::uint32_t button) {
      dispatcher.pointerMotion(x, y, 1);
      dispatcher.pointerButton(x, y, button, true, 2, 0, false);
      dispatcher.pointerButton(x, y, button, false, 3, 0, false);
    }

    // Ticks until the transitions end, at most two seconds.
    template <typename Each> void runAnimations(Each each) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (animations.hasActive()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        animations.tick(2);
        each();
      }
    }
  };

  std::size_t labelNodes(const ShapeView& view) { return view.labelLayer().children().size(); }

  std::size_t visibleLabels(const ShapeView& view) {
    std::size_t count = 0;
    for (const auto& child : view.labelLayer().children()) {
      count += child->visible() && child->opacity() > 0 ? 1 : 0;
    }
    return count;
  }

  const TextNode* labelWithText(const ShapeView& view, const std::string& text) {
    for (const auto& child : view.labelLayer().children()) {
      const auto* node = dynamic_cast<const TextNode*>(child.get());
      if (node != nullptr && node->visible() && node->text() == text) {
        return node;
      }
    }
    return nullptr;
  }

  void sameShapes(std::span<const Shape> actual, std::span<const Shape> expected) {
    TEST_CHECK(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
      TEST_CHECK(actual[i] == expected[i]);
    }
  }

  void hoverEntersAndLeaves() {
    Harness h;
    h.view->setScene(twoBoxes());
    std::vector<std::optional<ShapeId>> seen;
    h.view->onHoverChanged = [&](std::optional<ShapeId> id) { seen.push_back(id); };
    h.dispatcher.pointerEnter(20, 20, 1);
    h.dispatcher.pointerMotion(30, 25, 1);
    h.dispatcher.pointerMotion(120, 30, 1);
    h.dispatcher.pointerMotion(85, 30, 1);
    h.dispatcher.pointerMotion(110, 20, 1);
    h.dispatcher.pointerLeave();
    TEST_CHECK(seen.size() == 5);
    TEST_CHECK(seen[0] == kA);
    TEST_CHECK(seen[1] == kB);
    TEST_CHECK(!seen[2].has_value());
    TEST_CHECK(seen[3] == kB);
    TEST_CHECK(!seen[4].has_value());
    TEST_CHECK(!h.view->hovered().has_value());
  }

  void clicksActivateTheRightId() {
    Harness h;
    h.view->setScene(twoBoxes());
    struct Activation {
      ShapeId id;
      std::uint32_t button = 0;
      std::uint32_t modifiers = 0;
    };
    std::vector<Activation> activated;
    std::vector<ShapeId> pressed;
    std::vector<std::optional<ShapeId>> context;
    h.view->onActivate = [&](ShapeId id, std::uint32_t button, std::uint32_t modifiers) {
      activated.push_back({id, button, modifiers});
    };
    h.view->onPress = [&](ShapeId id, const InputArea::PointerData&) { pressed.push_back(id); };
    h.view->onContextRequest = [&](std::optional<ShapeId> id, float, float) { context.push_back(id); };
    h.dispatcher.pointerEnter(5, 5, 1);
    h.modifiers = KeyMod::Ctrl;
    h.click(120, 30, BTN_LEFT);
    TEST_CHECK(activated.size() == 1);
    TEST_CHECK(activated[0].id == kB);
    TEST_CHECK(activated[0].button == BTN_LEFT);
    TEST_CHECK(activated[0].modifiers == KeyMod::Ctrl);
    TEST_CHECK(pressed.size() == 1 && pressed[0] == kB);
    h.modifiers = 0;
    h.click(20, 20, BTN_MIDDLE);
    TEST_CHECK(activated.size() == 2);
    TEST_CHECK(activated[1].id == kA);
    TEST_CHECK(activated[1].button == BTN_MIDDLE);
    // Released over another shape or the background: no activation.
    h.dispatcher.pointerMotion(20, 20, 1);
    h.dispatcher.pointerButton(20, 20, BTN_LEFT, true, 4, 0, false);
    h.dispatcher.pointerMotion(120, 20, 1);
    h.dispatcher.pointerButton(120, 20, BTN_LEFT, false, 5, 0, false);
    h.click(90, 100, BTN_LEFT);
    TEST_CHECK(activated.size() == 2);
    h.click(30, 30, BTN_RIGHT);
    h.click(250, 100, BTN_RIGHT);
    TEST_CHECK(context.size() == 2);
    TEST_CHECK(context[0] == kA);
    TEST_CHECK(!context[1].has_value());
    TEST_CHECK(activated.size() == 2);
  }

  void wheelTooltipFocusAndMarks() {
    Harness h;
    h.view->setScene(twoBoxes());
    float delta = 0;
    float wheelX = 0;
    h.view->onWheel = [&](const ShapeWheel& wheel) {
      delta = wheel.delta;
      wheelX = wheel.x;
      return wheel.vertical;
    };
    h.dispatcher.pointerEnter(130, 30, 1);
    TEST_CHECK(h.dispatcher.pointerAxis(130, 30, 0, 0, 10.0, 1, 120, 0));
    TEST_CHECK(delta > 0);
    TEST_NEAR(wheelX, 130.0F, 0.01F);

    h.view->setTooltipProvider([](ShapeId id) -> TooltipContent { return std::to_string(id.node); });
    const auto tip = h.view->inputArea().tooltipContent();
    TEST_CHECK(std::holds_alternative<std::string>(tip) && std::get<std::string>(tip) == "2");
    h.dispatcher.pointerMotion(250, 100, 1);
    TEST_CHECK(std::holds_alternative<std::monostate>(h.view->inputArea().tooltipContent()));

    std::vector<std::uint32_t> keys;
    h.view->onKey = [&](const InputArea::KeyData& key) { keys.push_back(key.sym); };
    h.dispatcher.setFocus(&h.view->inputArea());
    h.dispatcher.keyEvent(0xff51, 0, 0, true);
    TEST_CHECK(keys.size() == 1 && keys[0] == 0xff51);

    int marksChanged = 0;
    h.view->onMarksChanged = [&] { ++marksChanged; };
    h.view->setHighlighted(7);
    h.view->setHighlighted(7);
    h.view->setCursor(9);
    TEST_CHECK(marksChanged == 2);
    TEST_CHECK(h.view->marks().linked == 7);
    TEST_CHECK(h.view->marks().cursor == 9);
    h.dispatcher.pointerMotion(20, 20, 1);
    TEST_CHECK(marksChanged == 3);
    TEST_CHECK(h.view->marks().hovered == kA);
  }

  void tweenReachesTargetWithoutLayout() {
    Harness h;
    h.view->setScene(twoBoxes());
    h.view->layout(h.renderer);
    const auto* first = labelWithText(*h.view, "first");
    TEST_CHECK(first != nullptr);
    TEST_NEAR(first->x(), 10.0F, 0.001F);
    // Centered in its 16 px box: the baseline sits half the cap height below the middle.
    TEST_NEAR(first->y(), 34.0F, 0.001F);

    int layouts = 0;
    h.root.setInvalidationCallback([&](NodeInvalidation kind) { layouts += kind == NodeInvalidation::Layout ? 1 : 0; });
    auto target = twoBoxes(40);
    target.labels[0].x = 50;
    target.labels[1].x = 140;
    h.view->setScene(target, 40);
    TEST_CHECK(h.view->transitioning());
    h.view->layout(h.renderer);
    h.root.clearDirty();
    layouts = 0;
    bool between = false;
    h.runAnimations([&] {
      const auto shown = h.view->shapes();
      between = between || (shown.size() == 2 && shown[0].rect.x > 10 && shown[0].rect.x < 50);
      h.root.clearDirty();
    });
    TEST_CHECK(layouts == 0);
    TEST_CHECK(between);
    TEST_CHECK(!h.view->transitioning());
    sameShapes(h.view->shapes(), target.shapes);
    TEST_CHECK(h.view->batch().mesh().drawn == 2);
    first = labelWithText(*h.view, "first");
    TEST_CHECK(first != nullptr);
    TEST_NEAR(first->x(), 50.0F, 0.001F);
    TEST_NEAR(first->y(), 34.0F, 0.001F);

    // Labels that leave fade out and are hidden once the motion has ended.
    auto other = target;
    other.labels = {label(3, "third", 30, 60)};
    h.view->setScene(other, 40);
    h.view->layout(h.renderer);
    h.root.clearDirty();
    layouts = 0;
    h.runAnimations([&] {
      if (h.animations.hasActive()) {
        TEST_CHECK(layouts == 0);
        TEST_CHECK(labelWithText(*h.view, "first") != nullptr);
      }
      h.root.clearDirty();
    });
    TEST_CHECK(labelWithText(*h.view, "first") == nullptr);
    TEST_CHECK(labelWithText(*h.view, "second") == nullptr);
    TEST_CHECK(labelWithText(*h.view, "third") != nullptr);
    TEST_CHECK(visibleLabels(*h.view) == 1);
  }

  void destroyedMidTween() {
    Harness h;
    h.view->setScene(twoBoxes());
    h.view->onHoverChanged = [](std::optional<ShapeId>) { TEST_CHECK(false); };
    h.view->setScene(twoBoxes(40), 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    h.animations.tick(5);
    TEST_CHECK(h.animations.hasActive());
    h.root.removeChild(h.view).reset();
    h.view = nullptr;
    TEST_CHECK(!h.animations.hasActive());
    h.animations.tick(5);
  }

  // An animation ahead of the view's ends in the same tick and starts a new transition from its completion.
  void retargetFromAnotherCompletion() {
    Harness h;
    h.view->setScene(twoBoxes());
    const auto next = twoBoxes(90);
    bool restarted = false;
    h.animations.animate(
        0.0F, 1.0F, 20, Easing::Linear, [](float) {},
        [&] {
          h.view->setScene(next, 200);
          restarted = true;
        }
    );
    h.view->setScene(twoBoxes(40), 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    h.animations.tick(40);
    TEST_CHECK(restarted);
    TEST_CHECK(h.view->transitioning());
    TEST_CHECK(h.view->shapes().size() == 2);
    h.runAnimations([&] {
      TEST_CHECK(h.view->shapes().size() == 2);
      TEST_CHECK(h.view->transitioning() == h.animations.hasActive());
    });
    TEST_CHECK(!h.view->transitioning());
    sameShapes(h.view->shapes(), next.shapes);
  }

  // A new scene mid-transition: labels continue from where they are drawn, like the shapes.
  void retargetMidTransition() {
    Harness h;
    h.view->setScene(twoBoxes());
    h.view->layout(h.renderer);
    auto middle = twoBoxes(40);
    middle.labels = {label(1, "first", 50, 20), label(3, "third", 30, 60)};
    h.view->setScene(middle, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    h.animations.tick(400);
    TEST_CHECK(h.view->transitioning());
    const auto* first = labelWithText(*h.view, "first");
    const auto* second = labelWithText(*h.view, "second");
    const auto* third = labelWithText(*h.view, "third");
    TEST_CHECK(first != nullptr && second != nullptr && third != nullptr);
    if (first == nullptr || second == nullptr || third == nullptr) {
      return;
    }
    const float firstX = first->x();
    const float secondOpacity = second->opacity();
    const float thirdOpacity = third->opacity();
    const double shapeX = h.view->shapes()[0].rect.x;
    TEST_CHECK(firstX > 10 && firstX < 50);
    TEST_CHECK(secondOpacity > 0 && secondOpacity < 1);
    TEST_CHECK(thirdOpacity > 0 && thirdOpacity < 1);

    auto last = twoBoxes(90);
    last.labels = {label(1, "first", 100, 20), label(3, "third", 80, 60)};
    h.view->setScene(last, 200);
    h.view->layout(h.renderer);
    TEST_NEAR(h.view->shapes()[0].rect.x, shapeX, 0.001);
    TEST_NEAR(first->x(), firstX, 0.001F);
    TEST_NEAR(third->x(), 30.0F, 0.001F);
    TEST_NEAR(third->opacity(), thirdOpacity, 0.001F);
    TEST_CHECK(second->visible());
    TEST_NEAR(second->opacity(), secondOpacity, 0.001F);

    h.runAnimations([] {});
    TEST_NEAR(first->x(), 100.0F, 0.001F);
    TEST_NEAR(third->x(), 80.0F, 0.001F);
    TEST_NEAR(third->opacity(), 1.0F, 0.001F);
    TEST_CHECK(!second->visible());
    TEST_CHECK(visibleLabels(*h.view) == 2);
  }

  SceneShapes manyLabels(const std::string& prefix, std::size_t count) {
    SceneShapes scene = twoBoxes();
    scene.labels.clear();
    for (std::size_t i = 0; i < count; ++i) {
      scene.labels.push_back(label(static_cast<NodeId>(i), prefix + std::to_string(i), 1, static_cast<float>(i)));
    }
    return scene;
  }

  void labelPoolStaysUnderCap() {
    Harness h;
    h.view->setScene(manyLabels("start-", 300));
    TEST_CHECK(labelNodes(*h.view) == ShapeView::kMaxLabels);
    TEST_CHECK(visibleLabels(*h.view) == ShapeView::kMaxLabels);
    for (int round = 0; round < 3; ++round) {
      const std::string prefix = "round" + std::to_string(round) + "-";
      h.view->setScene(manyLabels(prefix, ShapeView::kMaxLabels), 30);
      TEST_CHECK(labelNodes(*h.view) <= ShapeView::kMaxLabels);
      h.runAnimations([&] { TEST_CHECK(labelNodes(*h.view) <= ShapeView::kMaxLabels); });
      TEST_CHECK(visibleLabels(*h.view) == ShapeView::kMaxLabels);
      TEST_CHECK(labelWithText(*h.view, prefix + "0") != nullptr);
      TEST_CHECK(labelWithText(*h.view, prefix + "159") != nullptr);
    }
    h.view->setScene(manyLabels("half-", 80), 30);
    h.runAnimations([] {});
    TEST_CHECK(visibleLabels(*h.view) == 80);
    TEST_CHECK(labelNodes(*h.view) <= ShapeView::kMaxLabels);
  }

  void reducedMotionAndScale() {
    MotionService::instance().setEnabled(false);
    {
      Harness h;
      h.view->setScene(twoBoxes());
      const auto target = twoBoxes(40);
      h.view->setScene(target, 200);
      TEST_CHECK(!h.view->transitioning());
      TEST_CHECK(!h.animations.hasActive());
      sameShapes(h.view->shapes(), target.shapes);
    }
    MotionService::instance().setEnabled(true);

    Harness h;
    h.view->setScene(twoBoxes());
    TEST_NEAR(h.view->batch().mesh().vertices[0].style[3], 1.0F, 0.0001F);
    float resized = 0;
    h.view->onResize = [&](float width, float) { resized = width; };
    ScaledMetrics scaled(2);
    h.view->arrange(scaled, {.x = 0, .y = 0, .width = 320, .height = 140});
    TEST_NEAR(h.view->batch().meshScale(), 2.0F, 0.0001F);
    TEST_NEAR(h.view->batch().mesh().vertices[0].style[3], 0.5F, 0.0001F);
    TEST_NEAR(resized, 320.0F, 0.001F);
    TEST_NEAR(h.view->inputArea().width(), 320.0F, 0.001F);
    h.dispatcher.pointerEnter(310, 130, 1);
    TEST_CHECK(h.dispatcher.hoveredArea() == &h.view->inputArea());

    ShapeView detached([] { return 0U; });
    detached.setScene(twoBoxes(), 200);
    TEST_CHECK(!detached.transitioning());
  }
} // namespace

int main() {
  hoverEntersAndLeaves();
  clicksActivateTheRightId();
  wheelTooltipFocusAndMarks();
  tweenReachesTargetWithoutLayout();
  destroyedMidTween();
  retargetFromAnotherCompletion();
  retargetMidTransition();
  labelPoolStaysUnderCap();
  reducedMotionAndScale();
  std::println("shape view checks passed");
}
