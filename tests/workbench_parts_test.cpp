#include "render/scene/text_node.h"
#include "workbench/iris_hit.h"
#include "workbench_view_rig.h"

#include <cstddef>
#include <format>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The instrument and the ribbon inside a Workbench view: the page's folder trail reaching the Iris hub, and where
// the list of an Iris "+N more" opens from the keyboard.
namespace {
  using namespace luciole;
  using namespace luciole::test::view;
  namespace wb = luciole::workbench;

  void choose(Rig& rig, WorkbenchInstrument kind) {
    auto choices = rig.view->choices();
    choices.instrument = kind;
    rig.view->setChoices(choices);
    rig.frame();
  }

  void expand(Rig& rig) {
    auto geometry = rig.view->geometry();
    geometry.panes.expanded = true;
    rig.view->setGeometry(geometry);
    rig.frame();
  }

  // Opens `relative` and frames until both parts show its measured snapshot.
  void enter(Rig& rig, const std::string& relative) {
    rig.open(relative);
    rig.until([&rig, &relative] {
      const auto snapshot = rig.model.snapshot();
      return snapshot
          && snapshot->location == rig.fixture.at(relative)
          && !rig.model.measuring()
          && rig.view->instrument().shownSnapshot().serial == snapshot->serial
          && !rig.animations.hasActive();
    });
  }

  // The text of each trail chip of the Iris hub, in order.
  std::vector<std::string> chipTexts(const PartView& part) {
    const auto& view = part.shapeView();
    float originX = 0;
    float originY = 0;
    Node::absolutePosition(&view, originX, originY);
    std::vector<std::string> texts;
    for (std::size_t index = 0; index < 4; ++index) {
      for (const auto& shape : view.shapes()) {
        if (shape.id != wb::iris::chipId(index) || !shape.hittable) {
          continue;
        }
        std::string text;
        for (const auto& child : view.labelLayer().children()) {
          const auto* label = dynamic_cast<const TextNode*>(child.get());
          if (label == nullptr || !label->visible()) {
            continue;
          }
          float x = 0;
          float y = 0;
          Node::absolutePosition(label, x, y);
          const float middleY = y - originY + (label->height() / 2);
          const float left = x - originX;
          if (left >= shape.rect.x
              && left <= shape.rect.x + shape.rect.width
              && middleY >= shape.rect.y
              && middleY <= shape.rect.y + shape.rect.height) {
            text = label->text();
          }
        }
        texts.push_back(text);
      }
    }
    return texts;
  }

  // The page records big/deep while Nest is the instrument; Iris then offers the way back down, and keeps offering
  // it after a switch to Nest and back.
  void irisChipsFollowThePageTrail() {
    Rig rig;
    rig.settle();
    expand(rig);
    enter(rig, "big/deep");
    enter(rig, "");
    choose(rig, WorkbenchInstrument::Iris);
    rig.until([&rig] { return !rig.animations.hasActive() && !chipTexts(rig.view->instrument()).empty(); });
    const auto chips = chipTexts(rig.view->instrument());
    TEST_CHECK((chips == std::vector<std::string>{"big", "deep"}));
    choose(rig, WorkbenchInstrument::Nest);
    rig.until([&rig] { return !rig.animations.hasActive(); });
    TEST_CHECK(chipTexts(rig.view->instrument()).empty());
    choose(rig, WorkbenchInstrument::Iris);
    rig.until([&rig] { return !rig.animations.hasActive() && !chipTexts(rig.view->instrument()).empty(); });
    TEST_CHECK(chipTexts(rig.view->instrument()) == chips);
  }
  // many/ holds 100 folders of one file each: the full dial ends in a "+N more" row. Enter on it lists the entries at
  // that row, where Iris puts the list, not at the middle of its arc.
  void irisMoreOpensAtItsRowFromTheKeyboard() {
    Rig rig;
    for (std::size_t i = 0; i < 100; ++i) {
      Fixture::write(rig.fixture.root / "many" / std::format("d{:03}", i) / "f.bin", (i + 1) * 10);
    }
    rig.settle();
    expand(rig);
    choose(rig, WorkbenchInstrument::Iris);
    enter(rig, "many");
    const auto& part = rig.view->instrument();
    const auto folder = rig.model.snapshot()->tree.root;
    const wb::Shape* row = nullptr;
    for (const auto& shape : part.shapeView().shapes()) {
      if (shape.id.node == folder && wb::sceneRole(shape.id.role) == wb::SceneRole::Row && shape.hittable) {
        row = &shape;
      }
    }
    TEST_CHECK(row != nullptr);
    const auto rowX = row->rect.x + (row->rect.width / 2);
    const auto rowY = row->rect.y + (row->rect.height / 2);
    Point origin;
    Node::absolutePosition(&part.shapeView(), origin.x, origin.y);
    rig.dispatcher.setFocus(&part.shapeView().inputArea());
    rig.dispatcher.keyEvent(XKB_KEY_End, 0, 0, true);
    rig.frame();
    rig.dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    TEST_CHECK(rig.mores.size() == 1 && rig.mores.back().folder == rig.fixture.at("many"));
    TEST_NEAR(rig.mores.back().sceneX, origin.x + rowX, 0.5F);
    TEST_NEAR(rig.mores.back().sceneY, origin.y + rowY, 0.5F);
  }
} // namespace

int main() {
  irisChipsFollowThePageTrail();
  irisMoreOpensAtItsRowFromTheKeyboard();
  return 0;
}
