#include "core/input/key_modifiers.h"
#include "ui/controls/button.h"
#include "ui/controls/pane_divider.h"
#include "ui/workbench/ancestor_strips.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench/column_plan.h"
#include "workbench_view_rig.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <print>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

// The ancestor strips pane in a whole Workbench view: its divider, its saved width, Reset layout, the room beyond
// its bands, and the list's left edge across folders of different depth.
namespace {
  using namespace luciole;
  using namespace luciole::test::view;
  namespace wb = luciole::workbench;

  float sceneX(const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return x;
  }

  struct Bands {
    std::size_t named = 0;
    std::size_t folded = 0; // ancestors in the "+N levels" band; 0 without one
  };

  // The bands the center shows; every one of them carries a name or its levels.
  Bands bandsOf(const WorkbenchCenterView& center) {
    Bands out;
    const auto& strips = center.strips();
    TEST_CHECK(strips.count() == center.plan().strips.size());
    for (std::size_t i = 0; i < strips.count(); ++i) {
      const auto& strip = center.plan().strips[i];
      TEST_CHECK(strips.label(i) != nullptr);
      if (strip.kind == wb::StripKind::Stack) {
        out.folded = strip.lastLevel - strip.level + 1;
      } else {
        ++out.named;
      }
    }
    return out;
  }

  // The pane's drawn width, from where the list starts.
  float paneWidth(const Rig& rig) {
    const auto& center = rig.view->center();
    return sceneX(*center.list()) - sceneX(center);
  }

  // Presses, drags and keys at increasing times.
  class Pointer {
  public:
    explicit Pointer(Rig& owner) : m_rig(&owner) {}

    std::uint32_t time = 1000;

    void press(Point at) {
      m_rig->move(at);
      m_rig->dispatcher.pointerButton(at.x, at.y, BTN_LEFT, true, 2, time, false);
    }
    void release(Point at) {
      m_rig->dispatcher.pointerButton(at.x, at.y, BTN_LEFT, false, 3, time + 50, false);
      time += 1000; // the next press is never the second half of a double press
      m_rig->frame();
    }
    // Drags the strips divider by `travel`, 4 px a frame, and lets go.
    void drag(float travel) {
      const auto at = middle(m_rig->view->center().stripsDivider());
      press(at);
      const float step = travel < 0 ? -4.0F : 4.0F;
      for (float moved = 0; std::abs(moved) < std::abs(travel);) { // at most |travel| / 4 + 1 frames
        moved = std::abs(moved + step) > std::abs(travel) ? travel : moved + step;
        m_rig->move({.x = at.x + moved, .y = at.y});
        m_rig->frame();
      }
      release({.x = at.x + travel, .y = at.y});
    }
    void key(std::uint32_t sym, std::uint32_t modifiers = 0) {
      m_rig->dispatcher.keyEvent(sym, 0, modifiers, true);
      m_rig->dispatcher.keyEvent(sym, 0, modifiers, false);
      m_rig->frame();
    }

  private:
    Rig* m_rig = nullptr;
  };

  // Frames until the folder is measured and the motion has ended; a folder without subfolders lists no column ahead.
  void rest(Rig& rig) {
    rig.until([&rig] {
      const auto snapshot = rig.model.snapshot();
      return snapshot
          && !rig.view->center().ancestors().empty()
          && !rig.model.measuring()
          && !rig.animations.hasActive();
    });
  }

  void setStripsWidth(Rig& rig, std::optional<double> width) {
    auto geometry = rig.view->geometry();
    geometry.strips = width;
    rig.view->setGeometry(geometry);
    rig.frame();
  }

  // The pane keeps the default 153 px whatever the depth, so the list does not move sideways going down and up.
  void listEdgeStaysAcrossDepths() {
    Rig rig;
    rig.settle();
    const auto& center = rig.view->center();
    const std::size_t base = center.ancestors().size();
    TEST_CHECK(base >= 1);
    const float listX = sceneX(*center.list());
    TEST_CHECK(paneWidth(rig) == 153);
    const std::array<const char*, 5> paths{"big", "big/deep", "big/deep/inner", "big", ""};
    const std::array<std::size_t, 5> deeper{1, 2, 3, 1, 0};
    for (std::size_t i = 0; i < paths.size(); ++i) {
      rig.open(paths[i]);
      rest(rig);
      const std::size_t count = base + deeper[i];
      TEST_CHECK(center.ancestors().size() == count);
      TEST_CHECK(sceneX(*center.list()) == listX);
      TEST_CHECK(center.plan().stripsWidth == 153);
      // the default pane shows four named bands and folds the rest, or all of them when they fit
      const auto bands = bandsOf(center);
      if (count <= 5) {
        TEST_CHECK(bands.named == count && bands.folded == 0);
      } else {
        TEST_CHECK(bands.named == 4 && bands.folded == count - 4);
      }
    }
    TEST_CHECK(rig.geometries.empty());
  }

