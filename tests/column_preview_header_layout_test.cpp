#include "column_preview_view_rig.h"
#include "render/scene/input_area.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/list_rows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The column preview's header at every width: its parts stay whole, inside it and apart, and the reset step gives way.
namespace {
  using luciole::ColumnPreviewEditBar;
  using luciole::ColumnPreviewView;
  using test::preview_view::Rig;

  struct Part {
    const char* what;
    const Node* node;
    bool text; // a label, whose room is checked against its ellipsized form
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
  };

  bool shownInView(const Node& node, const Node& view) {
    for (const Node* at = &node; at != nullptr; at = at->parent()) {
      if (!at->visible() || !at->participatesInLayout()) {
        return false;
      }
      if (at == &view) {
        return true;
      }
    }
    return false;
  }

  bool within(const Node* node, const Node* ancestor) {
    for (; node != nullptr; node = node->parent()) {
      if (node == ancestor) {
        return true;
      }
    }
    return false;
  }

  // Every part the header lays out, the ones it shows with their place as laid out.
  std::vector<Part> shownParts(const ColumnPreviewView& view) {
    std::vector<Part> parts;
    const std::array<Part, 14> all{{
        {"name", &view.nameLabel(), true},
        {"smaller", &view.smallerButton(), false},
        {"size", &view.sizeButton(), false},
        {"larger", &view.largerButton(), false},
        {"open", &view.openButton(), false},
        {"kind", &view.kindLabel(), true},
        {"facts", &view.factsLabel(), true},
        {"enable", &view.enableEditingButton(), false},
        {"save", &view.saveButton(), false},
        {"stop", &view.stopEditingButton(), false},
        {"unsaved", &view.unsavedLabel(), true},
        {"notice", &view.noticeLabel(), true},
    }};
    for (auto part : all) {
      if (part.node == nullptr || !shownInView(*part.node, view)) {
        continue;
      }
      Node::absolutePosition(part.node, part.x, part.y);
      part.width = part.node->width();
      part.height = part.node->height();
      parts.push_back(part);
    }
    return parts;
  }

  bool overlap(const Part& a, const Part& b) {
    constexpr float kEps = 0.01F;
    return a.x < b.x + b.width - kEps
        && b.x < a.x + a.width - kEps
        && a.y < b.y + b.height - kEps
        && b.y < a.y + a.height - kEps;
  }

  struct Scenario {
    const char* what;
    bool media;
    ColumnPreviewEditBar bar;
  };
  const std::array<Scenario, 5> kScenarios{{
      {"file", false, {.shown = true, .enableAvailable = true}},
      {"media", true, {}},
      {"editing clean", false, {.shown = true, .editing = true}},
      {"editing unsaved", false, {.shown = true, .editing = true, .saveAvailable = true, .unsaved = true}},
      {"editing noticed",
       false,
       {.shown = true, .editing = true, .saveAvailable = true, .unsaved = true, .notice = "Saved to disk."}},
  }};

