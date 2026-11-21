#include "render/scene/text_node.h"
#include "scene_capture.h"
#include "system_text_scale.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/palette.h"
#include "ui/workbench/instrument_view.h"
#include "ui/workbench/part_view.h"
#include "ui/workbench/ribbon_view.h"
#include "ui/workbench/workbench_chrome.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <glib.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The instruments and the ribbons, laid out through their views with the toolkit's text shaping at the text scales a
// 14 pt, 11 pt, 16 pt and 21 pt system font give: no text lies over another, outside its pane or across the edge of
// the piece it names, and a font change lays the scene out as a fresh view at the new size does.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using luciole::test::WorkbenchRow;
  using ::test::SceneCapture;

  constexpr std::array<float, 4> kScales{1.0F, 44.0F / 42.0F, 16.0F * 96.0F / 72.0F / 14.0F, 2.0F};
  constexpr float kSlack = 0.75F;

  // / > home > user > projects > work > proj, entered at proj: five ancestors and a dozen entries with long names.
  Tree projectTree() {
    const std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "/", .folder = true, .bytes = 9e9, .items = 3},
        {.parent = 0, .name = "home", .folder = true, .bytes = 5e9, .items = 2},
        {.parent = 1, .name = "user", .folder = true, .bytes = 4e9, .items = 3},
        {.parent = 2, .name = "projects", .folder = true, .bytes = 3e9, .items = 2},
        {.parent = 3, .name = "work", .folder = true, .bytes = 2e9, .items = 3},
        {.parent = 4, .name = "proj", .folder = true, .bytes = 1.2e9, .items = 9},
        {.parent = 0, .name = "usr", .folder = true, .bytes = 3e9, .items = 1},
        {.parent = 0, .name = "var", .folder = true, .bytes = 1e9, .items = 1},
        {.parent = 1, .name = "guest", .folder = true, .bytes = 1e9, .items = 1},
        {.parent = 2, .name = "Music", .folder = true, .bytes = 6e8, .items = 1},
        {.parent = 3, .name = "luciole", .folder = true, .bytes = 1e9, .items = 1},
        {.parent = 4, .name = "scratch", .folder = true, .bytes = 6e8, .items = 1},
        {.parent = 4, .name = "todo.txt", .bytes = 2e8},
        {.parent = 5, .name = "README.md", .bytes = 1.5e8},
        {.parent = 5, .name = "data.csv", .bytes = 1e8},
        {.parent = 5, .name = "script.py", .bytes = 5e7},
        {.parent = 5, .name = "assets", .folder = true, .bytes = 3.5e8, .items = 2},
        {.parent = 5, .name = "build", .folder = true, .bytes = 2.5e8, .items = 1},
        {.parent = 5, .name = "documentation", .folder = true, .bytes = 1.5e8, .items = 1},
        {.parent = 5, .name = "src", .folder = true, .bytes = 1.2e8, .items = 2},
        {.parent = 5, .name = "tests", .folder = true, .bytes = 3e7, .items = 0},
        {.parent = 16, .name = "sounds", .folder = true, .bytes = 2e8, .items = 1},
        {.parent = 16, .name = "big.bin", .bytes = 1.5e8},
        {.parent = 17, .name = "deps", .folder = true, .bytes = 2.5e8, .items = 0},
        {.parent = 18, .name = "images", .folder = true, .bytes = 1.5e8, .items = 0},
        {.parent = 19, .name = "core", .folder = true, .bytes = 7e7, .items = 0},
    };
    auto tree = luciole::test::workbenchTree(rows, 1000);
    tree.root = 5;
    tree.ancestors = parentChain(tree, 5);
    return tree;
  }

  struct Ink {
    std::string text;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float right() const { return x + width; }
    float bottom() const { return y + height; }
  };

  // A pane: the part's view as the page puts it in its body.
  struct Pane {
    ::Node root;
    PartView* view = nullptr;
    float width = 0;
    float height = 0;

    Pane(std::unique_ptr<PartView> part, float w, float h) : view(part.get()), width(w), height(h) {
      root.addChild(paneBody(std::move(part)));
      root.children().front()->setSize(width, height);
    }

    // Lays out and draws until the shapes stop changing; the layout of Nest and Atlas finishes on a worker.
    void settle(SceneCapture& capture, const Tree& tree, std::uint64_t serial = 1) {
      const PartInputs inputs{
          .snapshot = {.tree = std::make_shared<const Tree>(tree), .serial = serial},
          .settings = {},
          .palette = workbenchPalette(palette),
      };
      std::size_t last = 0;
      int steady = 0;
      for (int round = 0; round < 600 && steady < 6; ++round) {
        while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
        }
        view->update(inputs, false);
        (void)capture.capture(root, static_cast<int>(width), static_cast<int>(height), 1.0F);
        const auto shown = view->shapeView().shapes().size() + view->sceneBuilds();
        steady = shown == last && shown > 0 && !view->shapeView().transitioning() ? steady + 1 : 0;
        last = shown;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
      }
      TEST_CHECK(steady >= 6);
    }

    // The text drawn, with the box its glyphs fill: the em square above and below the baseline.
    std::vector<Ink> inks(SceneCapture& capture) const {
      std::vector<Ink> out;
      for (const auto& child : view->shapeView().labelLayer().children()) {
        const auto* text = dynamic_cast<const TextNode*>(child.get());
        if (text == nullptr || !text->visible() || text->text().empty() || text->rotation() != 0.0F) {
          continue;
        }
        float x = 0;
        float y = 0;
        ::Node::absolutePosition(text, x, y);
        const float em = text->fontSize() * capture.textScale();
        out.push_back(
            Ink{.text = text->text(),
                .x = x,
                .y = y - 0.8F * em,
                .width = capture.textWidth(text->text(), text->fontSize(), text->fontWeight()),
                .height = em}
        );
      }
      return out;
    }
  };

  const Shape* pieceUnder(const Pane& pane, const Ink& ink) {
    const Shape* found = nullptr;
    for (const auto& shape : pane.view->shapeView().shapes()) {
      const bool box = shape.kind == ShapeKind::Rect || shape.kind == ShapeKind::RoundedRect;
      const auto& r = shape.rect;
      const float cx = ink.x + ink.width / 2;
      const float cy = ink.y + ink.height / 2;
      if (!box
          || !shape.hittable
          || r.width * r.height < 100
          || cx < r.x
          || cx > r.x + r.width
          || cy < r.y
          || cy > r.y + r.height) {
        continue;
      }
      if (found == nullptr || r.width * r.height < found->rect.width * found->rect.height) {
        found = &shape;
      }
    }
    return found;
  }

  // No text lies over another or outside the pane; text over a piece lies within it.
  std::size_t expectClear(SceneCapture& capture, const Pane& pane, std::string_view what, float scale) {
    const auto inks = pane.inks(capture);
    for (std::size_t a = 0; a < inks.size(); ++a) {
      const auto& first = inks[a];
      if (first.x < -kSlack
          || first.right() > pane.width + kSlack
          || first.y < -kSlack
          || first.bottom() > pane.height + kSlack) {
        std::println("{} at {:.2f}: '{}' leaves the pane", what, scale, first.text);
        TEST_CHECK(false);
      }
      if (const auto* piece = pieceUnder(pane, first); piece != nullptr) {
        const auto& r = piece->rect;
        if (first.x < r.x - kSlack
            || first.right() > r.x + r.width + kSlack
            || first.y < r.y - kSlack
            || first.bottom() > r.y + r.height + kSlack) {
          std::println(
              "{} at {:.2f}: '{}' {:.1f},{:.1f} {:.1f}x{:.1f} crosses its piece {:.1f},{:.1f} {:.1f}x{:.1f}", what,
              scale, first.text, first.x, first.y, first.width, first.height, r.x, r.y, r.width, r.height
          );
          TEST_CHECK(false);
        }
      }
      for (std::size_t b = a + 1; b < inks.size(); ++b) {
        const auto& second = inks[b];
        const float across = std::min(first.right(), second.right()) - std::max(first.x, second.x);
        const float down = std::min(first.bottom(), second.bottom()) - std::max(first.y, second.y);
        if (across > 1.0F && down > 1.5F) {
          std::println("{} at {:.2f}: '{}' lies over '{}'", what, scale, first.text, second.text);
          TEST_CHECK(false);
        }
      }
    }
    return inks.size();
  }

  struct Kind {
    std::string_view name;
    bool instrument = false;
    int index = 0;
    float width = 0;
    float height = 0;
  };

  // A ribbon's least height grows with the text, as the pane limits give it.
  std::unique_ptr<Pane> paneOf(const Kind& kind, float scale) {
    std::unique_ptr<PartView> view;
    if (kind.instrument) {
      view = std::make_unique<InstrumentView>(static_cast<WorkbenchInstrument>(kind.index), [] { return 0U; });
    } else {
      view = std::make_unique<RibbonView>(static_cast<WorkbenchRibbon>(kind.index), [] { return 0U; });
    }
    return std::make_unique<Pane>(
        std::move(view), kind.width, kind.instrument ? kind.height : std::round(kind.height * scale)
    );
  }

  constexpr std::array<Kind, 6> kKinds{{
      {.name = "nest", .instrument = true, .index = 0, .width = 700, .height = 520},
      {.name = "iris", .instrument = true, .index = 1, .width = 520, .height = 520},
      {.name = "atlas", .instrument = true, .index = 2, .width = 700, .height = 520},
      {.name = "flame", .instrument = false, .index = 0, .width = 1060, .height = 280},
      {.name = "flow", .instrument = false, .index = 1, .width = 1060, .height = 280},
      {.name = "ladder", .instrument = false, .index = 2, .width = 1060, .height = 280},
  }};

  void textStaysClear(SceneCapture& capture) {
    const auto tree = projectTree();
    for (const float wanted : kScales) {
      ::test::setSystemTextScale(capture, wanted);
      for (const auto& kind : kKinds) {
        auto pane = paneOf(kind, wanted);
        pane->settle(capture, tree);
        const auto drawn = expectClear(capture, *pane, kind.name, wanted);
        std::println("{} at {:.2f}: {} texts", kind.name, wanted, drawn);
        TEST_CHECK(drawn > 0);
      }
    }
  }

  // A pane laid out at one size, then at another after the font changed, shows what a pane made at the second does.
  void fontChangeLaysOutAgain(SceneCapture& capture) {
    const auto tree = projectTree();
    for (const auto& kind : kKinds) {
      ::test::setSystemTextScale(capture, 1.0F);
      auto live = paneOf(kind, kScales[2]);
      live->settle(capture, tree);
      ::test::setSystemTextScale(capture, kScales[2]);
      live->settle(capture, tree);
      auto fresh = paneOf(kind, kScales[2]);
      fresh->settle(capture, tree);
      const auto before = live->inks(capture);
      const auto after = fresh->inks(capture);
      std::println("{}: {} texts after the change, {} in a fresh pane", kind.name, before.size(), after.size());
      TEST_CHECK(std::ranges::equal(live->view->shapeView().shapes(), fresh->view->shapeView().shapes()));
      TEST_CHECK(before.size() == after.size());
      for (std::size_t i = 0; i < before.size(); ++i) {
        TEST_CHECK(before[i].text == after[i].text);
        TEST_CHECK(std::abs(before[i].x - after[i].x) < 0.01F && std::abs(before[i].y - after[i].y) < 0.01F);
      }
    }
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(Palette{});
  textStaysClear(*capture);
  fontChangeLaysOutAgain(*capture);
  std::println("workbench-text-scale-panes: all checks passed");
  return 0;
}