  // Dragging the divider widens or narrows the pane; bands appear and fold as it does, and the width is reported.
  void dragShowsAndFoldsBands() {
    Rig rig;
    rig.open("big/deep/inner");
    rest(rig);
    const auto& center = rig.view->center();
    const std::size_t count = center.ancestors().size();
    TEST_CHECK(count >= 4 && count <= 12);
    const float paneX = sceneX(center);
    Pointer pointer(rig);

    pointer.drag(-83); // to 70: one named band and the band of the rest
    TEST_CHECK(paneWidth(rig) == 70 && center.plan().stripsWidth == 70);
    TEST_CHECK(bandsOf(center).named == 1 && bandsOf(center).folded == count - 1);
    TEST_CHECK(rig.geometries.size() == 1 && rig.geometries.back().strips == 70.0);
    TEST_CHECK(rig.geometries.back().columns == rig.view->geometry().columns);

    pointer.drag(-40); // the narrowest pane holds
    TEST_CHECK(paneWidth(rig) == 70 && rig.geometries.back().strips == 70.0);

    pointer.drag(34); // 104, one pixel short of a second named band beside the "+N levels" band
    TEST_CHECK(paneWidth(rig) == 104 && bandsOf(center).named == 1);
    pointer.drag(-10);
    pointer.drag(11); // 105: two named bands
    TEST_CHECK(paneWidth(rig) == 105 && bandsOf(center).named == 2 && bandsOf(center).folded == count - 2);

    pointer.drag(400); // the widest pane: every ancestor named
    TEST_CHECK(paneWidth(rig) == 321 && bandsOf(center).named == count && bandsOf(center).folded == 0);
    TEST_CHECK(rig.geometries.back().strips == 321.0);
    TEST_CHECK(sceneX(center) == paneX && !rig.view->resizing());
    // the room beyond the bands is empty and takes no clicks
    const float bandsLeft = sceneX(*center.strips().area(0));
    TEST_CHECK(bandsLeft == paneX + 321 - static_cast<float>(wb::stripBandsWidth(count)));
    const auto gone = rig.gone.size();
    rig.click({.x = paneX + 10, .y = middle(center).y});
    rig.frame();
    TEST_CHECK(rig.gone.size() == gone);
  }

  // Keys step the divider, Home and a double press reset it, and Escape abandons a drag.
  void keysResetAndEscape() {
    Rig rig;
    rig.settle();
    auto& divider = rig.view->center().stripsDivider();
    Pointer pointer(rig);
    rig.dispatcher.setFocus(&divider);
    pointer.key(XKB_KEY_Right);
    TEST_CHECK(paneWidth(rig) == 169 && rig.geometries.back().strips == 169.0);
    pointer.key(XKB_KEY_Right, KeyMod::Shift);
    TEST_CHECK(paneWidth(rig) == 233);
    pointer.key(XKB_KEY_Left);
    TEST_CHECK(paneWidth(rig) == 217 && rig.geometries.size() == 3);
    pointer.key(XKB_KEY_Home);
    TEST_CHECK(paneWidth(rig) == 153 && rig.geometries.size() == 4 && !rig.geometries.back().strips);
    rig.dispatcher.setFocus(nullptr);

    pointer.drag(60);
    TEST_CHECK(paneWidth(rig) == 213 && rig.geometries.size() == 5);
    const auto at = middle(divider);
    pointer.press(at);
    rig.dispatcher.pointerButton(at.x, at.y, BTN_LEFT, false, 3, pointer.time + 50, false);
    pointer.time += 100;
    pointer.press(at);
    pointer.release(at);
    TEST_CHECK(paneWidth(rig) == 153 && rig.geometries.size() == 6 && !rig.geometries.back().strips);

    const auto edge = middle(divider);
    pointer.press(edge);
    for (int frame = 1; frame <= 10; ++frame) {
      rig.move({.x = edge.x + (6.0F * static_cast<float>(frame)), .y = edge.y});
      rig.frame();
    }
    TEST_CHECK(paneWidth(rig) == 213 && rig.view->resizing());
    setStripsWidth(rig, 300); // ignored while dragging
    pointer.key(XKB_KEY_Escape);
    TEST_CHECK(paneWidth(rig) == 153 && !rig.view->resizing() && !rig.view->geometry().strips);
    pointer.release({.x = edge.x + 60, .y = edge.y});
    TEST_CHECK(paneWidth(rig) == 153 && rig.geometries.size() == 6);
  }

