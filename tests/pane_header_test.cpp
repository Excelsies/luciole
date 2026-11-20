#include "metrics_renderer.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_header.h"
#include "ui/controls/segmented.h"
#include "ui/style.h"

#include <memory>
#include <print>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
  std::string tooltipOf(const Button* button) {
    TEST_CHECK(button != nullptr);
    const auto content = button->inputArea()->tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  // The header in a scene of its own, laid out at a width.
  struct Rig {
    test::MetricsRenderer renderer;
    Node root;
    PaneHeader* header = nullptr;
    InputDispatcher dispatcher;
    int collapses = 0;
    int expands = 0;
    std::uint32_t serial = 0;

    explicit Rig(PaneHeaderOptions options) {
      root.setSize(400, 100);
      header = static_cast<PaneHeader*>(root.addChild(std::make_unique<PaneHeader>(std::move(options))));
      header->setOnCollapse([this] { ++collapses; });
      header->setOnExpand([this] { ++expands; });
      dispatcher.setSceneRoot(&root);
      layout(400);
    }

    void layout(float width) { header->arrange(renderer, {0, 0, width, PaneHeader::kHeight}); }

    void click(const Button* button) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(button, x, y);
      x += button->width() / 2;
      y += button->height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 0, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 0, false);
    }

    std::vector<InputArea*> tabOrder() {
      std::vector<InputArea*> order;
      dispatcher.setFocus(nullptr);
      for (int i = 0; i < 8 && dispatcher.cycleTabFocus(false); ++i) {
        if (!order.empty() && dispatcher.focusedArea() == order.front()) {
          break;
        }
        order.push_back(dispatcher.focusedArea());
      }
      return order;
    }
  };

  bool inside(const Node& node, const Node& box) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return x >= box.x()
        && y >= box.y()
        && x + node.width() <= box.x() + box.width() + 0.01F
        && y + node.height() <= box.y() + box.height() + 0.01F;
  }

  void titleAndButtons() {
    Rig rig({.name = "Instrument", .expandable = true});
    rig.header->setTitle("Instrument: Nest");
    rig.layout(400);
    TEST_CHECK(rig.header->titleLabel()->text() == "Instrument: Nest");
    TEST_CHECK(rig.header->height() == PaneHeader::kHeight);
    TEST_CHECK(tooltipOf(rig.header->collapseButton()) == "Collapse Instrument");
    TEST_CHECK(tooltipOf(rig.header->expandButton()) == "Expand Instrument to the whole view");
    TEST_CHECK(inside(*rig.header->collapseButton(), *rig.header));
    TEST_CHECK(inside(*rig.header->expandButton(), *rig.header));
    // the buttons sit at the end, collapse last
    TEST_CHECK(rig.header->expandButton()->x() < rig.header->collapseButton()->x());
    TEST_CHECK(rig.header->titleLabel()->x() < rig.header->expandButton()->x());
    rig.click(rig.header->collapseButton());
    TEST_CHECK(rig.collapses == 1 && rig.expands == 0);
    rig.click(rig.header->expandButton());
    TEST_CHECK(rig.collapses == 1 && rig.expands == 1);
    const auto order = rig.tabOrder();
    TEST_CHECK(order.size() == 2);
    TEST_CHECK(order[0] == rig.header->expandButton()->inputArea());
    TEST_CHECK(order[1] == rig.header->collapseButton()->inputArea());
  }

  void collapsedAndExpandedStates() {
    Rig rig({.name = "Instrument", .expandable = true});
    rig.header->setCollapsed(true);
    rig.layout(400);
    TEST_CHECK(tooltipOf(rig.header->collapseButton()) == "Restore Instrument");
    TEST_CHECK(!rig.header->expandButton()->visible()); // a collapsed pane cannot expand
    rig.header->setCollapsed(false);
    rig.header->setExpanded(true);
    rig.layout(400);
    TEST_CHECK(rig.header->expandButton()->visible());
    TEST_CHECK(tooltipOf(rig.header->expandButton()) == "Return Instrument to its pane");
    TEST_CHECK(!rig.header->collapseButton()->visible()); // expanded, only Restore is offered
    TEST_CHECK(rig.tabOrder().size() == 1);
    rig.header->setExpanded(false);
    rig.layout(400);
    TEST_CHECK(rig.header->collapseButton()->visible());
    TEST_CHECK(tooltipOf(rig.header->expandButton()) == "Expand Instrument to the whole view");
  }

  void plainPaneHasNoExpand() {
    Rig rig({.name = "Jump ahead", .collapsesDown = true});
    TEST_CHECK(rig.header->expandButton() == nullptr);
    rig.header->setTitle("Jump ahead");
    rig.header->setCollapsed(true);
    TEST_CHECK(tooltipOf(rig.header->collapseButton()) == "Restore Jump ahead");
    TEST_CHECK(rig.tabOrder().size() == 1);
  }

  void closedByForce() {
    Rig rig({.name = "Jump ahead", .collapsesDown = true});
    rig.header->setTitle("Jump ahead");
    rig.header->setCollapsed(true);
    rig.header->setClosedReason("too short");
    rig.layout(400);
    TEST_CHECK(rig.header->titleLabel()->text() == "Jump ahead (closed: too short)");
    TEST_CHECK(!rig.header->collapseButton()->enabled());
    TEST_CHECK(tooltipOf(rig.header->collapseButton()) == "Jump ahead: closed, the window is too short");
    rig.click(rig.header->collapseButton());
    TEST_CHECK(rig.collapses == 0);
    TEST_CHECK(rig.tabOrder().empty());
    rig.header->setTitle("Jump ahead now");
    TEST_CHECK(rig.header->titleLabel()->text() == "Jump ahead now (closed: too short)");
    rig.header->setClosedReason("");
    TEST_CHECK(rig.header->titleLabel()->text() == "Jump ahead now");
    TEST_CHECK(rig.header->collapseButton()->enabled());
    TEST_CHECK(tooltipOf(rig.header->collapseButton()) == "Restore Jump ahead");
  }

  void ownerControls() {
    Rig rig({.name = "Ribbon"});
    auto segmented = std::make_unique<Segmented>();
    segmented->addOption("Flame");
    segmented->addOption("Flow");
    auto* controls = rig.header->setControls(std::move(segmented));
    rig.layout(400);
    TEST_CHECK(controls != nullptr && controls->parent() == rig.header);
    TEST_CHECK(controls->x() + controls->width() <= rig.header->collapseButton()->x());
    TEST_CHECK(rig.header->titleLabel()->x() + rig.header->titleLabel()->width() <= controls->x() + 0.01F);
    int resets = 0;
    auto* glyph = rig.header->setControls(paneHeaderButton("refresh", "Reset layout", [&] { ++resets; }));
    rig.layout(400);
    TEST_CHECK(controls != glyph);
    const auto* button = dynamic_cast<const Button*>(glyph);
    TEST_CHECK(tooltipOf(button) == "Reset layout");
    rig.click(button);
    TEST_CHECK(resets == 1);
    TEST_CHECK(rig.tabOrder().size() == 2);
    TEST_CHECK(rig.header->setControls(nullptr) == nullptr);
    TEST_CHECK(rig.tabOrder().size() == 1);
  }

  void narrowTitleStaysInside() {
    Rig rig({.name = "Instrument", .expandable = true});
    rig.header->setTitle("Instrument: a very long name that cannot fit in the header at all");
    rig.layout(120);
    TEST_CHECK(rig.header->width() == 120);
    TEST_CHECK(rig.header->titleLabel()->x() + rig.header->titleLabel()->width() <= rig.header->expandButton()->x());
    TEST_CHECK(inside(*rig.header->collapseButton(), *rig.header));
  }

  void mirrorsInRightToLeft() {
    Style::setRtl(true);
    Rig rig({.name = "Instrument", .expandable = true});
    rig.header->setTitle("Instrument: Nest");
    rig.layout(400);
    TEST_CHECK(rig.header->collapseButton()->x() < rig.header->expandButton()->x());
    TEST_CHECK(rig.header->expandButton()->x() < rig.header->titleLabel()->x());
    Style::setRtl(false);
  }
} // namespace

void closedTitleFormat() {
  TEST_CHECK(closedPaneTitle("Ribbon: Flame graph", "too narrow") == "Ribbon: Flame graph (closed: too narrow)");
  TEST_CHECK(closedPaneTitle("Jump ahead", "") == "Jump ahead");
  TEST_CHECK(closedPaneTitle("", "too short") == " (closed: too short)");
}

int main() {
  closedTitleFormat();
  titleAndButtons();
  collapsedAndExpandedStates();
  plainPaneHasNoExpand();
  closedByForce();
  ownerControls();
  narrowTitleStaysInside();
  mirrorsInRightToLeft();
  std::println("pane header checks passed");
}