  // The part under the pointer at x, y is the control or something in it.
  bool takesPointer(ColumnPreviewView& view, const Node& control) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&control, x, y);
    return within(Node::hitTest(&view, x + (control.width() / 2), y + (control.height() / 2)), &control);
  }

  // Nothing of the header overlaps, is cut by the view's edge or by another part, and Open, Save and Stop are whole
  // and take the pointer, at every width from the narrowest column up, at the preview sizes the list sizes 12 to 16
  // give, for a short and a long name.
  void headerFitsEveryWidth() {
    constexpr float kHeight = 600;
    const std::array<std::string, 2> names{"a.txt", "recording of the weekly meeting 2026-11-18.final.wav"};
    for (const auto& scenario : kScenarios) {
      for (const std::string& name : names) {
        for (float list = 12; list <= 16; list += 1) {
          Rig rig;
          rig.height = kHeight;
          rig.view->setTextSize(luciole::quietTextSize(list, 0));
          rig.view->setFile(name, "Plain text document");
          rig.view->setFacts("1.0 KiB · 2026-11-18 09:30");
          rig.view->setSizeAvailable(!scenario.media, "Video and sound are always fitted to the pane.");
          rig.view->setEditBar(scenario.bar);
          std::vector<std::vector<Part>> seen;
          for (float width = 140; width <= 400; width += 5) {
            rig.width = width;
            rig.layout();
            const auto context = std::format("{} '{}' list {} width {}", scenario.what, name.substr(0, 6), list, width);
            const auto parts = shownParts(*rig.view);
            const float header = rig.view->headerHeight();
            for (const auto& part : parts) {
              const bool inside = part.x >= -0.01F
                  && part.y >= -0.01F
                  && part.x + part.width <= width + 0.01F
                  && part.y + part.height <= header + 0.01F;
              if (!inside) {
                std::println(
                    "{}: {} at {},{} {}x{} leaves the header {}x{}", context, part.what, part.x, part.y, part.width,
                    part.height, width, header
                );
              }
              TEST_CHECK(inside);
            }
            for (std::size_t i = 0; i < parts.size(); ++i) {
              for (std::size_t j = i + 1; j < parts.size(); ++j) {
                if (overlap(parts[i], parts[j])) {
                  std::println("{}: {} overlaps {}", context, parts[i].what, parts[j].what);
                }
                TEST_CHECK(!overlap(parts[i], parts[j]));
              }
            }
            // Whole: a button as wide as it measures, a label with the room of its ellipsized form.
            for (const auto& part : parts) {
              auto& node = const_cast<Node&>(*part.node);
              const float natural = node.measure(rig.renderer, LayoutConstraints::unconstrained()).width;
              const float least = part.text ? std::min(natural, 5 * rig.view->textSize()) : natural;
              if (part.width + 0.01F < least) {
                std::println("{}: {} is {} wide, needs {}", context, part.what, part.width, least);
              }
              TEST_CHECK(part.width + 0.01F >= least);
            }
            for (const Button* control :
                 {&rig.view->openButton(), &rig.view->saveButton(), &rig.view->stopEditingButton()}) {
              if (shownInView(*control, *rig.view)) {
                TEST_CHECK(takesPointer(*rig.view, *control));
              }
            }
            seen.push_back(parts);
          }
          // Coming down from the widest gives the same places as going up: no width depends on the one before.
          std::size_t at = seen.size();
          for (float width = 400; width >= 140; width -= 5) {
            rig.width = width;
            rig.layout();
            const auto parts = shownParts(*rig.view);
            const auto& before = seen.at(--at);
            TEST_CHECK(parts.size() == before.size());
            for (std::size_t i = 0; i < std::min(parts.size(), before.size()); ++i) {
              const bool same = parts[i].node == before[i].node
                  && std::abs(parts[i].x - before[i].x) < 0.01F
                  && std::abs(parts[i].y - before[i].y) < 0.01F
                  && std::abs(parts[i].width - before[i].width) < 0.01F
                  && std::abs(parts[i].height - before[i].height) < 0.01F;
              if (!same) {
                std::println("{} width {}: {} is placed by what came before", scenario.what, width, parts[i].what);
              }
              TEST_CHECK(same);
            }
          }
        }
      }
    }
  }

  // Below the narrowest column the controls alone can fill a line: the step that resets the size gives way, takes no
  // pointer, and returns with the room.
  void resetStepGivesWay() {
    Rig rig;
    rig.view->setTextSize(16);
    rig.view->setFile("notes.txt", "Plain text document");
    rig.view->setSizeAvailable(true);
    rig.width = 100;
    rig.layout();
    TEST_CHECK(!shownInView(rig.view->sizeButton(), *rig.view));
    TEST_CHECK(shownInView(rig.view->smallerButton(), *rig.view) && shownInView(rig.view->largerButton(), *rig.view));
    const auto parts = shownParts(*rig.view);
    for (const auto& part : parts) {
      TEST_CHECK(part.x >= 0 && part.x + part.width <= rig.width + 0.01F);
    }
    for (float x = 0; x < rig.width; x += 2) {
      for (float y = 0; y < rig.view->headerHeight(); y += 2) {
        TEST_CHECK(!within(Node::hitTest(rig.view.get(), x, y), &rig.view->sizeButton()));
      }
    }
    rig.width = 360;
    rig.layout();
    TEST_CHECK(shownInView(rig.view->sizeButton(), *rig.view) && takesPointer(*rig.view, rig.view->sizeButton()));
  }

  // Laid out at the width it measures with no bound, the header is one line, whatever the length of the name.
  void naturalWidthIsOneLine() {
    const std::array<std::string, 2> names{"a.txt", "recording of the weekly meeting 2026-11-18.final.wav"};
    for (const std::string& name : names) {
      for (const bool sizes : {true, false}) {
        Rig rig;
        rig.view->setTextSize(14);
        rig.view->setFile(name, "Plain text document");
        rig.view->setSizeAvailable(sizes);
        rig.layout();
        Node* row = rig.view->openButton().parent();
        const auto natural = row->measure(rig.renderer, LayoutConstraints::unconstrained());
        const float line = rig.view->openButton().measure(rig.renderer, LayoutConstraints::unconstrained()).height;
        LayoutConstraints bound;
        bound.setMaxWidth(natural.width);
        const auto at = row->measure(rig.renderer, bound);
        std::println(
            "'{}': natural {}x{}, at that width {}, one line {}", name.substr(0, 6), natural.width, natural.height,
            at.height, line
        );
        TEST_NEAR(at.height, line, 0.01F);
        TEST_NEAR(natural.height, line, 0.01F);
      }
    }
  }

  // The reset step is hidden at a narrow width: it takes no key either, whether or not it had the keyboard.
  void hiddenResetStepTakesNoKey() {
    Rig rig;
    std::vector<int> chosen;
    rig.view->onPreviewSizeChosen = [&chosen](int percent) { chosen.push_back(percent); };
    rig.view->setTextSize(16);
    rig.view->setFile("notes.txt", "Plain text document");
    rig.view->setSizeAvailable(true);
    rig.view->setPreviewSize(150);
    rig.layout();
    InputArea* reset = rig.view->sizeButton().inputArea();
    rig.dispatcher.setFocus(reset);
    TEST_CHECK(rig.dispatcher.focusedArea() == reset);
    rig.width = 100;
    rig.layout();
    TEST_CHECK(!shownInView(rig.view->sizeButton(), *rig.view));
    rig.dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true, false);
    rig.dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false, false);
    TEST_CHECK(chosen.empty() && rig.view->previewSize() == 150);
    TEST_CHECK(rig.dispatcher.focusedArea() != reset);
    // With the room back, the step is there and works again.
    rig.width = 360;
    rig.layout();
    rig.click(rig.view->sizeButton());
    TEST_CHECK(chosen == std::vector{100});
  }
} // namespace

int main() {
  headerFitsEveryWidth();
  resetStepGivesWay();
  naturalWidthIsOneLine();
  hiddenResetStepTakesNoKey();
  std::println("column-preview-header-layout: all checks passed");
  return 0;
}
