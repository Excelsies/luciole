#include "model/directory_options.h"
#include "render/scene/glyph_node.h"
#include "render/text/glyph_registry.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_chrome.h"
#include "workbench/color.h"

#include <memory>
#include <print>

namespace {
  using namespace luciole;

  // The code point a glyph control draws.
  char32_t glyphOf(const Glyph& glyph) {
    for (const auto& child : glyph.children()) {
      if (const auto* node = dynamic_cast<const GlyphNode*>(child.get())) {
        return node->codepoint();
      }
    }
    return 0;
  }

  char32_t railGlyph(const PaneShell& shell) {
    const auto* button = shell.railButton(workbench::Pane::Instrument);
    TEST_CHECK(button != nullptr && button->glyph() != nullptr);
    return glyphOf(*button->glyph());
  }

  void railShowsTheChosenInstrument() {
    Node root;
    auto* shell = static_cast<PaneShell*>(root.addChild(std::make_unique<PaneShell>()));
    WorkbenchHeaders headers(*shell, [] {});
    TEST_CHECK(railGlyph(*shell) == GlyphRegistry::lookup("chart-treemap"));
    headers.show(WorkbenchInstrument::Iris, WorkbenchRibbon::Flame);
    TEST_CHECK(railGlyph(*shell) == GlyphRegistry::lookup("chart-donut"));
    headers.show(WorkbenchInstrument::Atlas, WorkbenchRibbon::Ladder);
    TEST_CHECK(railGlyph(*shell) == GlyphRegistry::lookup("chart-bubble"));
    headers.show(WorkbenchInstrument::Nest, WorkbenchRibbon::Flow);
    TEST_CHECK(railGlyph(*shell) == GlyphRegistry::lookup("chart-treemap"));
  }

  void shadowRoleBecomesTheSceneShadow() {
    Palette roles{};
    roles.shadow = rgba(0.2F, 0.4F, 0.6F, 1);
    roles.surface = rgba(1, 1, 1, 1);
    const auto palette = workbenchPalette(roles);
    TEST_CHECK((palette.shadow == workbench::Rgb{.r = 51, .g = 102, .b = 153}));
    TEST_CHECK((palette.surface == workbench::Rgb{.r = 255, .g = 255, .b = 255}));
  }
} // namespace

int main() {
  railShowsTheChosenInstrument();
  shadowRoleBecomesTheSceneShadow();
  std::println("workbench chrome checks passed");
  return 0;
}
