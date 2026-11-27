#include "column_preview_view_rig.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/list_rows.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <vector>

namespace {
  using luciole::ColumnPreviewEditBar;
  using luciole::ColumnPreviewView;
  using test::preview_view::Rig;

  // A content node that says when it goes.
  class Probe final : public Node {
  public:
    explicit Probe(int& gone) : m_gone(gone) {}
    ~Probe() override { ++m_gone; }
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

  private:
    int& m_gone;
  };

  struct Point {
    float x = 0;
    float y = 0;
  };

  Point center(const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return {.x = x + (node.width() / 2), .y = y + (node.height() / 2)};
  }

  // The content may name its kind better than the listing does; the next file starts from the listing's again.
  void contentNamesItsKind() {
    Rig rig;
    rig.view->setFile("cube.ply", "Plain text document");
    rig.view->setKind("3D model");
    rig.layout();
    TEST_CHECK(rig.view->kindLabel().text() == "3D model");
    TEST_CHECK(rig.view->nameLabel().text() == "cube.ply");
    rig.view->setFile("notes.txt", "Plain text document");
    TEST_CHECK(rig.view->kindLabel().text() == "Plain text document");
    rig.view->setKind("3D model");
    rig.view->clear();
    TEST_CHECK(rig.view->kindLabel().text().empty());
  }

  void showsTheFile() {
    Rig rig;
    rig.view->setFile("notes.txt", "Plain text document");
    rig.view->setFacts("1.0 KiB · 2026-11-18 09:30");
    rig.layout();
    TEST_CHECK(rig.view->nameLabel().text() == "notes.txt");
    TEST_CHECK(rig.view->kindLabel().text() == "Plain text document");
    TEST_CHECK(rig.view->factsLabel().text() == "1.0 KiB · 2026-11-18 09:30");
    TEST_CHECK(rig.view->factsLabel().visible());
    rig.view->setFacts("");
    TEST_CHECK(!rig.view->factsLabel().visible());
    TEST_CHECK(rig.view->content() == nullptr && rig.view->message().empty());
  }

  // Every text in the view takes the one size it is given.
  void oneTextSize() {
    Rig rig;
    rig.view->setFile("a.csv", "CSV document");
    rig.view->setFacts("3 rows");
    rig.view->showMessage("Loading…");
    for (const float size : {12.5F, 9.0F}) {
      rig.view->setTextSize(size);
      rig.layout();
      TEST_NEAR(rig.view->textSize(), size, 0.0F);
      for (const Label* label : {&rig.view->nameLabel(), &rig.view->kindLabel(), &rig.view->factsLabel()}) {
        TEST_NEAR(label->fontSize(), size, 0.0F);
      }
      TEST_NEAR(rig.view->messageLabel().fontSize(), size, 0.0F);
      TEST_NEAR(rig.view->openButton().label()->fontSize(), size, 0.0F);
    }
    for (const float bad : {0.0F, -3.0F, std::nanf("")}) { // ignored
      rig.view->setTextSize(bad);
      TEST_NEAR(rig.view->textSize(), 9.0F, 0.0F);
    }
  }

  // One content at a time: a new one, a message and clear() each free the one before.
  void holdsOneContent() {
    Rig rig;
    int gone = 0;
    rig.view->setContent(std::make_unique<Probe>(gone));
    auto* first = rig.view->content();
    TEST_CHECK(first != nullptr);
    rig.layout();
    // The content fills the width and the rest of the height below the header.
    TEST_NEAR(first->width(), rig.width, 0.5F);
    float x = 0;
    float y = 0;
    Node::absolutePosition(first, x, y);
    TEST_CHECK(y > 0);
    TEST_NEAR(y + first->height(), rig.height, 0.5F);

    rig.view->setContent(std::make_unique<Probe>(gone));
    TEST_CHECK(gone == 1 && rig.view->content() != first);
    rig.view->showMessage("This file could not be read.");
    TEST_CHECK(gone == 2 && rig.view->content() == nullptr);
    TEST_CHECK(rig.view->message() == "This file could not be read.");
    rig.layout();
    TEST_CHECK(rig.view->messageLabel().parent()->visible());
    rig.view->setContent(std::make_unique<Probe>(gone));
    TEST_CHECK(rig.view->message().empty() && !rig.view->messageLabel().parent()->visible());
    rig.view->setFile("b.txt", "Plain text document");
    rig.view->clear();
    TEST_CHECK(gone == 3 && rig.view->content() == nullptr);
    TEST_CHECK(rig.view->nameLabel().text().empty() && rig.view->kindLabel().text().empty());
    rig.view->setContent(std::make_unique<Probe>(gone));
    rig.view.reset();
    TEST_CHECK(gone == 4);
  }

