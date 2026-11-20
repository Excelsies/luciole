#include "metrics_renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/ancestor_strips.h"
#include "workbench/column_plan.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <print>
#include <string>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using luciole::AncestorEntry;
  using luciole::AncestorStrips;

  struct SceneRect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
  };

  SceneRect sceneBox(const Node* node) {
    TEST_CHECK(node != nullptr);
    SceneRect box{.width = node->width(), .height = node->height()};
    Node::absolutePosition(node, box.x, box.y);
    return box;
  }

  bool shown(const Node* node) {
    for (; node != nullptr; node = node->parent()) {
      if (!node->visible()) {
        return false;
      }
    }
    return true;
  }

  std::string tooltipOf(const InputArea& area) {
    const auto content = area.tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  // The first Box child of a strip: its background.
  const Box* backgroundOf(const InputArea& area) {
    for (const auto& child : area.children()) {
      if (const auto* box = dynamic_cast<const Box*>(child.get()); box != nullptr && shown(box)) {
        return box;
      }
    }
    return nullptr;
  }

  // Level k (1 is the nearest) is named "level-k", lives at "/l<depth>/.../l<k>" and has id 100 + k.
  std::vector<AncestorEntry> ancestorsOf(std::size_t count) {
    std::vector<AncestorEntry> out;
    for (std::size_t level = 1; level <= count; ++level) {
      std::string path;
      for (std::size_t k = count; k >= level; --k) {
        path += std::format("/l{}", k);
      }
      out.push_back({.name = std::format("level-{}", level), .path = path, .id = 100 + level});
    }
    return out;
  }

  struct Rig {
    test::MetricsRenderer renderer;
    Node root;
    AncestorStrips* strips = nullptr;
    InputDispatcher dispatcher;
    std::vector<std::uint64_t> activated;
    std::vector<std::uint32_t> buttons; // of each activation; 0 for a key
    std::vector<std::optional<std::uint64_t>> hovers;
    std::uint32_t serial = 0;
    float x = 10;
    float width = 0;
    float height = 600;

    // A strips pane `paneWidth` px wide, planned for `count` ancestors.
    Rig(std::size_t count, float paneWidth) : width(paneWidth) {
      root.setSize(4000, 4000);
      strips = static_cast<AncestorStrips*>(root.addChild(std::make_unique<AncestorStrips>()));
      strips->onActivate = [this](std::uint64_t id, std::uint32_t button) {
        activated.push_back(id);
        buttons.push_back(button);
      };
      strips->onHover = [this](std::optional<std::uint64_t> id) { hovers.push_back(id); };
      show(count);
      dispatcher.setSceneRoot(&root);
      layout();
    }

    // Ancestors and a plan for `count` levels at the pane's width.
    void show(std::size_t count) {
      strips->setAncestors(ancestorsOf(count));
      plan(count);
    }
    void plan(std::size_t count) { strips->setPlan(luciole::workbench::ancestorStrips(count, width)); }
    void layout() { strips->arrange(renderer, {x, 0, width, height}); }
    SceneRect strip(std::size_t index) const { return sceneBox(strips->area(index)); }
    void moveTo(float px, float py) {
      dispatcher.pointerMotion(px, py, ++serial);
      layout();
    }
    void clickAt(float px, float py, std::uint32_t button = BTN_LEFT) {
      dispatcher.pointerEnter(px, py, ++serial);
      dispatcher.pointerButton(px, py, button, true, ++serial, 1000, false);
      dispatcher.pointerButton(px, py, button, false, ++serial, 1050, false);
      layout();
    }
    void click(std::size_t index, std::uint32_t button = BTN_LEFT) {
      const auto box = strip(index);
      clickAt(box.x + box.width / 2, box.y + 300, button);
    }
    void key(std::uint32_t sym) {
      dispatcher.keyEvent(sym, 0, 0, true);
      dispatcher.keyEvent(sym, 0, 0, false);
      layout();
    }
  };

  // Seven named bands take 20 * 4 + 26 + 34 + 44 px and a 1 px gap after each: 191 of the pane's 230.
  void bandsThinWithDistanceAgainstTheList() {
    Rig rig(7, 230);
    TEST_CHECK(rig.strips->count() == 7);
    const std::vector<float> widths{20, 20, 20, 20, 26, 34, 44}; // farthest first
    const std::vector<float> lefts{49, 70, 91, 112, 133, 160, 195};
    for (std::size_t i = 0; i < widths.size(); ++i) {
      const auto box = rig.strip(i);
      TEST_CHECK(box.width == widths[i]);
      TEST_CHECK(box.x == lefts[i]);
      TEST_CHECK(box.y == 0 && box.height == 600);
      TEST_CHECK(shown(rig.strips->area(i)));
    }
    TEST_CHECK(rig.strip(6).x + 44 + 1 == 10 + 230); // the nearest sits against the list
    // every band is named; 20 px ones in a smaller font than the wider ones
    for (std::size_t i = 0; i < 4; ++i) {
      TEST_CHECK(rig.strips->label(i) != nullptr && rig.strips->label(i)->fontSize() == 11);
      TEST_CHECK(rig.strips->label(i)->text() == std::format("level-{}", 7 - i));
    }
    TEST_CHECK(rig.strips->label(4)->fontSize() == 13);
    TEST_CHECK(rig.strips->label(6)->text() == "level-1");
    TEST_CHECK(rig.strips->label(6)->fontSize() == 13);
    // the name is written along the strip: turned a quarter, inside the strip, below the icon
    for (std::size_t i = 0; i < 7; ++i) {
      const Label* label = rig.strips->label(i);
      TEST_NEAR(label->rotation(), std::numbers::pi / 2, 1e-6);
      float left = 0;
      float top = 0;
      float right = 0;
      float bottom = 0;
      Node::transformedBounds(label, left, top, right, bottom);
      const auto box = rig.strip(i);
      TEST_CHECK(left >= box.x - 0.01F && right <= box.x + box.width + 0.01F);
      TEST_CHECK(bottom - top > right - left);
      TEST_NEAR((left + right) / 2, box.x + box.width / 2, 0.01);
      TEST_CHECK(top >= 14 - 0.01F && bottom <= 600 - 24 + 0.01F);
    }
    float top = 0;
    float ignore = 0;
    Node::transformedBounds(rig.strips->label(6), ignore, top, ignore, ignore);
    TEST_NEAR(top, 14 + 18 + 10, 0.01); // under the 18 px icon of the widest strip
    Node::transformedBounds(rig.strips->label(4), ignore, top, ignore, ignore);
    TEST_NEAR(top, 14, 0.01); // the 26 px strip has no icon
    // the room beyond the farthest band takes no clicks
    rig.clickAt(30, 300);
    TEST_CHECK(rig.activated.empty());
  }

  // At any pane width and depth, every band shown carries its folder's name or the levels it folds.
  void everyBandIsNamed() {
    for (const float pane : {70.0F, 104.0F, 153.0F, 230.0F, 321.0F}) {
      Rig rig(0, pane);
      for (std::size_t count = 0; count <= 40; ++count) {
        rig.show(count);
        rig.layout();
        for (std::size_t i = 0; i < rig.strips->count(); ++i) {
          TEST_CHECK(rig.strips->label(i) != nullptr);
          TEST_CHECK(rig.strip(i).width >= 20);
        }
      }
    }
  }

  void farLevelsFoldIntoOneBand() {
    {
      Rig rig(5, 149); // five named bands fit exactly: no "+N levels" band
      TEST_CHECK(rig.strips->count() == 5);
      TEST_CHECK(rig.strip(0).width == 20 && rig.strip(0).x == 10);
      TEST_CHECK(tooltipOf(*rig.strips->area(0)) == "/l5");
    }
    Rig rig(10, 153); // the default pane: the nearest four and one 24 px band for levels 5 to 10
    TEST_CHECK(rig.strips->count() == 5);
    TEST_CHECK(rig.strip(0).width == 24 && rig.strip(0).x == 10);
    TEST_CHECK(rig.strip(1).width == 20 && rig.strip(1).x == 35);
    TEST_CHECK(rig.strips->label(0)->text() == "6 levels");
    TEST_CHECK(rig.strips->label(0)->fontSize() == 11);
    TEST_CHECK(tooltipOf(*rig.strips->area(0)) == "/l10/l9/l8/l7/l6/l5"); // where a click on it goes
    rig.show(14);
    rig.layout();
    TEST_CHECK(rig.strips->count() == 5);
    TEST_CHECK(rig.strips->label(0)->text() == "10 levels");
    rig.width = 105; // two named bands and the band of the one level left
    rig.show(3);
    rig.layout();
    TEST_CHECK(rig.strips->count() == 3);
    TEST_CHECK(rig.strips->label(0)->text() == "1 level");
    TEST_CHECK(rig.strips->label(1)->text() == "level-2");
  }

  void clickGoesToTheAncestor() {
    Rig rig(4, 128); // 20, 26, 34, 44
    rig.click(2);    // the 34 px strip: level 2
    TEST_CHECK((rig.activated == std::vector<std::uint64_t>{102}));
    rig.click(3);
    TEST_CHECK(rig.activated.back() == 101);
    TEST_CHECK(tooltipOf(*rig.strips->area(0)) == "/l4");
    TEST_CHECK(tooltipOf(*rig.strips->area(3)) == "/l4/l3/l2/l1");
    // a stack goes to the nearest level it stands for: six named bands, then levels 7 to 12
    rig.width = 209;
    rig.show(12);
    rig.layout();
    TEST_CHECK(rig.strips->count() == 7);
    rig.click(0);
    TEST_CHECK(rig.activated.back() == 107);
    // keys: Enter and Space on a focused strip
    rig.dispatcher.setFocus(rig.strips->area(3));
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.activated.back() == 104);
    rig.dispatcher.setFocus(rig.strips->area(6));
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.activated.back() == 101);
    rig.key(XKB_KEY_Left);
    TEST_CHECK(rig.activated.size() == 5);
    TEST_CHECK(rig.strips->area(6)->focusable() && rig.strips->area(6)->tabStop());
    TEST_CHECK(rig.strips->area(7) == nullptr && rig.strips->label(7) == nullptr);
  }

  // The owner learns what asked: the pointer button of a click, the middle button included, or 0 for a key.
  void activationNamesItsButton() {
    Rig rig(4, 128);
    rig.click(1);
    rig.click(2, BTN_MIDDLE);
    rig.click(0, BTN_RIGHT);
    rig.dispatcher.setFocus(rig.strips->area(3));
    rig.key(XKB_KEY_Return);
    TEST_CHECK((rig.activated == std::vector<std::uint64_t>{103, 102, 101}));
    TEST_CHECK((rig.buttons == std::vector<std::uint32_t>{BTN_LEFT, BTN_MIDDLE, 0}));
    // the nearest shown strip, and whether a strip holds the keyboard focus, shown or not
    TEST_CHECK(rig.strips->nearest() == rig.strips->area(3) && rig.strips->focused());
    rig.show(2);
    rig.layout();
    TEST_CHECK(rig.strips->count() == 2 && rig.strips->nearest() == rig.strips->area(1));
    TEST_CHECK(rig.strips->focused()); // the hidden fourth strip keeps it until someone moves it
    rig.dispatcher.setFocus(nullptr);
    TEST_CHECK(!rig.strips->focused());
    rig.strips->setPlan({});
    TEST_CHECK(rig.strips->count() == 0 && rig.strips->nearest() == nullptr);
  }

  void hoverPointedAndFocusShow() {
    palette.surfaceContainerHighest = rgba(0.30F, 0.30F, 0.30F, 1);
    palette.surfaceContainerLow = rgba(0.10F, 0.10F, 0.10F, 1);
    palette.hover = rgba(0.50F, 0.50F, 0.50F, 1);
    palette.primaryContainer = rgba(0.20F, 0.30F, 0.60F, 1);
    palette.primary = rgba(0.20F, 0.40F, 0.90F, 1);
    palette.outlineVariant = rgba(0.40F, 0.40F, 0.40F, 1);
    Rig rig(10, 209); // six named bands and the band of levels 7 to 10
    const auto& nearest = *rig.strips->area(6);
    const Box* background = backgroundOf(nearest);
    TEST_CHECK(background != nullptr);
    const Color quiet = background->style().fill;
    TEST_CHECK(quiet == palette.surfaceContainerHighest);
    TEST_CHECK(backgroundOf(*rig.strips->area(0))->style().fill == palette.surfaceContainerLow); // the far stack
    // hover reports the folder and lights the strip
    const auto box = rig.strip(6);
    rig.dispatcher.pointerEnter(box.x + 5, 300, ++rig.serial);
    rig.layout();
    TEST_CHECK((rig.hovers == std::vector<std::optional<std::uint64_t>>{101}));
    TEST_CHECK(background->style().fill == palette.hover);
    rig.moveTo(rig.strip(5).x + 5, 300);
    TEST_CHECK(rig.hovers.size() == 3 && !rig.hovers[1].has_value() && rig.hovers[2] == 102U);
    TEST_CHECK(background->style().fill == quiet);
    rig.moveTo(rig.strip(0).x + 5, 300); // the stack points at no single folder
    TEST_CHECK(!rig.hovers.back().has_value());
    rig.moveTo(1000, 300);
    TEST_CHECK(!rig.hovers.back().has_value());
    // another pane points at level 1, then at a level inside the stack
    rig.strips->setPointed(101);
    TEST_CHECK(background->style().fill == palette.primaryContainer);
    rig.strips->setPointed(110);
    TEST_CHECK(background->style().fill == quiet);
    TEST_CHECK(backgroundOf(*rig.strips->area(0))->style().fill == palette.primaryContainer);
    rig.strips->setPointed(999); // not an ancestor: nothing is marked
    TEST_CHECK(backgroundOf(*rig.strips->area(0))->style().fill == palette.surfaceContainerLow);
    rig.strips->setPointed(std::nullopt);
    TEST_CHECK(background->style().fill == quiet);
    // keyboard focus draws a ring
    const float edge = background->style().borderWidth;
    rig.dispatcher.setFocus(rig.strips->area(6));
    TEST_CHECK(background->style().border == palette.primary);
    TEST_CHECK(background->style().borderWidth == Style::focusRingWidth);
    rig.dispatcher.setFocus(nullptr);
    TEST_CHECK(background->style().borderWidth == edge);
    TEST_CHECK(background->style().border == palette.outlineVariant);
    palette = Palette{};
  }

  // The ancestors of a folder `depth` levels deep, ids by folder: level k is the folder `depth - k` deep, id 1000 plus
  // that depth.
  std::vector<AncestorEntry> folderChainOf(std::size_t depth) {
    std::vector<AncestorEntry> out;
    for (std::size_t level = 1; level <= depth; ++level) {
      out.push_back({.name = std::format("depth-{}", depth - level), .path = "/", .id = 1000 + depth - level});
    }
    return out;
  }

  // Going up keeps a deep tree's strips in place: the strip under the pointer now holds the next folder up.
  void hoverFollowsTheStripAfterGoingUp() {
    Rig rig(12, 209);
    std::size_t depth = 12;
    const auto goTo = [&rig, &depth](std::size_t next) {
      depth = next;
      rig.strips->setAncestors(folderChainOf(depth));
      rig.plan(depth);
    };
    rig.strips->onActivate = [&rig, &goTo](std::uint64_t id, std::uint32_t) {
      goTo(id - 1000);
      rig.hovers.emplace_back(); // the owner forgets the hover when the folder changes
    };
    goTo(12);
    rig.layout();
    const auto box = rig.strip(6);
    rig.dispatcher.pointerEnter(box.x + 10, 300, ++rig.serial);
    rig.layout();
    TEST_CHECK(rig.hovers.back() == 1011U);
    rig.dispatcher.pointerButton(box.x + 10, 300, BTN_LEFT, true, ++rig.serial, 1000, false);
    rig.dispatcher.pointerButton(box.x + 10, 300, BTN_LEFT, false, ++rig.serial, 1050, false);
    rig.layout();
    TEST_CHECK(depth == 11 && rig.strips->count() == 7);
    TEST_CHECK(rig.strips->label(6)->text() == "depth-10");
    rig.moveTo(box.x + 11, 301);
    TEST_CHECK(rig.hovers.back() == 1010U);
    const auto reported = rig.hovers.size();
    rig.moveTo(box.x + 12, 302); // the same folder is not reported twice
    TEST_CHECK(rig.hovers.size() == reported);
    rig.moveTo(rig.strip(0).x + 5, 300); // the stack points at no single folder
    TEST_CHECK(!rig.hovers.back().has_value());
  }

  // A strip a shallower tree no longer shows keeps no keys, though it may still hold the focus.
  void hiddenStripTakesNoKeys() {
    Rig rig(3, 107);
    std::size_t depth = 3;
    rig.strips->onActivate = [&rig, &depth](std::uint64_t id, std::uint32_t) {
      rig.activated.push_back(id);
      depth = id - 1000;
      rig.strips->setAncestors(folderChainOf(depth));
      rig.plan(depth);
    };
    rig.strips->setAncestors(folderChainOf(3));
    rig.layout();
    InputArea* nearest = rig.strips->area(2);
    rig.dispatcher.setFocus(nearest);
    rig.key(XKB_KEY_Return);
    TEST_CHECK((rig.activated == std::vector<std::uint64_t>{1002}));
    TEST_CHECK(depth == 2 && rig.strips->count() == 2 && !shown(nearest));
    TEST_CHECK(!nearest->enabled());
    rig.key(XKB_KEY_Return);
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.activated.size() == 1 && depth == 2);
    // shown again, the strip answers again
    rig.strips->setAncestors(folderChainOf(3));
    rig.plan(3);
    rig.layout();
    TEST_CHECK(rig.strips->area(2) == nearest && shown(nearest) && nearest->enabled());
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.activated.size() == 2 && rig.activated.back() == 1002);
  }

  void degenerateInputs() {
    Rig rig(3, 107);
    TEST_CHECK(rig.strips->count() == 3);
    rig.height = 40; // too short for any name
    rig.layout();
    for (std::size_t i = 0; i < 3; ++i) {
      TEST_CHECK(rig.strips->label(i) == nullptr);
      TEST_CHECK(rig.strip(i).height == 40);
    }
    rig.height = 600;
    // a plan for more levels than there are ancestors shows only the ancestors it has
    rig.strips->setPlan(luciole::workbench::ancestorStrips(5, 153));
    rig.layout();
    TEST_CHECK(rig.strips->count() == 3);
    rig.strips->setPlan({});
    rig.layout();
    TEST_CHECK(rig.strips->count() == 0);
    rig.strips->setAncestors({});
    rig.plan(4);
    rig.layout();
    TEST_CHECK(rig.strips->count() == 0);
    rig.clickAt(20, 300);
    TEST_CHECK(rig.activated.empty());
    // the pool never grows past one node per strip a plan can hold
    rig.width = 321;
    for (std::size_t count = 0; count <= 40; ++count) {
      rig.show(count);
      rig.layout();
    }
    TEST_CHECK(rig.strips->children().size() == AncestorStrips::kMaxStrips);
  }

  void rightToLeftMirrors() {
    Style::setRtl(true);
    {
      Rig rig(4, 128);
      TEST_CHECK(rig.strip(3).x == 10 + 1 && rig.strip(3).width == 44); // the nearest is on the left, by the list
      TEST_CHECK(rig.strip(0).x == 10 + 128 - 20 && rig.strip(0).width == 20);
      rig.click(3);
      TEST_CHECK(rig.activated.back() == 101);
      rig.width = 200; // the room beyond the bands is on the right
      rig.layout();
      TEST_CHECK(rig.strip(3).x == 10 + 1 && rig.strip(0).x == 10 + 128 - 20);
    }
    Style::setRtl(false);
  }
} // namespace

int main() {
  bandsThinWithDistanceAgainstTheList();
  everyBandIsNamed();
  farLevelsFoldIntoOneBand();
  clickGoesToTheAncestor();
  activationNamesItsButton();
  hoverPointedAndFocusShow();
  hoverFollowsTheStripAfterGoingUp();
  hiddenStripTakesNoKeys();
  degenerateInputs();
  rightToLeftMirrors();
  std::println("ancestor strips checks passed");
}
