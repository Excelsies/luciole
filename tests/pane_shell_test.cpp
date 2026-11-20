#include "core/input/key_modifiers.h"
#include "metrics_renderer.h"
#include "render/scene/glyph_node.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/text/glyph_registry.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_divider.h"
#include "ui/controls/pane_header.h"
#include "ui/style.h"
#include "ui/workbench/pane_shell.h"
#include "workbench/pane_layout.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <print>
#include <string>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  std::size_t g_allocations = 0;
}

void* operator new(std::size_t size) {
  ++g_allocations;
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
  using luciole::PaneRegion;
  using luciole::PaneShell;
  using namespace luciole::workbench;

  const PartLimits kLim{
      .instrument = {.minWidth = 240, .minHeight = 220},
      .ribbon = {.minWidth = 360, .minHeight = 140, .preferredHeight = 200, .maxHeight = 360},
  };
  const PartLimits kWideRibbon{
      .instrument = {.minWidth = 240, .minHeight = 220},
      .ribbon = {.minWidth = 560, .minHeight = 180, .preferredHeight = 280, .maxHeight = 420},
  };

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

  // The code point a glyph control draws.
  char32_t glyphOf(const Glyph& glyph) {
    for (const auto& child : glyph.children()) {
      if (const auto* node = dynamic_cast<const GlyphNode*>(child.get())) {
        return node->codepoint();
      }
    }
    return 0;
  }

  std::string tooltipOf(const Button* button) {
    const auto content = button->inputArea()->tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  // A shell filling a scene, with one input area per region, the center's above like a list's grid.
  struct Rig {
    test::MetricsRenderer renderer;
    Node root;
    PaneShell* shell = nullptr;
    InputDispatcher dispatcher;
    std::array<InputArea*, 4> contents{};
    std::vector<PaneState> commits;
    int centerPresses = 0;
    std::uint32_t serial = 0;
    float width = 1400;
    float height = 700;

    explicit Rig(const PartLimits& limits = kLim) {
      root.setSize(4000, 4000);
      shell = static_cast<PaneShell*>(root.addChild(std::make_unique<PaneShell>()));
      for (std::size_t i = 0; i < contents.size(); ++i) {
        auto area = std::make_unique<InputArea>();
        contents[i] = area.get();
        TEST_CHECK(shell->setContent(static_cast<PaneRegion>(i), std::move(area)) == contents[i]);
      }
      contents[3]->setZIndex(50);
      contents[3]->setOnPress([this](const InputArea::PointerData& data) { centerPresses += data.pressed ? 1 : 0; });
      shell->setTitle(Pane::Instrument, "Instrument: Nest");
      shell->setTitle(Pane::Ribbon, "Ribbon: Flame graph");
      shell->setLimits(limits);
      shell->setOnCommit([this](const PaneState& state) { commits.push_back(state); });
      dispatcher.setSceneRoot(&root);
      layout();
    }

    void layout() { shell->arrange(renderer, {0, 0, width, height}); }
    void resize(float w, float h) {
      width = w;
      height = h;
      layout();
    }
    SceneRect content(PaneRegion region) const { return sceneBox(contents[static_cast<std::size_t>(region)]); }
    bool visible(PaneRegion region) const { return shown(contents[static_cast<std::size_t>(region)]); }

    void press(float x, float y, std::uint32_t time = 1000) {
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, time, false);
    }
    // One frame of a drag: the pointer moves, then the window lays out.
    void move(float x, float y) {
      dispatcher.pointerMotion(x, y, ++serial);
      layout();
    }
    void release(float x, float y, std::uint32_t time = 1100) {
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, time, false);
      layout();
    }
    void click(const Button* button) {
      TEST_CHECK(button != nullptr && shown(button));
      const auto box = sceneBox(button);
      press(box.x + box.width / 2, box.y + box.height / 2);
      release(box.x + box.width / 2, box.y + box.height / 2);
    }
    void key(std::uint32_t sym, std::uint32_t modifiers = 0) {
      dispatcher.keyEvent(sym, 0, modifiers, true);
      dispatcher.keyEvent(sym, 0, modifiers, false);
      layout();
    }
  };

  void defaultLayout() {
    Rig rig;
    const auto instrument = rig.content(PaneRegion::Instrument);
    TEST_CHECK(instrument.x == 0 && instrument.y == 24 && instrument.width == 340 && instrument.height == 410);
    const auto jump = rig.content(PaneRegion::Jump);
    TEST_CHECK(jump.x == 0 && jump.y == 458 && jump.width == 340 && jump.height == 242);
    const auto ribbon = rig.content(PaneRegion::Ribbon);
    TEST_CHECK(ribbon.x == 340 && ribbon.y == 24 && ribbon.width == 1060 && ribbon.height == 200);
    const auto center = rig.content(PaneRegion::Center);
    TEST_CHECK(center.x == 340 && center.y == 224 && center.width == 1060 && center.height == 476);
    const auto header = sceneBox(&rig.shell->header(Pane::Jump));
    TEST_CHECK(header.x == 0 && header.y == 434 && header.width == 340 && header.height == 24);
    const auto left = sceneBox(&rig.shell->divider(DividerId::Left));
    TEST_CHECK(left.x == 337 && left.y == 0 && left.width == 6 && left.height == 700);
    const auto ribbonDivider = sceneBox(&rig.shell->divider(DividerId::Ribbon));
    TEST_CHECK(ribbonDivider.x == 340 && ribbonDivider.y == 221 && ribbonDivider.width == 1060);
    TEST_CHECK(rig.shell->header(Pane::Instrument).titleLabel()->text() == "Instrument: Nest");
    TEST_CHECK(rig.shell->header(Pane::Jump).titleLabel()->text() == "Jump ahead");
    TEST_CHECK(rig.shell->content(PaneRegion::Center) == rig.contents[3]);
    TEST_CHECK(rig.commits.empty());
    rig.resize(1400.5F, 700.5F); // panes reach a fractional edge
    TEST_NEAR(rig.content(PaneRegion::Ribbon).x + rig.content(PaneRegion::Ribbon).width, 1400.5, 1e-3);
    TEST_NEAR(rig.content(PaneRegion::Center).y + rig.content(PaneRegion::Center).height, 700.5, 1e-3);
    TEST_NEAR(rig.content(PaneRegion::Jump).y + rig.content(PaneRegion::Jump).height, 700.5, 1e-3);
    TEST_CHECK(rig.content(PaneRegion::Instrument).width == 340);
  }

  void dragMovesTheLeftEdge() {
    Rig rig;
    rig.press(340, 300);
    TEST_CHECK(rig.shell->resizing());
    rig.move(400, 300);
    TEST_CHECK(rig.content(PaneRegion::Center).x == 400);
    TEST_CHECK(rig.content(PaneRegion::Instrument).width == 400);
    TEST_CHECK(rig.commits.empty()); // live while dragging, saved on release
    rig.move(520, 320);
    TEST_CHECK(rig.content(PaneRegion::Center).x == 520);
    rig.move(100, 320); // the instrument needs 240
    TEST_CHECK(rig.content(PaneRegion::Center).x == 240);
    rig.move(461, 320);
    const auto start = PaneState{};
    const auto expected =
        dragDivider(start, {.width = 1400, .height = 700}, kLim, RibbonKind::Flame, DividerId::Left, 340, 121);
    TEST_CHECK(expected.value == 461);
    TEST_CHECK(rig.content(PaneRegion::Center).x == 461);
    rig.release(461, 320);
    TEST_CHECK(rig.commits.size() == 1);
    TEST_CHECK(rig.commits[0] == expected.state);
    TEST_CHECK(rig.shell->state() == expected.state);
    TEST_CHECK(!rig.shell->resizing());
    // a left drag leaves every height alone
    TEST_CHECK(!rig.commits[0].instrumentHeight.has_value());
    TEST_CHECK(
        !rig.commits[0].ribbonHeights[0] && !rig.commits[0].ribbonHeights[1] && !rig.commits[0].ribbonHeights[2]
    );
    TEST_CHECK(rig.content(PaneRegion::Instrument).height == 410);
    TEST_CHECK(rig.content(PaneRegion::Ribbon).height == 200);
  }

  void dragMovesTheRibbonEdgeOnly() {
    Rig rig;
    rig.shell->setRibbonKind(RibbonKind::Ladder);
    rig.layout();
    rig.press(800, 224);
    rig.move(800, 300);
    TEST_CHECK(rig.content(PaneRegion::Center).y == 300);
    rig.move(800, 650);                                   // the center keeps 160
    TEST_CHECK(rig.content(PaneRegion::Center).y == 384); // the ribbon's maximum, 24 + 360, comes first
    rig.release(800, 650);
    TEST_CHECK(rig.commits.size() == 1);
    const auto& state = rig.commits[0];
    TEST_CHECK(state.ribbonHeights[2] == 384.0);
    TEST_CHECK(!state.ribbonHeights[0] && !state.ribbonHeights[1]); // heights are kept per ribbon kind
    TEST_CHECK(!state.leftWidth && !state.instrumentHeight);
    TEST_CHECK(rig.content(PaneRegion::Instrument).height == 410);
    TEST_CHECK(rig.content(PaneRegion::Center).x == 340);
  }

  void escapeAndLostCaptureRestore() {
    Rig rig;
    rig.press(800, 224);
    rig.move(800, 300);
    TEST_CHECK(rig.content(PaneRegion::Center).y == 300);
    rig.key(XKB_KEY_Escape);
    TEST_CHECK(rig.content(PaneRegion::Center).y == 224);
    TEST_CHECK(rig.shell->state() == PaneState{});
    TEST_CHECK(!rig.shell->resizing());
    rig.move(800, 350);
    rig.release(800, 350);
    TEST_CHECK(rig.content(PaneRegion::Center).y == 224);
    TEST_CHECK(rig.commits.empty());
    rig.press(340, 300);
    rig.move(420, 300);
    rig.dispatcher.cancelPointerCapture();
    rig.layout();
    TEST_CHECK(rig.content(PaneRegion::Center).x == 340);
    TEST_CHECK(rig.commits.empty());
  }

  void doublePressResets() {
    Rig rig;
    PaneState saved;
    saved.leftWidth = 500;
    rig.shell->setState(saved);
    rig.layout();
    TEST_CHECK(rig.content(PaneRegion::Center).x == 500);
    TEST_CHECK(rig.commits.empty()); // a state set by the owner is not reported back
    rig.press(500, 300, 1000);
    rig.release(500, 300, 1050);
    rig.press(501, 302, 1200);
    rig.release(501, 302, 1250);
    TEST_CHECK(rig.commits.size() == 1);
    TEST_CHECK(!rig.commits[0].leftWidth.has_value());
    TEST_CHECK(rig.content(PaneRegion::Center).x == 340);
  }

  void keysStep() {
    Rig rig;
    rig.dispatcher.setFocus(&rig.shell->divider(DividerId::Instrument));
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.content(PaneRegion::Jump).y == 450 + 24);
    rig.key(XKB_KEY_Up, KeyMod::Shift);
    TEST_CHECK(rig.content(PaneRegion::Jump).y == 386 + 24);
    TEST_CHECK(rig.commits.size() == 2);
    TEST_CHECK(rig.commits[1].instrumentHeight == 386.0);
    for (int i = 0; i < 40; ++i) { // the jump list keeps its 52 px body
      rig.key(XKB_KEY_Down, KeyMod::Shift);
    }
    TEST_CHECK(rig.content(PaneRegion::Jump).height == 52);
    rig.key(XKB_KEY_Up); // one step back from the limit moves at once
    TEST_CHECK(rig.content(PaneRegion::Jump).height == 68);
    rig.key(XKB_KEY_Home);
    TEST_CHECK(!rig.shell->state().instrumentHeight.has_value());
    TEST_CHECK(rig.content(PaneRegion::Jump).y == 458);
    // a divider the layout removes takes no keys, even with the focus
    rig.key(XKB_KEY_Down);
    auto expanded = rig.shell->state();
    expanded.expanded = true;
    rig.shell->setState(expanded);
    rig.layout();
    TEST_CHECK(rig.dispatcher.focusedArea() == &rig.shell->divider(DividerId::Instrument));
    const auto commits = rig.commits.size();
    rig.key(XKB_KEY_Home);
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.commits.size() == commits);
    TEST_CHECK(rig.shell->state() == expanded && expanded.instrumentHeight == 450.0);
  }

  void collapseAndRestore() {
    Rig rig;
    auto& instrument = rig.shell->header(Pane::Instrument);
    rig.click(instrument.collapseButton());
    TEST_CHECK(rig.commits.size() == 1 && rig.commits[0].collapsed.instrument);
    TEST_CHECK(!rig.visible(PaneRegion::Instrument));
    TEST_CHECK(sceneBox(&instrument).height == 24 && shown(&instrument));
    TEST_CHECK(rig.content(PaneRegion::Jump).y == 48);
    TEST_CHECK(rig.content(PaneRegion::Jump).height == 652);
    rig.click(instrument.collapseButton());
    TEST_CHECK(rig.visible(PaneRegion::Instrument));
    TEST_CHECK(rig.content(PaneRegion::Instrument).height == 410);
    auto& ribbon = rig.shell->header(Pane::Ribbon);
    rig.click(ribbon.collapseButton());
    TEST_CHECK(!rig.visible(PaneRegion::Ribbon));
    TEST_CHECK(rig.content(PaneRegion::Center).y == 24);
    TEST_CHECK(!rig.shell->divider(DividerId::Ribbon).draggable());
    rig.press(800, 25); // a fixed divider is only a line: the center under it takes the press
    rig.release(800, 25);
    TEST_CHECK(rig.centerPresses == 1);
    rig.click(ribbon.collapseButton());
    TEST_CHECK(rig.content(PaneRegion::Center).y == 224);
    // both left panes collapsed: the left column becomes the rail
    rig.click(instrument.collapseButton());
    rig.click(rig.shell->header(Pane::Jump).collapseButton());
    TEST_CHECK(!shown(&instrument) && !shown(&rig.shell->header(Pane::Jump)));
    const auto rail = sceneBox(rig.shell->railButton(Pane::Jump)->parent());
    TEST_CHECK(rail.x == 0 && rail.width == 32 && rail.height == 700);
    TEST_CHECK(rig.content(PaneRegion::Center).x == 32);
    rig.press(33, 300); // beside the rail's fixed divider
    rig.release(33, 300);
    TEST_CHECK(rig.centerPresses == 2);
    TEST_CHECK(tooltipOf(rig.shell->railButton(Pane::Jump)) == "Show Jump ahead");
    rig.click(rig.shell->railButton(Pane::Jump));
    TEST_CHECK(rig.visible(PaneRegion::Jump));
    TEST_CHECK(!shown(rig.shell->railButton(Pane::Jump)));
    TEST_CHECK(rig.content(PaneRegion::Jump).y == 48);
    TEST_CHECK(rig.commits.size() == 7);
  }

  void expandAndRestore() {
    Rig rig;
    auto& header = rig.shell->header(Pane::Instrument);
    rig.click(header.expandButton());
    TEST_CHECK(rig.commits.size() == 1 && rig.commits[0].expanded);
    const auto instrument = rig.content(PaneRegion::Instrument);
    TEST_CHECK(instrument.x == 0 && instrument.y == 24 && instrument.width == 1400 && instrument.height == 676);
    TEST_CHECK(!rig.visible(PaneRegion::Jump) && !rig.visible(PaneRegion::Ribbon) && !rig.visible(PaneRegion::Center));
    TEST_CHECK(!shown(&rig.shell->divider(DividerId::Left)) && !shown(&rig.shell->divider(DividerId::Ribbon)));
    TEST_CHECK(!shown(header.collapseButton()));
    rig.click(header.expandButton());
    TEST_CHECK(!rig.commits.back().expanded);
    TEST_CHECK(rig.content(PaneRegion::Instrument).width == 340);
    TEST_CHECK(rig.visible(PaneRegion::Center) && rig.visible(PaneRegion::Jump) && rig.visible(PaneRegion::Ribbon));
    TEST_CHECK(rig.shell->content(PaneRegion::Center) == rig.contents[3]); // hidden panes keep their content
  }

  void shortViewClosesRibbonThenJump() {
    Rig rig;
    auto& jump = rig.shell->header(Pane::Jump);
    auto& ribbon = rig.shell->header(Pane::Ribbon);
    rig.resize(1400, 322);
    TEST_CHECK(!rig.visible(PaneRegion::Ribbon) && rig.visible(PaneRegion::Jump));
    TEST_CHECK(ribbon.titleLabel()->text() == "Ribbon: Flame graph (closed: too short)");
    TEST_CHECK(!ribbon.collapseButton()->enabled());
    rig.resize(1400, 300);
    TEST_CHECK(!rig.visible(PaneRegion::Jump));
    TEST_CHECK(jump.titleLabel()->text() == "Jump ahead (closed: too short)");
    TEST_CHECK(sceneBox(&jump).height == 24 && shown(&jump));
    TEST_CHECK(rig.content(PaneRegion::Center).y == 24);
    rig.resize(1400, 700);
    TEST_CHECK(rig.visible(PaneRegion::Jump) && rig.visible(PaneRegion::Ribbon));
    TEST_CHECK(jump.titleLabel()->text() == "Jump ahead");
    TEST_CHECK(ribbon.titleLabel()->text() == "Ribbon: Flame graph");
    TEST_CHECK(ribbon.collapseButton()->enabled());
    TEST_CHECK(rig.commits.empty() && rig.shell->state() == PaneState{});
  }

  void narrowViewClosesRibbonThenLeftColumn() {
    Rig rig(kWideRibbon);
    rig.resize(850, 700);
    TEST_CHECK(rig.visible(PaneRegion::Ribbon) && rig.visible(PaneRegion::Instrument));
    rig.resize(700, 700);
    TEST_CHECK(!rig.visible(PaneRegion::Ribbon) && rig.visible(PaneRegion::Instrument));
    TEST_CHECK(rig.shell->header(Pane::Ribbon).titleLabel()->text() == "Ribbon: Flame graph (closed: too narrow)");
    rig.resize(500, 700);
    TEST_CHECK(!rig.visible(PaneRegion::Instrument) && !rig.visible(PaneRegion::Jump));
    const auto* button = rig.shell->railButton(Pane::Instrument);
    TEST_CHECK(shown(button) && !button->enabled());
    // Nest, the instrument a new view shows, until the owner names another.
    TEST_CHECK(button->glyph() != nullptr && glyphOf(*button->glyph()) == GlyphRegistry::lookup("chart-treemap"));
    TEST_CHECK(tooltipOf(button) == "Instrument: closed, the window is too narrow");
    TEST_CHECK(rig.content(PaneRegion::Center).x == 32);
    rig.resize(1400, 700);
    TEST_CHECK(rig.visible(PaneRegion::Instrument) && rig.visible(PaneRegion::Jump) && rig.visible(PaneRegion::Ribbon));
    TEST_CHECK(!shown(button));
    TEST_CHECK(rig.commits.empty());
  }

  void dividersSitAboveTheList() {
    Rig rig;
    rig.press(342, 400); // inside the center, within the divider's 6 px
    TEST_CHECK(rig.shell->resizing());
    rig.release(342, 400);
    TEST_CHECK(rig.centerPresses == 0);
    rig.press(360, 400);
    TEST_CHECK(rig.centerPresses == 1);
    rig.release(360, 400);
  }

  void draggedDividerStaysDuringItsDrag() {
    Rig rig;
    auto& divider = rig.shell->divider(DividerId::Instrument);
    rig.press(170, 434);
    rig.move(170, 460);
    rig.resize(500, 700); // the rail has no instrument divider
    TEST_CHECK(shown(&divider) && divider.parent() == rig.shell);
    TEST_CHECK(rig.dispatcher.pointerCaptured() && divider.dragging());
    rig.release(170, 460);
    TEST_CHECK(rig.commits.size() == 1);
    TEST_CHECK(!shown(&divider));
  }

  void rightToLeftMirrors() {
    Style::setRtl(true);
    {
      Rig rig;
      TEST_CHECK(rig.content(PaneRegion::Instrument).x == 1060);
      TEST_CHECK(rig.content(PaneRegion::Center).x == 0 && rig.content(PaneRegion::Center).width == 1060);
      TEST_CHECK(sceneBox(&rig.shell->divider(DividerId::Left)).x == 1057);
      TEST_CHECK(sceneBox(&rig.shell->divider(DividerId::Ribbon)).x == 0);
      rig.press(1060, 300);
      rig.move(1000, 300);
      rig.release(1000, 300);
      TEST_CHECK(rig.commits.size() == 1 && rig.commits[0].leftWidth == 400.0);
      TEST_CHECK(rig.content(PaneRegion::Instrument).x == 1000);
      TEST_CHECK(rig.content(PaneRegion::Center).width == 1000);
      rig.dispatcher.setFocus(&rig.shell->divider(DividerId::Left));
      rig.key(XKB_KEY_Left);
      TEST_CHECK(rig.shell->state().leftWidth == 416.0);
      rig.resize(1400.5F, 700); // mirrored about the real edge, not the whole-pixel one
      TEST_NEAR(rig.content(PaneRegion::Instrument).x, 984.5, 1e-3);
      TEST_NEAR(rig.content(PaneRegion::Instrument).width, 416, 1e-3);
      TEST_CHECK(rig.content(PaneRegion::Center).x == 0);
      TEST_NEAR(rig.content(PaneRegion::Center).width, 984.5, 1e-3);
      TEST_NEAR(sceneBox(&rig.shell->divider(DividerId::Left)).x, 981.5, 1e-3);
    }
    Style::setRtl(false);
  }

  void stateRoundTrips() {
    Rig rig;
    PaneState state;
    state.leftWidth = 450;
    state.instrumentHeight = 300;
    state.collapsed.ribbon = true;
    state.ribbonHeights = {250.0, std::nullopt, 190.0};
    state.columnWidths = std::array<double, 3>{300, 200, 150};
    rig.shell->setState(state);
    TEST_CHECK(rig.shell->state() == state);
    rig.layout();
    TEST_CHECK(rig.content(PaneRegion::Center).x == 450);
    TEST_CHECK(rig.content(PaneRegion::Jump).y == 324);
    PaneState hostile = state;
    hostile.leftWidth = std::numeric_limits<double>::quiet_NaN();
    hostile.instrumentHeight = -5;
    hostile.expanded = true;
    hostile.collapsed.instrument = true;
    rig.shell->setState(hostile);
    TEST_CHECK(!rig.shell->state().leftWidth && !rig.shell->state().instrumentHeight);
    TEST_CHECK(!rig.shell->state().expanded);
    TEST_CHECK(rig.shell->state().columnWidths == state.columnWidths);
    rig.layout();
    TEST_CHECK(rig.content(PaneRegion::Center).x == 340);
    TEST_CHECK(rig.commits.empty());
  }

  void layoutAllocationsAreBounded() {
    Rig rig;
    rig.layout();
    g_allocations = 0;
    for (int i = 0; i < 100; ++i) {
      rig.layout();
    }
    const auto steady = g_allocations;
    rig.press(340, 300);
    rig.move(400, 300);
    std::size_t dragging = 0;
    for (int i = 0; i < 100; ++i) {
      rig.dispatcher.pointerMotion(static_cast<float>(401 + (i % 50)), 300, ++rig.serial);
      const auto before = g_allocations;
      rig.layout(); // a pass whose state changed
      dragging += g_allocations - before;
    }
    rig.release(450, 300);
    PaneState state;
    state.leftWidth = 420;
    const auto before = g_allocations;
    TEST_CHECK(layoutPanes(state, {.width = 1400, .height = 700}, kLim, RibbonKind::Flame).rects.center->x == 420);
    const auto solver = g_allocations - before;
    std::println(
        "layout allocations: {} in 100 unchanged passes, {} in 100 drag frames, {} in one layoutPanes call", steady,
        dragging, solver
    );
    TEST_CHECK(steady == 0);
    TEST_CHECK(solver + 3 * 2 <= PaneShell::kLayoutAllocationBound); // three header titles, two each
    TEST_CHECK(dragging <= 100 * PaneShell::kLayoutAllocationBound);
  }
} // namespace

int main() {
  defaultLayout();
  dragMovesTheLeftEdge();
  dragMovesTheRibbonEdgeOnly();
  escapeAndLostCaptureRestore();
  doublePressResets();
  keysStep();
  collapseAndRestore();
  expandAndRestore();
  shortViewClosesRibbonThenJump();
  narrowViewClosesRibbonThenLeftColumn();
  dividersSitAboveTheList();
  draggedDividerStaysDuringItsDrag();
  rightToLeftMirrors();
  stateRoundTrips();
  layoutAllocationsAreBounded();
  std::println("pane shell checks passed");
}