  void openButtonOpens() {
    Rig rig;
    int opened = 0;
    rig.view->onOpen = [&opened] { ++opened; };
    rig.view->setFile("notes.txt", "Plain text document");
    rig.layout();
    TEST_CHECK(rig.view->openButton().width() > 0);
    rig.click(rig.view->openButton());
    TEST_CHECK(opened == 1);
    rig.view->clear(); // nothing to open
    rig.layout();
    rig.click(rig.view->openButton());
    TEST_CHECK(opened == 1);
  }

  // The size controls step along the preview sizes, show the step, return to 100% and stop at the ends; the header's
  // text keeps its size whatever the step.
  void sizeControls() {
    Rig rig;
    std::vector<int> chosen;
    rig.view->onPreviewSizeChosen = [&chosen](int percent) { chosen.push_back(percent); };
    rig.view->setFile("notes.txt", "Plain text document");
    rig.view->setTextSize(11);
    rig.view->setSizeAvailable(true);
    rig.layout();
    TEST_CHECK(rig.view->sizeButton().label()->text() == "100%");
    for (const Button* button : {&rig.view->smallerButton(), &rig.view->sizeButton(), &rig.view->largerButton()}) {
      TEST_CHECK(button->enabled() && button->width() > 0);
      TEST_NEAR(button->height(), 22, 0.0F); // twice the header's text size, like Open
    }
    TEST_NEAR(rig.view->sizeButton().label()->fontSize(), 11, 0.0F);
    // In the header's row, between the name and Open.
    TEST_NEAR(center(rig.view->smallerButton()).y, center(rig.view->openButton()).y, 0.5F);
    TEST_CHECK(center(rig.view->nameLabel()).x < center(rig.view->smallerButton()).x);
    TEST_CHECK(center(rig.view->largerButton()).x < center(rig.view->openButton()).x);

    rig.click(rig.view->largerButton());
    rig.click(rig.view->largerButton());
    TEST_CHECK((chosen == std::vector{110, 125}) && rig.view->previewSize() == 125);
    TEST_CHECK(rig.view->sizeButton().label()->text() == "125%");
    rig.click(rig.view->sizeButton());
    TEST_CHECK(chosen.back() == 100 && rig.view->sizeButton().label()->text() == "100%");
    for (int i = 0; i < 4; ++i) {
      rig.click(rig.view->smallerButton());
    }
    // 90, 80, 70, then nothing: the smaller control is disabled at the end.
    TEST_CHECK((chosen == std::vector{110, 125, 100, 90, 80, 70}));
    TEST_CHECK(!rig.view->smallerButton().enabled() && rig.view->largerButton().enabled());
    rig.view->setPreviewSize(300);
    rig.layout();
    TEST_CHECK(rig.view->smallerButton().enabled() && !rig.view->largerButton().enabled());
    rig.click(rig.view->largerButton());
    TEST_CHECK(chosen.size() == 6 && rig.view->previewSize() == 300);
    // Setting a size tells nobody; a size off the steps shows as the default.
    rig.view->setPreviewSize(95);
    TEST_CHECK(rig.view->previewSize() == 100 && chosen.size() == 6);
    // The header's own text does not follow the step.
    for (const Label* label : {&rig.view->nameLabel(), &rig.view->kindLabel()}) {
      TEST_NEAR(label->fontSize(), 11, 0.0F);
    }

    // Unavailable for a reason: every control is disabled, says why, and a click chooses nothing.
    rig.view->setSizeAvailable(false, "Video and sound are always fitted to the pane.");
    rig.layout();
    for (const Button* button : {&rig.view->smallerButton(), &rig.view->sizeButton(), &rig.view->largerButton()}) {
      TEST_CHECK(!button->enabled());
      TEST_CHECK(button->inputArea()->enabled() && button->inputArea()->hasTooltip());
    }
    rig.click(rig.view->smallerButton());
    rig.click(rig.view->sizeButton());
    TEST_CHECK(chosen.size() == 6 && rig.view->previewSize() == 100);
    // Available again at an end of the range: the control disabled there takes no pointer and says nothing.
    rig.view->setPreviewSize(300);
    rig.view->setSizeAvailable(true);
    TEST_CHECK(!rig.view->largerButton().inputArea()->enabled());
    TEST_CHECK(rig.view->smallerButton().inputArea()->enabled() && rig.view->sizeButton().inputArea()->enabled());
    // No file: nothing to size.
    rig.view->setSizeAvailable(true);
    rig.view->clear();
    TEST_CHECK(!rig.view->sizeAvailable() && !rig.view->largerButton().enabled());
  }

