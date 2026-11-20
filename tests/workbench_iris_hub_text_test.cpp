#include "png_writer.h"
#include "render/scene/node.h"
#include "render/scene/text_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/iris_overlay.h"
#include "workbench/ring_geometry.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_view_rig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The hub of the Iris instrument, laid out and drawn with the toolkit's text shaping: a size that is a lower bound
// shows whole, or shortened to "≥ value", or is left out; every line lies inside the hub's circle.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::workbench::iris;
  using ::test::GlPixels;

  constexpr std::string_view kShort = "≥ 12.6\u00A0MiB";
  constexpr std::string_view kLong = "at least 12.6\u00A0MiB";
  constexpr double kSlack = 1e-6;
  constexpr std::size_t kFirstPrintable = 0x20;
  constexpr std::size_t kLastPrintable = 0x7E;

  // Natural text widths, laid out by the capture rig as a scene's labels are.
  class Shaper {
  public:
    explicit Shaper(::test::SceneCapture& capture) : m_capture(capture) {
      m_root.setDirection(FlexDirection::Horizontal);
      m_root.setAlign(FlexAlign::Start);
      m_label = static_cast<Label*>(m_root.addChild(std::make_unique<Label>()));
      m_label->setMaxLines(1);
    }

    double width(std::string_view text, double size, bool semibold) {
      constexpr int kRoom = 4000;
      (void)m_label->setText(text);
      m_label->setFontSize(static_cast<float>(size));
      m_label->setFontWeight(semibold ? FontWeight::SemiBold : FontWeight::Normal);
      (void)m_capture.capture(m_root, kRoom, 2, 1.0F);
      TEST_CHECK(m_label->width() < kRoom);
      return m_label->width();
    }

    // Advances at TextMeasure::kBaseSize, measured as the view measures its label font; a space between two letters.
    LabelFont font(bool semibold) {
      constexpr double kBase = TextMeasure::kBaseSize;
      LabelFont font;
      for (std::size_t code = kFirstPrintable; code <= kLastPrintable; ++code) {
        const std::string glyph(1, static_cast<char>(code));
        font.ascii[code] = code == ' ' ? width("a a", kBase, semibold) - (2 * width("a", kBase, semibold))
                                       : width(glyph, kBase, semibold);
      }
      font.other = width("…", kBase, semibold);
      return font;
    }

  private:
    ::test::SceneCapture& m_capture;
    Flex m_root;
    Label* m_label = nullptr;
  };

  enum class Size : std::uint8_t { Exact, LowerBound, NotMeasured, Measuring };

  // / > home > u > Projects > luciole > rw7, four context rings; rw7 holds 12.6 MiB in two files.
  Tree folderTree(Size size) {
    auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "/", .folder = true, .bytes = 5e9, .items = 1},
         {.parent = 0, .name = "home", .folder = true, .bytes = 5e9, .items = 1},
         {.parent = 1, .name = "u", .folder = true, .bytes = 5e9, .items = 1},
         {.parent = 2, .name = "Projects", .folder = true, .bytes = 5e9, .items = 1},
         {.parent = 3, .name = "luciole", .folder = true, .bytes = 5e9, .items = 1},
         {.parent = 4, .name = "rw7", .folder = true, .bytes = 13212058, .items = 2},
         {.parent = 5, .name = "a.bin", .bytes = 7e6},
         {.parent = 5, .name = "b.bin", .bytes = 6e6}},
        1e6
    );
    auto& folder = tree.nodes[5];
    folder.lowerBound = size == Size::LowerBound || size == Size::NotMeasured;
    folder.partial = size == Size::Measuring;
    folder.bytes = size == Size::NotMeasured || size == Size::Measuring ? 0 : folder.bytes;
    tree.root = 5;
    tree.ancestors = parentChain(tree, 5);
    return tree;
  }

  const HubLine* lineOf(const Hub& hub, HubInk ink) {
    const auto found = std::ranges::find(hub.lines, ink, &HubLine::ink);
    return found == hub.lines.end() ? nullptr : &*found;
  }

  std::string textOf(const Hub& hub, HubInk ink) {
    const auto* line = lineOf(hub, ink);
    return line == nullptr ? std::string{} : line->text;
  }

  // Every corner of the box lies in the circle.
  bool inCircle(const Rect& box, const ring::Point& center, double radius) {
    return std::ranges::all_of(std::array{box.x, box.x + box.width}, [&](double x) {
      return std::ranges::all_of(std::array{box.y, box.y + box.height}, [&](double y) {
        return std::hypot(x - center.x, y - center.y) <= radius + kSlack;
      });
    });
  }

  struct Rigs {
    ::test::SceneCapture& capture;
    Shaper shaper;
    TextMeasure measure;
  };

  // The hub's lines are as wide as the text is shaped, or wider, lie in its circle as shaped, and stay clear of each
  // other.
  void expectLinesFit(const Hub& hub, Rigs& rigs, bool corners = true) {
    const HubLine* above = nullptr;
    for (const auto& line : hub.lines) {
      const double shaped = rigs.shaper.width(line.text, line.fontSize, line.ink != HubInk::Items);
      TEST_CHECK(shaped <= line.box.width + 1.0);
      const Rect drawn{
          .x = line.box.x + ((line.box.width - shaped) / 2), .y = line.box.y, .width = shaped, .height = line.box.height
      };
      TEST_CHECK(!corners || inCircle(drawn, hub.center, hub.radius));
      TEST_CHECK(above == nullptr || line.box.y >= above->box.y + above->box.height);
      above = &line;
    }
  }

  Hub hubOf(Rigs& rigs, const ring::Dial& dial, Size size) {
    return hubLayout(folderTree(size), dial, 5, {}, {}, rigs.measure);
  }

  // The instrument pane of the page at a 1400 by 900 window: the window less the sidebar, tabs and toolbar.
  ring::Dial defaultPane() {
    luciole::test::view::Rig rig;
    auto choices = rig.view->choices();
    choices.instrument = WorkbenchInstrument::Iris;
    rig.view->setChoices(choices);
    constexpr float kPageLeft = 218;
    constexpr float kPageTop = 95;
    rig.resize(1400 - kPageLeft, 900 - kPageTop);
    const auto& pane = rig.view->instrument();
    std::println("default pane {} x {}", pane.width(), pane.height());
    return ring::compactDial(pane.width(), pane.height()).value();
  }

  void report(Rigs& rigs, const ring::Dial& dial) {
    std::println("hub diameter {}", 2 * dial.rHub);
    for (const std::string_view text : std::initializer_list<std::string_view>{
             kLong, kShort, "12.6\u00A0MiB", "at least 2 items", "not measured", "measuring…"
         }) {
      std::println(
          "  '{}': semibold 12 {:.1f}, 15 {:.1f}; regular 11 {:.1f}", text, rigs.shaper.width(text, 12, true),
          rigs.shaper.width(text, 15, true), rigs.shaper.width(text, 11, false)
      );
    }
  }

  // The default compact pane: the long wording does not fit the hub, the short one does at the headline size.
  void aLowerBoundShowsItsShortSizeInTheDefaultPane(Rigs& rigs, const ring::Dial& dial) {
    const auto hub = hubOf(rigs, dial, Size::LowerBound);
    TEST_CHECK(textOf(hub, HubInk::Name) == "rw7");
    TEST_CHECK(textOf(hub, HubInk::Total) == kShort);
    TEST_CHECK(textOf(hub, HubInk::Items) == "at least 2 items");
    TEST_NEAR(lineOf(hub, HubInk::Total)->fontSize, 15, kSlack);
    TEST_CHECK(rigs.shaper.width(kLong, 12, true) > 2 * dial.rHub);
    expectLinesFit(hub, rigs);
  }

  // An exact size keeps its place, its size and its text in the same pane.
  void anExactSizeIsUnchanged(Rigs& rigs, const ring::Dial& dial) {
    const auto hub = hubOf(rigs, dial, Size::Exact);
    TEST_CHECK(textOf(hub, HubInk::Total) == "12.6\u00A0MiB");
    TEST_CHECK(textOf(hub, HubInk::Items) == "2 items");
    TEST_NEAR(lineOf(hub, HubInk::Total)->fontSize, 15, kSlack);
    TEST_NEAR(lineOf(hub, HubInk::Items)->fontSize, 11, kSlack);
    expectLinesFit(hub, rigs);
  }

  // A full-size instrument at 1400 by 737 has room for the long wording and keeps it; the 68 px hub of a 900 by 600
  // one has room for neither and leaves the size out, while an exact size is shown in both.
  void aFullDialKeepsTheLongWordingWhereItFits(Rigs& rigs) {
    for (const auto& [width, height, wording] :
         {std::tuple{1400.0, 737.0, kLong}, std::tuple{900.0, 600.0, std::string_view{}}}) {
      const auto dial = ring::fullDial(width, height).value();
      const auto hub = hubOf(rigs, dial, Size::LowerBound);
      TEST_CHECK(textOf(hub, HubInk::Total) == wording);
      TEST_CHECK(textOf(hub, HubInk::Items) == (width > 1000 ? "at least 2 items" : ""));
      expectLinesFit(hub, rigs);
      const auto exact = hubOf(rigs, dial, Size::Exact);
      TEST_CHECK(textOf(exact, HubInk::Total) == "12.6\u00A0MiB" && textOf(exact, HubInk::Items) == "2 items");
      expectLinesFit(exact, rigs);
    }
  }

  // Hubs from small to large: the short wording shows only where the long one does not fit whole.
  void theShortWordingShowsOnlyWhereTheLongOneDoesNot(Rigs& rigs, const ring::Dial& dial) {
    for (const double radius : {47.0, 52.0, 56.0, 60.0, 66.0, 74.0}) {
      auto sized = dial;
      sized.rHub = radius;
      const auto hub = hubOf(rigs, sized, Size::LowerBound);
      const auto total = textOf(hub, HubInk::Total);
      const bool longFits = rigs.shaper.width(kLong, 12, true) <= 2 * radius;
      std::println("  hub {}: '{}'", 2 * radius, total);
      TEST_CHECK(total.empty() || total == kShort || total == kLong);
      TEST_CHECK(!longFits || total == kLong);
      TEST_CHECK(longFits || total != kLong);
      expectLinesFit(hub, rigs);
    }
  }

  // A hub too small for even the short wording leaves the size out.
  void aHubTooSmallLeavesTheSizeOut(Rigs& rigs, const ring::Dial& dial) {
    auto small = dial;
    small.rHub = 30;
    TEST_CHECK(rigs.shaper.width(kShort, 12, true) > 2 * small.rHub);
    const auto hub = hubOf(rigs, small, Size::LowerBound);
    TEST_CHECK(textOf(hub, HubInk::Total).empty());
    TEST_CHECK(textOf(hub, HubInk::Name) == "rw7");
    expectLinesFit(hub, rigs);
  }

  // "not measured" and "measuring…" follow the hub's width rule: shown whole, else left out. A line as wide as the
  // disc has corners that reach past the circle, so only the width is checked.
  void zeroSizesAreShownWholeOrLeftOut(Rigs& rigs, const ring::Dial& dial) {
    for (const auto& [size, text] : {std::pair{Size::NotMeasured, "not measured"}, {Size::Measuring, "measuring…"}}) {
      const auto hub = hubOf(rigs, dial, size);
      const auto shown = textOf(hub, HubInk::Total);
      std::println("  '{}' shown as '{}'", text, shown);
      TEST_CHECK(shown == text || shown.empty());
      TEST_CHECK((rigs.shaper.width(text, 12, true) <= 2 * dial.rHub) == (shown == text));
      expectLinesFit(hub, rigs, false);
    }
  }

  Color color(Rgb rgb) {
    return rgba(
        static_cast<float>(rgb.r) / 255.0F, static_cast<float>(rgb.g) / 255.0F, static_cast<float>(rgb.b) / 255.0F, 1
    );
  }

  // The instrument drawn at the default compact pane: each text the hub shows is drawn, shaped inside the hub's circle.
  void theDrawnHub(Rigs& rigs, const ring::Dial& pane, Size size, const std::vector<std::string>& texts) {
    const ColorPalette palette{};
    const auto width = static_cast<float>(pane.width);
    const auto height = static_cast<float>(pane.height);
    ::Node root;
    auto* surface = static_cast<Box*>(root.addChild(std::make_unique<Box>()));
    surface->setSize(width, height);
    surface->setFill(color(palette.surface));
    auto* view = static_cast<InstrumentView*>(
        root.addChild(std::make_unique<InstrumentView>(WorkbenchInstrument::Iris, [] { return 0U; }))
    );
    view->setSize(width, height);
    const PartInputs inputs{
        .snapshot = {.tree = std::make_shared<const Tree>(folderTree(size)), .serial = 1},
        .settings = {},
        .palette = palette
    };
    GlPixels pixels;
    for (int frame = 0; frame < 8; ++frame) {
      view->update(inputs, false);
      pixels = rigs.capture.capture(root, static_cast<int>(width), static_cast<int>(height), 1.0F);
    }
    TEST_CHECK(view->shownSnapshot().tree == inputs.snapshot.tree);
    const auto& shapes = view->shapeView().shapes();
    const auto hub =
        std::ranges::find_if(shapes, [](const Shape& shape) { return sceneRole(shape.id.role) == SceneRole::Hub; });
    TEST_CHECK(hub != shapes.end());
    // Each line the layout keeps is drawn as that text, centered on the hub and within the line's box. The readout at
    // the bottom of a small pane may repeat a text; the hub's is the label nearest the hub's center.
    const auto layout = hubOf(rigs, pane, size);
    TEST_CHECK(layout.lines.size() == texts.size());
    for (const auto& line : layout.lines) {
      const TextNode* nearest = nullptr;
      float nearestX = 0;
      float nearestY = 0;
      double nearestDistance = 0;
      for (const auto& child : view->shapeView().labelLayer().children()) {
        const auto* label = dynamic_cast<const TextNode*>(child.get());
        if (label == nullptr || !label->visible() || label->text() != line.text) {
          continue;
        }
        float x = 0;
        float y = 0;
        ::Node::absolutePosition(label, x, y);
        const double distance = std::hypot(x - hub->circle.center.x, y - hub->circle.center.y);
        if (nearest == nullptr || distance < nearestDistance) {
          nearest = label;
          nearestX = x;
          nearestY = y;
          nearestDistance = distance;
        }
      }
      TEST_CHECK(nearest != nullptr);
      const double shaped = rigs.shaper.width(line.text, nearest->fontSize(), line.ink != HubInk::Items);
      std::println(
          "  drawn '{}' at {:.1f} {:.1f}, {:.0f} px wide, size {}", line.text, nearestX, nearestY, shaped,
          nearest->fontSize()
      );
      TEST_NEAR(nearest->fontSize(), line.fontSize, 0.01);
      TEST_NEAR(nearestX + (shaped / 2), hub->circle.center.x, 1.0);
      TEST_CHECK(nearestY >= line.box.y - 1 && nearestY <= line.box.y + line.box.height + 1);
    }
    expectLinesFit(layout, rigs);
    if (const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR"); dir != nullptr && dir[0] != '\0') {
      const auto path =
          std::filesystem::path(dir) / (size == Size::Exact ? "iris-hub-exact.png" : "iris-hub-lower-bound.png");
      TEST_CHECK(::test::writePng(pixels, path));
      std::println("wrote {}", path.string());
    }
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  const auto pane = defaultPane();
  Rigs rigs{.capture = *capture, .shaper = Shaper(*capture), .measure = {}};
  rigs.measure.font = rigs.shaper.font(false);
  rigs.measure.boldFont = rigs.shaper.font(true);
  report(rigs, pane);
  aLowerBoundShowsItsShortSizeInTheDefaultPane(rigs, pane);
  anExactSizeIsUnchanged(rigs, pane);
  aFullDialKeepsTheLongWordingWhereItFits(rigs);
  theShortWordingShowsOnlyWhereTheLongOneDoesNot(rigs, pane);
  aHubTooSmallLeavesTheSizeOut(rigs, pane);
  zeroSizesAreShownWholeOrLeftOut(rigs, pane);
  theDrawnHub(rigs, pane, Size::LowerBound, {"rw7", std::string(kShort), "at least 2 items"});
  theDrawnHub(rigs, pane, Size::Exact, {"rw7", "12.6\u00A0MiB", "2 items"});
  std::println("workbench-iris-hub-text: all checks passed");
  return 0;
}