  // A saved width comes back on start, a narrow window draws the pane narrower without changing it, and Reset layout
  // puts the default back.
  void savedWidthAndResetLayout() {
    Rig rig;
    rig.settle();
    setStripsWidth(rig, 250);
    TEST_CHECK(paneWidth(rig) == 250 && rig.geometries.empty());
    TEST_CHECK(rig.view->geometry().strips == 250.0);
    Pointer pointer(rig);
    pointer.drag(30);
    const auto saved = rig.geometries.back();
    TEST_CHECK(saved.strips == 280.0);
    {
      Rig again; // a new window starts from the saved geometry
      again.view->setGeometry(saved);
      again.settle();
      TEST_CHECK(paneWidth(again) == 280 && again.geometries.empty());
    }

    rig.resize(860, 900);
    const auto& center = rig.view->center();
    const float drawn = paneWidth(rig);
    TEST_CHECK(drawn >= 70 && drawn < 280);
    TEST_CHECK(center.plan().columns.empty() && center.plan().list.width == 320); // the columns gave up their room
    TEST_CHECK(rig.view->geometry().strips == 280.0 && rig.geometries.size() == 1);
    rig.resize(1400, 900);
    TEST_CHECK(paneWidth(rig) == 280);

    rig.click(rig.view->resetButton(wb::Pane::Ribbon));
    rig.frame();
    TEST_CHECK(paneWidth(rig) == 153);
    TEST_CHECK(rig.geometries.size() == 2 && rig.geometries.back() == WorkbenchGeometry{});
  }

  // A key on the divider of a pane the window squeezed steps the saved width, not the width drawn.
  void keysStepTheSavedWidth() {
    Rig rig;
    rig.settle();
    setStripsWidth(rig, 280);
    rig.resize(860, 900);
    const float drawn = paneWidth(rig);
    TEST_CHECK(drawn >= 70 && drawn < 280 && rig.geometries.empty());
    Pointer pointer(rig);
    rig.dispatcher.setFocus(&rig.view->center().stripsDivider());
    pointer.key(XKB_KEY_Right);
    TEST_CHECK(rig.geometries.size() == 1 && rig.geometries.back().strips == 296.0);
    pointer.key(XKB_KEY_Left);
    pointer.key(XKB_KEY_Left);
    TEST_CHECK(rig.geometries.back().strips == 264.0);
    TEST_CHECK(paneWidth(rig) == drawn);
    rig.resize(1400, 900);
    TEST_CHECK(paneWidth(rig) == 264);
  }

  // With the strips off, the pane and its divider are gone and the list starts at the center's edge.
  void stripsOffRemoveThePane() {
    Rig rig;
    rig.settle();
    setStripsWidth(rig, 250);
    auto choices = rig.view->choices();
    choices.strips = false;
    rig.view->setChoices(choices);
    rig.frame();
    rig.frame();
    const auto& center = rig.view->center();
    TEST_CHECK(!shown(&center.stripsDivider()) && !shown(&center.strips()));
    TEST_CHECK(paneWidth(rig) == 0);
    choices.strips = true;
    rig.view->setChoices(choices);
    rig.frame();
    rig.frame();
    TEST_CHECK(shown(&center.stripsDivider()) && paneWidth(rig) == 250);
    TEST_CHECK(rig.geometries.empty());
  }
} // namespace

int main() {
  listEdgeStaysAcrossDepths();
  dragShowsAndFoldsBands();
  keysResetAndEscape();
  savedWidthAndResetLayout();
  keysStepTheSavedWidth();
  stripsOffRemoveThePane();
  std::println("workbench strips pane checks passed");
  return 0;
}