  // Ctrl with the wheel over the content steps the size, a notch a step, down smaller; elsewhere, without content or
  // with the controls disabled, it is left alone.
  void wheelSteps() {
    Rig rig;
    int gone = 0;
    std::vector<int> chosen;
    rig.view->onPreviewSizeChosen = [&chosen](int percent) { chosen.push_back(percent); };
    rig.view->setFile("notes.txt", "Plain text document");
    rig.view->setSizeAvailable(true);
    rig.layout();
    TEST_CHECK(!rig.view->wheelSize(180, 400, 1)); // no content yet
    rig.view->setContent(std::make_unique<Probe>(gone));
    rig.layout();
    const auto inside = center(*rig.view->content());
    TEST_NEAR(rig.view->headerHeight(), inside.y - (rig.view->content()->height() / 2), 0.5F);
    TEST_CHECK(rig.view->wheelSize(inside.x, inside.y, -1));
    TEST_CHECK(rig.view->wheelSize(inside.x, inside.y, -2));
    TEST_CHECK(rig.view->wheelSize(inside.x, inside.y, 1));
    TEST_CHECK((chosen == std::vector{110, 150, 125}));
    // Half notches add up to a step.
    TEST_CHECK(rig.view->wheelSize(inside.x, inside.y, 0.5) && chosen.size() == 3);
    TEST_CHECK(rig.view->wheelSize(inside.x, inside.y, 0.5) && chosen.back() == 110);
    // Over the header, or with the controls disabled, nothing happens.
    TEST_CHECK(!rig.view->wheelSize(inside.x, rig.view->headerHeight() / 2, 1));
    rig.view->setSizeAvailable(false, "This preview is shown at one size.");
    TEST_CHECK(!rig.view->wheelSize(inside.x, inside.y, 1) && chosen.size() == 4);
  }

  // Measured with no bound, the view is as tall as its header, facts and content, not collapsed to its header.
  void measuresWithoutBound() {
    Rig rig;
    rig.view->setFile("notes.txt", "Plain text document");
    rig.view->setFacts("3 lines");
    const auto header = rig.view->measure(rig.renderer, LayoutConstraints::unconstrained());
    int gone = 0;
    auto content = std::make_unique<Probe>(gone);
    content->setSize(120, 200);
    rig.view->setContent(std::move(content));
    const auto full = rig.view->measure(rig.renderer, LayoutConstraints::unconstrained());
    TEST_CHECK(full.height >= header.height + 199);
    TEST_CHECK(full.width >= 120);
  }

  // A status shows as the side preview's badge beside the kind, in the view's one text size; a problem is red; the
  // next file and clear() drop it, and a view without a status shows none.
  void statusBadge() {
    Palette colors{};
    colors.error = Color{1.0F, 0.2F, 0.2F, 1.0F};
    colors.onSurfaceVariant = Color{0.6F, 0.6F, 0.7F, 1.0F};
    setPalette(colors);
    Rig rig;
    rig.view->setFile("notes.txt", "Plain text document");
    rig.layout();
    TEST_CHECK(!rig.view->statusBadge().pill->visible() && rig.view->statusBadge().label->text().empty());
    rig.view->setStatus("Truncated", false);
    rig.layout();
    const auto& badge = rig.view->statusBadge();
    TEST_CHECK(badge.pill->visible() && badge.label->text() == "Truncated");
    TEST_CHECK(badge.label->fontSize() == rig.view->textSize());
    TEST_CHECK(badge.pill->height() >= rig.view->textSize() && badge.pill->height() <= rig.view->textSize() * 2);
    // Beside the kind, on its line, at its right end.
    float kindX = 0, kindY = 0, badgeX = 0, badgeY = 0;
    Node::absolutePosition(&rig.view->kindLabel(), kindX, kindY);
    Node::absolutePosition(badge.pill, badgeX, badgeY);
    TEST_CHECK(badgeX > kindX && badgeY < kindY + rig.view->kindLabel().height());
    TEST_CHECK(badgeX + badge.pill->width() <= rig.width + 0.01F);
    const auto quiet = badge.label->color();
    rig.view->setStatus("Read error", true);
    TEST_CHECK(badge.label->color() != quiet);
    rig.view->setFile("other.txt", "Plain text document");
    TEST_CHECK(!badge.pill->visible());
    rig.view->setStatus("Empty", false);
    rig.view->clear();
    TEST_CHECK(!badge.pill->visible());
    // A long kind gives way to the badge: both stay in view.
    rig.view->setFile("a.txt", std::string(200, 'k'));
    rig.view->setStatus("Not UTF-8 text", false);
    rig.layout();
    Node::absolutePosition(badge.pill, badgeX, badgeY);
    TEST_CHECK(badgeX + badge.pill->width() <= rig.width + 0.01F);
  }
} // namespace

int main() {
  statusBadge();
  showsTheFile();
  contentNamesItsKind();
  oneTextSize();
  holdsOneContent();
  openButtonOpens();
  sizeControls();
  wheelSteps();
  measuresWithoutBound();
  std::println("column-preview-view: all checks passed");
  return 0;
}
