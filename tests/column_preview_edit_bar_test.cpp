#include "metrics_renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/workbench/column_preview_view.h"

#include <cmath>
#include <initializer_list>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <variant>

// The edit bar under a column preview's header, driven through a real dispatcher over the view.
namespace {
  using luciole::ColumnPreviewEditBar;
  using luciole::ColumnPreviewView;

  struct Rig {
    test::MetricsRenderer renderer;
    std::unique_ptr<ColumnPreviewView> view = std::make_unique<ColumnPreviewView>();
    InputDispatcher dispatcher;
    std::uint32_t serial = 0;
    int enables = 0;
    int saves = 0;
    int stops = 0;

    Rig() {
      dispatcher.setSceneRoot(view.get());
      view->setFile("notes.txt", "Plain text document");
      view->onEnableEditing = [this] { ++enables; };
      view->onSave = [this] { ++saves; };
      view->onStopEditing = [this] { ++stops; };
      layout();
    }

    void layout() { view->arrange(renderer, {0, 0, 360, 480}); }

    void click(const Node& node) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&node, x, y);
      x += node.width() / 2;
      y += node.height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 0, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 0, false);
      layout();
    }

    void set(const ColumnPreviewEditBar& bar) {
      view->setEditBar(bar);
      layout();
    }
  };

  std::string tooltipOf(const Button& button) {
    const auto content = button.inputArea()->tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  bool shown(const Node& node) {
    // Bounded by the depth of the scene.
    for (const Node* at = &node; at != nullptr; at = at->parent()) {
      if (!at->visible()) {
        return false;
      }
    }
    return node.width() > 0 && node.height() > 0;
  }

  // The unsaved mark is seen: shown, and not the transparent one that only keeps its place.
  bool marked(const Label& label) { return shown(label) && label.opacity() > 0.0F; }

  void hiddenUntilSet() {
    Rig rig;
    TEST_CHECK(!rig.view->editBar().shown);
    TEST_CHECK(!shown(rig.view->enableEditingButton()));
    TEST_CHECK(!shown(rig.view->saveButton()) && !shown(rig.view->stopEditingButton()));
    TEST_CHECK(!shown(rig.view->unsavedLabel()) && !shown(rig.view->noticeLabel()));
  }

  void enableEditingOffered() {
    Rig rig;
    const float before = rig.view->headerHeight();
    rig.set({.shown = true, .enableAvailable = true});
    TEST_CHECK(shown(rig.view->enableEditingButton()));
    TEST_CHECK(rig.view->enableEditingButton().enabled());
    TEST_CHECK(rig.view->enableEditingButton().label()->text() == "Enable editing");
    TEST_CHECK(!shown(rig.view->saveButton()) && !shown(rig.view->stopEditingButton()));
    TEST_CHECK(rig.view->headerHeight() > before); // the bar lies under the header, above the content
    rig.click(rig.view->enableEditingButton());
    TEST_CHECK(rig.enables == 1);
  }

  void disabledWithItsReason() {
    Rig rig;
    const std::string reason = "This file is read-only.";
    rig.set({.shown = true, .enableAvailable = false, .reason = reason});
    TEST_CHECK(shown(rig.view->enableEditingButton()));
    TEST_CHECK(!rig.view->enableEditingButton().enabled());
    TEST_CHECK(tooltipOf(rig.view->enableEditingButton()) == reason);
    rig.click(rig.view->enableEditingButton());
    TEST_CHECK(rig.enables == 0);
    rig.set({.shown = true, .enableAvailable = true});
    TEST_CHECK(tooltipOf(rig.view->enableEditingButton()) != reason);
  }

  void editingShowsSaveStopAndUnsaved() {
    Rig rig;
    rig.set({.shown = true, .editing = true});
    TEST_CHECK(!shown(rig.view->enableEditingButton()));
    TEST_CHECK(shown(rig.view->saveButton()) && shown(rig.view->stopEditingButton()));
    TEST_CHECK(rig.view->saveButton().label()->text() == "Save");
    TEST_CHECK(rig.view->stopEditingButton().label()->text() == "Stop editing");
    TEST_CHECK(!rig.view->saveButton().enabled());
    TEST_CHECK(!marked(rig.view->unsavedLabel()));
    rig.click(rig.view->saveButton());
    TEST_CHECK(rig.saves == 0); // nothing to save
    rig.set({.shown = true, .editing = true, .saveAvailable = true, .unsaved = true});
    TEST_CHECK(rig.view->saveButton().enabled());
    TEST_CHECK(marked(rig.view->unsavedLabel()) && rig.view->unsavedLabel().text() == "Unsaved changes");
    rig.click(rig.view->saveButton());
    TEST_CHECK(rig.saves == 1);
    rig.click(rig.view->stopEditingButton());
    TEST_CHECK(rig.stops == 1);
  }

  void noticeUnderTheButtons() {
    Rig rig;
    const std::string notice = "This file mixes line endings. Saving writes CRLF, its most common one, throughout.";
    rig.set({.shown = true, .editing = true, .notice = notice});
    TEST_CHECK(shown(rig.view->noticeLabel()) && rig.view->noticeLabel().text() == notice);
    float buttonX = 0;
    float buttonY = 0;
    Node::absolutePosition(&rig.view->saveButton(), buttonX, buttonY);
    float noticeX = 0;
    float noticeY = 0;
    Node::absolutePosition(&rig.view->noticeLabel(), noticeX, noticeY);
    TEST_CHECK(noticeY >= buttonY + rig.view->saveButton().height());
    rig.set({.shown = true, .editing = true});
    TEST_CHECK(!shown(rig.view->noticeLabel()));
    rig.set({});
    TEST_CHECK(!shown(rig.view->saveButton()) && !shown(rig.view->enableEditingButton()));
  }

  // The bar keeps the preview's one text size, which clear() leaves alone with the bar.
  void oneTextSize() {
    Rig rig;
    rig.set({.shown = true, .editing = true, .saveAvailable = true, .unsaved = true, .notice = "A sentence."});
    rig.view->setTextSize(9.5F);
    rig.layout();
    for (const Button* button : {&rig.view->saveButton(), &rig.view->stopEditingButton()}) {
      TEST_NEAR(button->label()->fontSize(), 9.5F, 0.0F);
      TEST_NEAR(button->height(), std::round(9.5F * 2), 0.5F);
    }
    TEST_NEAR(rig.view->enableEditingButton().label()->fontSize(), 9.5F, 0.0F);
    TEST_NEAR(rig.view->unsavedLabel().fontSize(), 9.5F, 0.0F);
    TEST_NEAR(rig.view->noticeLabel().fontSize(), 9.5F, 0.0F);
    rig.view->clear();
    rig.layout();
    TEST_CHECK(rig.view->editBar().shown && shown(rig.view->saveButton()));
  }

  // The words beside "Stop editing" are never cut, at any width a column can have: they wrap under the buttons when
  // the row is too narrow.
  void unsavedNeverCut() {
    Rig rig;
    rig.set({.shown = true, .editing = true, .saveAvailable = true, .unsaved = true});
    const auto& label = rig.view->unsavedLabel();
    bool wrapped = false;
    bool beside = false;
    for (float width = 140; width <= 700; width += 5) {
      rig.view->arrange(rig.renderer, {0, 0, width, 480});
      const float whole =
          rig.renderer
              .measureText(
                  label.text(), label.fontSize(), label.font(), 0, 1, TextAlign::Start, TextEllipsize::End, false
              )
              .width;
      TEST_CHECK(label.width() + 0.5F >= whole);
      float labelX = 0;
      float labelY = 0;
      Node::absolutePosition(&label, labelX, labelY);
      TEST_CHECK(labelX + label.width() <= width + 0.5F);
      float stopX = 0;
      float stopY = 0;
      Node::absolutePosition(&rig.view->stopEditingButton(), stopX, stopY);
      TEST_CHECK(stopX + rig.view->stopEditingButton().width() <= width + 0.5F);
      (labelY >= stopY + rig.view->stopEditingButton().height() ? wrapped : beside) = true;
    }
    TEST_CHECK(wrapped && beside);
  }

  struct Spots {
    float contentY = 0;
    float saveY = 0;
    float stopY = 0;
    float contentHeight = 0;
    bool operator==(const Spots&) const = default;
  };

  Spots spotsOf(Rig& rig) {
    Spots spots;
    float x = 0;
    Node::absolutePosition(rig.view->content(), x, spots.contentY);
    Node::absolutePosition(&rig.view->saveButton(), x, spots.saveY);
    Node::absolutePosition(&rig.view->stopEditingButton(), x, spots.stopY);
    spots.contentHeight = rig.view->content()->height();
    return spots;
  }

  // While a file is edited, nothing below the header moves when the unsaved mark appears or goes, at any width: the
  // mark's place is kept, beside the buttons or on the line under them.
  void unsavedMarkMovesNothing() {
    Rig rig;
    rig.view->setContent(std::make_unique<InputArea>());
    bool wrapped = false;
    bool beside = false;
    for (float width = 140; width <= 700; width += 10) {
      rig.view->arrange(rig.renderer, {0, 0, width, 480});
      rig.set({.shown = true, .editing = true, .saveAvailable = true, .unsaved = false});
      rig.view->arrange(rig.renderer, {0, 0, width, 480});
      const auto clean = spotsOf(rig);
      rig.set({.shown = true, .editing = true, .saveAvailable = true, .unsaved = true});
      rig.view->arrange(rig.renderer, {0, 0, width, 480});
      TEST_CHECK(marked(rig.view->unsavedLabel()));
      TEST_CHECK(spotsOf(rig) == clean);
      float labelX = 0;
      float labelY = 0;
      Node::absolutePosition(&rig.view->unsavedLabel(), labelX, labelY);
      (labelY >= clean.stopY + rig.view->stopEditingButton().height() ? wrapped : beside) = true;
      rig.set({.shown = true, .editing = true, .saveAvailable = true, .unsaved = false});
      rig.view->arrange(rig.renderer, {0, 0, width, 480});
      TEST_CHECK(spotsOf(rig) == clean && !marked(rig.view->unsavedLabel()));
    }
    TEST_CHECK(wrapped && beside);
    // Without editing there is no mark to reserve a place for.
    rig.set({.shown = true, .enableAvailable = true});
    TEST_CHECK(!marked(rig.view->unsavedLabel()) && !shown(rig.view->unsavedLabel()));
    rig.view->setContent(nullptr);
  }

  // While the owner keeps the keyboard, a click on anything of the view that is not a text field of its own leaves
  // it where it was, whether the target is a button, a dimmed button, a label or the padding between them.
  void clicksKeepTheKeyboard(bool keeps) {
    Rig rig;
    auto field = std::make_unique<InputArea>();
    field->setFocusable(true);
    auto* area = field.get();
    rig.view->setContent(std::move(field));
    rig.view->setSizeAvailable(true);
    rig.view->setKeepsKeyboard(keeps);
    TEST_CHECK(rig.view->keepsKeyboard() == keeps);
    rig.set(
        {.shown = true,
         .editing = true,
         .saveAvailable = false,
         .unsaved = true,
         .notice = "This file mixes line endings."}
    );
    const std::initializer_list<const Node*> targets{
        &rig.view->smallerButton(), &rig.view->sizeButton(),        &rig.view->largerButton(), &rig.view->openButton(),
        &rig.view->saveButton(),    &rig.view->stopEditingButton(), &rig.view->nameLabel(),    &rig.view->kindLabel(),
        &rig.view->unsavedLabel(),  &rig.view->noticeLabel(),
    };
    for (const Node* target : targets) {
      rig.dispatcher.setFocus(area);
      TEST_CHECK(rig.dispatcher.focusedArea() == area);
      rig.click(*target);
      TEST_CHECK((rig.dispatcher.focusedArea() == area) == keeps);
    }
    // The padding at the view's top left corner.
    rig.dispatcher.setFocus(area);
    rig.dispatcher.pointerEnter(1, 1, ++rig.serial);
    rig.dispatcher.pointerButton(1, 1, BTN_LEFT, true, ++rig.serial, 0, false);
    rig.dispatcher.pointerButton(1, 1, BTN_LEFT, false, ++rig.serial, 0, false);
    TEST_CHECK((rig.dispatcher.focusedArea() == area) == keeps);
    // The buttons still work.
    const int stops = rig.stops;
    rig.click(rig.view->stopEditingButton());
    TEST_CHECK(rig.stops == stops + 1);
    const int size = rig.view->previewSize();
    rig.click(rig.view->smallerButton());
    TEST_CHECK(rig.view->previewSize() < size);
    rig.view->setContent(nullptr);
  }
} // namespace

int main() {
  hiddenUntilSet();
  enableEditingOffered();
  disabledWithItsReason();
  editingShowsSaveStopAndUnsaved();
  noticeUnderTheButtons();
  oneTextSize();
  unsavedNeverCut();
  unsavedMarkMovesNothing();
  clicksKeepTheKeyboard(true);
  clicksKeepTheKeyboard(false);
  std::println("column-preview-edit-bar: all checks passed");
  return 0;
}
