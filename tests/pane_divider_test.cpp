#include "core/input/key_modifiers.h"
#include "cursor-shape-v1-client-protocol.h"
#include "metrics_renderer.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/pane_divider.h"
#include "ui/controls/pane_divider_focus.h"
#include "ui/style.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <print>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  // A divider in a 400 by 300 scene whose owner moves it to every value it reports, like a pane shell does.
  struct Rig {
    test::MetricsRenderer renderer;
    Node root;
    PaneDivider* divider = nullptr;
    InputDispatcher dispatcher;
    PaneDividerFocus focus{dispatcher};
    std::vector<float> previews;
    std::vector<float> commits;
    std::vector<float> cancels;
    int resets = 0;
    std::uint32_t cursor = 0;
    std::uint32_t serial = 0;

    explicit Rig(PaneDividerAxis axis) {
      root.setSize(400, 300);
      divider = static_cast<PaneDivider*>(root.addChild(std::make_unique<PaneDivider>(axis)));
      divider->setCallbacks({
          .preview =
              [this](float value) {
                previews.push_back(value);
                place(value);
              },
          .commit =
              [this](float value) {
                commits.push_back(value);
                place(value);
              },
          .cancel =
              [this](float value) {
                cancels.push_back(value);
                place(value);
              },
          .reset =
              [this] {
                ++resets;
                place(100);
              },
      });
      place(200);
      dispatcher.setSceneRoot(&root);
      dispatcher.setFocusChangeCallback([this](InputArea* old, InputArea* now) { focus.changed(old, now); });
      dispatcher.setCursorShapeCallback([this](std::uint32_t, std::uint32_t shape) { cursor = shape; });
    }

    // The owner's layout: the 6 px hit area is centered on the value.
    void place(float value) {
      divider->setValue(value);
      const bool column = divider->axis() == PaneDividerAxis::Column;
      const float x = column ? (Style::rtl() ? 400 - value : value) - 3 : 0;
      const float y = column ? 0 : value - 3;
      divider->arrange(renderer, {x, y, column ? 6.0F : 400.0F, column ? 300.0F : 6.0F});
    }

    void move(float x, float y) { dispatcher.pointerMotion(x, y, ++serial); }
    void press(float x, float y, std::uint32_t time) {
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, time, false);
    }
    void release(float x, float y, std::uint32_t time) {
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, time, false);
    }
    void key(std::uint32_t sym, std::uint32_t modifiers = 0) {
      dispatcher.keyEvent(sym, 0, modifiers, true);
      dispatcher.keyEvent(sym, 0, modifiers, false);
    }
    // A focusable area clear of the divider that counts the keys it gets, as the media preview does.
    InputArea* addTarget(int& keys) {
      auto area = std::make_unique<InputArea>();
      area->setPosition(0, 0);
      area->setSize(100, 100);
      area->setFocusable(true);
      area->setOnKeyDown([&keys](const InputArea::KeyData&) { ++keys; });
      return static_cast<InputArea*>(root.addChild(std::move(area)));
    }
    const Box& line() const {
      const Box* found = nullptr;
      for (const auto& child : divider->children()) {
        if (const auto* box = dynamic_cast<const Box*>(child.get())) {
          found = box;
        }
      }
      TEST_CHECK(found != nullptr);
      return *found;
    }
  };

  void dragFollowsTheStartValue() {
    Rig rig(PaneDividerAxis::Column);
    rig.press(200, 50, 1000);
    TEST_CHECK(rig.divider->dragging());
    rig.move(203, 50); // under the drag threshold: nothing moves yet
    TEST_CHECK(rig.previews.empty());
    rig.move(250, 50);
    rig.move(260, 80);
    rig.move(240, 80);
    // each value is the start value plus the pointer travel since the press, never a sum of steps
    TEST_CHECK((rig.previews == std::vector<float>{250, 260, 240}));
    TEST_CHECK(rig.commits.empty());
    rig.release(240, 80, 1100);
    TEST_CHECK((rig.commits == std::vector<float>{240}));
    TEST_CHECK(!rig.divider->dragging());
    rig.move(300, 80);
    TEST_CHECK(rig.previews.size() == 3);
    TEST_CHECK(rig.commits.size() == 1);
  }

  void rowDragUsesVerticalTravel() {
    Rig rig(PaneDividerAxis::Row);
    rig.press(150, 200, 1000);
    rig.move(10, 230);
    rig.release(10, 230, 1100);
    TEST_CHECK((rig.previews == std::vector<float>{230}));
    TEST_CHECK((rig.commits == std::vector<float>{230}));
  }

  void escapeRestoresTheStart() {
    Rig rig(PaneDividerAxis::Column);
    rig.press(200, 50, 1000);
    rig.move(280, 50);
    rig.key(XKB_KEY_Escape);
    TEST_CHECK((rig.cancels == std::vector<float>{200}));
    TEST_CHECK(rig.divider->value() == 200);
    TEST_CHECK(!rig.divider->dragging());
    rig.move(300, 50);
    rig.release(300, 50, 1100);
    TEST_CHECK(rig.previews.size() == 1);
    TEST_CHECK(rig.commits.empty());
  }

  void lostCaptureRestoresTheStart() {
    Rig rig(PaneDividerAxis::Column);
    rig.press(200, 50, 1000);
    rig.move(150, 50);
    rig.dispatcher.cancelPointerCapture();
    TEST_CHECK((rig.cancels == std::vector<float>{200}));
    TEST_CHECK(rig.commits.empty());
    TEST_CHECK(!rig.divider->dragging());
  }

  void doublePressResets() {
    Rig rig(PaneDividerAxis::Column);
    rig.press(200, 50, 1000);
    rig.release(200, 50, 1080);
    TEST_CHECK(rig.commits.empty()); // a press without a drag changes nothing
    rig.press(202, 54, 1300);
    TEST_CHECK(rig.resets == 1);
    TEST_CHECK(!rig.divider->dragging());
    rig.release(202, 54, 1350);
    TEST_CHECK(rig.commits.empty());
    // a third press starts over instead of resetting again
    rig.press(100, 50, 1500);
    TEST_CHECK(rig.resets == 1);
    rig.release(100, 50, 1550);
  }

  void slowOrDistantSecondPressDoesNotReset() {
    Rig rig(PaneDividerAxis::Column);
    rig.press(200, 50, 1000);
    rig.release(200, 50, 1050);
    rig.press(200, 50, 1401);
    rig.release(200, 50, 1450);
    TEST_CHECK(rig.resets == 0);
    rig.press(200, 120, 1500);
    TEST_CHECK(rig.resets == 0);
    rig.release(200, 120, 1550);
    // a drag in between is not the first half of a double press
    rig.press(200, 50, 2000);
    rig.move(230, 50);
    rig.release(230, 50, 2050);
    rig.press(230, 50, 2100);
    TEST_CHECK(rig.resets == 0);
    TEST_CHECK(rig.divider->dragging());
    rig.release(230, 50, 2150);
  }

  void keysStepAndHomeResets() {
    Rig rig(PaneDividerAxis::Column);
    rig.dispatcher.setFocus(rig.divider);
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right, KeyMod::Shift);
    rig.key(XKB_KEY_Left);
    rig.key(XKB_KEY_Up); // not this divider's axis
    rig.key(XKB_KEY_Right, KeyMod::Ctrl);
    TEST_CHECK((rig.commits == std::vector<float>{216, 280, 264}));
    rig.key(XKB_KEY_Home);
    TEST_CHECK(rig.resets == 1);
    TEST_CHECK(rig.previews.empty());
    Rig row(PaneDividerAxis::Row);
    row.dispatcher.setFocus(row.divider);
    row.key(XKB_KEY_Down);
    row.key(XKB_KEY_Up, KeyMod::Shift);
    row.key(XKB_KEY_Right);
    TEST_CHECK((row.commits == std::vector<float>{216, 152}));
  }

  void cursorFocusAndLook() {
    Rig rig(PaneDividerAxis::Column);
    TEST_CHECK(rig.line().width() == 1);
    rig.dispatcher.pointerEnter(200, 50, 1);
    TEST_CHECK(rig.cursor == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
    TEST_CHECK(rig.line().width() == 3);
    rig.dispatcher.pointerMotion(390, 50, 2);
    TEST_CHECK(rig.line().width() == 1);
    TEST_CHECK(rig.dispatcher.cycleTabFocus(false));
    TEST_CHECK(rig.dispatcher.focusedArea() == rig.divider);
    TEST_CHECK(rig.line().width() == 3);
    TEST_CHECK(rig.line().x() + 1.5F == 3.0F); // centered in the hit area
    Rig row(PaneDividerAxis::Row);
    row.dispatcher.pointerEnter(100, 200, 1);
    TEST_CHECK(row.cursor == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE);
    TEST_CHECK(row.line().height() == 3);
  }

  void fixedDividerIgnoresInput() {
    Rig rig(PaneDividerAxis::Column);
    auto under = std::make_unique<InputArea>();
    under->setZIndex(-1);
    under->setSize(400, 300);
    const auto* below = rig.root.addChild(std::move(under));
    TEST_CHECK(rig.dispatcher.inputAreaAt(200, 50) == rig.divider);
    rig.divider->setDraggable(false);
    // a fixed divider is only a line: what lies under it takes the pointer
    TEST_CHECK(rig.dispatcher.inputAreaAt(200, 50) == below);
    TEST_CHECK(!rig.dispatcher.cycleTabFocus(false));
    rig.press(200, 50, 1000);
    rig.move(260, 50);
    rig.release(260, 50, 1100);
    TEST_CHECK(rig.previews.empty() && rig.commits.empty());
    TEST_CHECK(rig.line().width() == 1);
    // a divider that stops being draggable mid-drag keeps its drag
    rig.divider->setDraggable(true);
    TEST_CHECK(rig.dispatcher.inputAreaAt(200, 50) == rig.divider);
    rig.press(200, 50, 2000);
    rig.divider->setDraggable(false);
    rig.move(230, 50);
    rig.release(230, 50, 2100);
    TEST_CHECK((rig.commits == std::vector<float>{230}));
  }

  void mirrorsInRightToLeft() {
    Style::setRtl(true);
    Rig rig(PaneDividerAxis::Column);
    // value 200 sits at x 200 from the right edge
    rig.press(200, 50, 1000);
    rig.move(150, 50);
    rig.release(150, 50, 1100);
    TEST_CHECK((rig.previews == std::vector<float>{250}));
    TEST_CHECK((rig.commits == std::vector<float>{250}));
    rig.dispatcher.setFocus(rig.divider);
    rig.key(XKB_KEY_Left);
    TEST_CHECK(rig.commits.back() == 266);
    Rig row(PaneDividerAxis::Row);
    row.press(100, 200, 1000);
    row.move(100, 220);
    row.release(100, 220, 1100);
    TEST_CHECK((row.commits == std::vector<float>{220}));
    Style::setRtl(false);
  }

  void hostileValues() {
    Rig rig(PaneDividerAxis::Column);
    rig.divider->setValue(std::numeric_limits<float>::quiet_NaN());
    TEST_CHECK(rig.divider->value() == 200);
    rig.divider->setValue(std::numeric_limits<float>::infinity());
    TEST_CHECK(rig.divider->value() == 200);
    rig.divider->setValue(-20);
    TEST_CHECK(rig.divider->value() == -20);
  }

  // A drag or a click on a divider leaves the keyboard where it was.
  void pointerKeepsTheFocus() {
    Rig rig(PaneDividerAxis::Column);
    int keys = 0;
    auto* target = rig.addTarget(keys);
    rig.dispatcher.setFocus(target);
    rig.press(200, 50, 1000);
    rig.move(260, 50);
    rig.release(260, 50, 1100);
    TEST_CHECK((rig.commits == std::vector<float>{260}));
    TEST_CHECK(rig.dispatcher.focusedArea() == target);
    rig.key(XKB_KEY_space);
    TEST_CHECK(keys == 1);
    rig.press(260, 50, 2000); // a click without a drag
    rig.release(260, 50, 2050);
    TEST_CHECK(rig.dispatcher.focusedArea() == target);
    rig.key(XKB_KEY_space);
    TEST_CHECK(keys == 2);
    Rig row(PaneDividerAxis::Row);
    int rowKeys = 0;
    auto* rowTarget = row.addTarget(rowKeys);
    row.dispatcher.setFocus(rowTarget);
    row.press(50, 200, 1000);
    row.move(50, 240);
    row.release(50, 240, 1100);
    TEST_CHECK(row.dispatcher.focusedArea() == rowTarget);
  }

  void pointerWithoutFocusLeavesNone() {
    Rig rig(PaneDividerAxis::Column);
    rig.press(200, 50, 1000);
    rig.move(260, 50);
    rig.release(260, 50, 1100);
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr);
  }

  // A divider reached with Tab holds the focus and its arrow keys move it, also after the pointer has been over it.
  void tabbedDividerKeepsKeys() {
    Rig rig(PaneDividerAxis::Column);
    int keys = 0;
    auto* target = rig.addTarget(keys);
    rig.dispatcher.setFocus(target);
    TEST_CHECK(rig.dispatcher.cycleTabFocus(false));
    TEST_CHECK(rig.dispatcher.focusedArea() == rig.divider);
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right, KeyMod::Shift);
    TEST_CHECK((rig.commits == std::vector<float>{216, 280}));
    TEST_CHECK(keys == 0);
    rig.dispatcher.pointerEnter(280, 50, 1); // the pointer now rests on the focused divider
    rig.key(XKB_KEY_Left);
    TEST_CHECK(rig.dispatcher.focusedArea() == rig.divider);
    TEST_CHECK((rig.commits == std::vector<float>{216, 280, 264}));
    rig.dispatcher.pointerMotion(390, 50, 2); // away again: the divider is a tab stop as before
    TEST_CHECK(rig.dispatcher.cycleTabFocus(false) && rig.dispatcher.focusedArea() == target);
    TEST_CHECK(rig.dispatcher.cycleTabFocus(false) && rig.dispatcher.focusedArea() == rig.divider);
  }

  // A click on a divider that had the focus by Tab ends like any press: the focus goes back to where Tab came from.
  void clickOnATabbedDivider() {
    Rig rig(PaneDividerAxis::Column);
    int keys = 0;
    auto* target = rig.addTarget(keys);
    rig.dispatcher.setFocus(target);
    TEST_CHECK(rig.dispatcher.cycleTabFocus(false) && rig.dispatcher.focusedArea() == rig.divider);
    rig.press(200, 50, 1000);
    rig.release(200, 50, 1050);
    TEST_CHECK(rig.dispatcher.focusedArea() == target);
  }

  // The area that had the focus is gone or out of the scene: the press ends as it always did.
  void goneAreaIsNotRestored() {
    Rig rig(PaneDividerAxis::Column);
    int keys = 0;
    auto* target = rig.addTarget(keys);
    rig.dispatcher.setFocus(target);
    rig.press(200, 50, 1000);
    rig.move(260, 50);
    auto detached = rig.root.removeChild(target);
    rig.release(260, 50, 1100);
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr);
    detached.reset();
    Rig second(PaneDividerAxis::Column);
    auto* other = second.addTarget(keys);
    second.dispatcher.setFocus(other);
    second.press(200, 50, 1000);
    second.move(260, 50);
    TEST_CHECK(second.root.removeChild(other) != nullptr);
    second.release(260, 50, 1100); // destroyed at once: the remembered area is forgotten
    TEST_CHECK(second.dispatcher.focusedArea() == nullptr);
  }

  // The focus is cleared while the pointer is still down (a tab switch, a dialog): nothing takes it back.
  void clearBeforeReleaseRestoresNothing() {
    Rig rig(PaneDividerAxis::Column);
    int keys = 0;
    auto* target = rig.addTarget(keys);
    rig.dispatcher.setFocus(target);
    rig.press(200, 50, 1000);
    rig.move(260, 50);
    TEST_CHECK(rig.dispatcher.focusedArea() == rig.divider);
    rig.dispatcher.setFocus(nullptr);
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr);
    rig.release(260, 50, 1100);
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr);
    rig.key(XKB_KEY_space);
    TEST_CHECK(keys == 0);
    rig.press(260, 50, 2000); // the forgotten area is not offered to a later press
    rig.release(260, 50, 2050);
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr);
  }

  // A button that clears its own focus is not "the area before" of a divider reached later with Tab.
  void clearedAreaIsNotRememberedForTab() {
    Rig rig(PaneDividerAxis::Column);
    int keys = 0;
    auto* target = rig.addTarget(keys);
    rig.press(50, 50, 500);
    rig.release(50, 50, 550);
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr);
    TEST_CHECK(rig.dispatcher.cycleTabFocus(false) && rig.dispatcher.focusedArea() == rig.divider);
    rig.press(200, 50, 1000);
    rig.release(200, 50, 1050);
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr && rig.dispatcher.focusedArea() != target);
    rig.key(XKB_KEY_space);
    TEST_CHECK(keys == 0);
  }

  // Tab from the area to one divider and on to another: a click ends where Tab started, as with one divider.
  void tabbingBetweenDividersKeepsTheOrigin() {
    Rig rig(PaneDividerAxis::Column);
    int keys = 0;
    auto* other = static_cast<PaneDivider*>(rig.root.addChild(std::make_unique<PaneDivider>(PaneDividerAxis::Column)));
    other->arrange(rig.renderer, {297, 0, 6, 300});
    auto* target = rig.addTarget(keys);
    rig.dispatcher.setFocus(target);
    TEST_CHECK(rig.dispatcher.cycleTabFocus(false) && rig.dispatcher.focusedArea() == rig.divider);
    TEST_CHECK(rig.dispatcher.cycleTabFocus(false) && rig.dispatcher.focusedArea() == other);
    rig.press(300, 50, 1000);
    rig.release(300, 50, 1050);
    TEST_CHECK(rig.dispatcher.focusedArea() == target);
  }

  // An area destroyed while it is only remembered is not touched when the divider's focus is cleared later.
  void areaGoneWhileRemembered() {
    Rig rig(PaneDividerAxis::Column);
    int keys = 0;
    auto* target = rig.addTarget(keys);
    rig.dispatcher.setFocus(target);
    rig.press(200, 50, 1000);
    TEST_CHECK(rig.dispatcher.focusedArea() == rig.divider);
    TEST_CHECK(rig.root.removeChild(target) != nullptr);
    rig.dispatcher.setFocus(nullptr);
    rig.release(200, 50, 1100);
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr);
    rig.key(XKB_KEY_space);
    TEST_CHECK(keys == 0);
  }
} // namespace

int main() {
  dragFollowsTheStartValue();
  rowDragUsesVerticalTravel();
  escapeRestoresTheStart();
  lostCaptureRestoresTheStart();
  doublePressResets();
  slowOrDistantSecondPressDoesNotReset();
  keysStepAndHomeResets();
  cursorFocusAndLook();
  fixedDividerIgnoresInput();
  mirrorsInRightToLeft();
  hostileValues();
  pointerKeepsTheFocus();
  pointerWithoutFocusLeavesNone();
  tabbedDividerKeepsKeys();
  clickOnATabbedDivider();
  goneAreaIsNotRestored();
  clearBeforeReleaseRestoresNothing();
  clearedAreaIsNotRememberedForTab();
  tabbingBetweenDividersKeepsTheOrigin();
  areaGoneWhileRemembered();
  std::println("pane divider checks passed");
}
