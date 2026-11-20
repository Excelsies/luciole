#include "png_writer.h"
#include "render/scene/node.h"
#include "render/text/cairo_text_renderer.h"
#include "render/text/glyph_registry.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/shape_labels.h"
#include "ui/controls/shape_view.h"
#include "workbench/flame_row.h"
#include "workbench/scene_shapes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Labels of a shape view drawn with real text: aligned boxes put their ink where the alignment says.
namespace {
  using namespace luciole::workbench;
  using test::GlPixels;
  using test::SceneCapture;

  constexpr int kWidth = 420;
  constexpr int kHeight = 180;
  constexpr std::array<float, 2> kScales{1.0F, 1.5F};
  constexpr Rgba8 kInk{240, 240, 240, 255};

  ShapeLabel label(std::string text, float x, float y, float width, LabelAlign align, float size = 13) {
    return {
        .node = kNoNode,
        .text = std::move(text),
        .x = x,
        .y = y,
        .width = width,
        .height = size * 1.3F,
        .align = align,
        .fontSize = size,
        .rotation = 0,
        .color = kInk,
    };
  }

  // Device columns from the first to past the last that hold ink, over the rows of a logical box.
  struct Columns {
    int first = 0;
    int end = 0;
  };

  std::optional<Columns> inkColumns(const GlPixels& pixels, const ShapeLabel& box, float scale) {
    const int top = static_cast<int>(std::floor(box.y * scale));
    const int bottom = std::min(static_cast<int>(std::ceil((box.y + box.height) * scale)), pixels.height);
    std::optional<Columns> found;
    for (int x = 0; x < pixels.width; ++x) {
      for (int y = std::max(top, 0); y < bottom; ++y) {
        if (pixels.at(x, y)[3] > 24) {
          found = Columns{.first = found ? found->first : x, .end = x + 1};
          break;
        }
      }
    }
    return found;
  }

  void keepForViewing(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // Centered labels keep their ink centered in the box, end-aligned ones end at the box's right edge, and
  // start-aligned ones begin at its left edge, each within a device pixel.
  void alignedInk(SceneCapture& capture) {
    ::Node root;
    auto* view = static_cast<ShapeView*>(root.addChild(std::make_unique<ShapeView>([] { return 0U; })));
    view->setPosition(0, 0);
    view->setSize(kWidth, kHeight);
    const std::array<ShapeLabel, 5> labels{
        label("proj", 40, 10, 200, LabelAlign::Center),
        label("+4 more", 40, 40, 120, LabelAlign::Center),
        label("assets", 250, 40, 90, LabelAlign::Center, 12),
        label("171 KiB  ·  2 items", 40, 80, 300, LabelAlign::End),
        label("build", 40, 120, 300, LabelAlign::Start, 14),
    };
    SceneShapes scene;
    scene.labels.assign(labels.begin(), labels.end());
    view->setScene(std::move(scene));
    for (const float scale : kScales) {
      const auto pixels = capture.capture(root, kWidth, kHeight, scale);
      keepForViewing(pixels, std::format("shape-labels-aligned-{}x.png", scale));
      for (const auto& each : labels) {
        // One label per row band, except the two centered ones that share a band and sit apart in x.
        const auto columns = [&] -> std::optional<Columns> {
          if (each.text != "+4 more" && each.text != "assets") {
            return inkColumns(pixels, each, scale);
          }
          GlPixels half = pixels;
          const int cut = static_cast<int>(std::lround(245 * scale));
          for (int y = 0; y < half.height; ++y) {
            for (int x = 0; x < half.width; ++x) {
              const bool other = each.text == "assets" ? x < cut : x >= cut;
              if (other) {
                half.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(half.width) + x) * 4 + 3] = 0;
              }
            }
          }
          return inkColumns(half, each, scale);
        }();
        TEST_CHECK(columns.has_value());
        if (!columns) {
          continue;
        }
        const float left = each.x * scale;
        const float right = (each.x + each.width) * scale;
        const float middle = static_cast<float>(columns->first + columns->end) / 2;
        std::println(
            "{}x \"{}\": ink {} to {}, box {:.1f} to {:.1f}", scale, each.text, columns->first, columns->end, left,
            right
        );
        switch (each.align) {
        case LabelAlign::Center:
          TEST_NEAR(middle, (left + right) / 2, 1.0F);
          break;
        case LabelAlign::End:
          TEST_CHECK(static_cast<float>(columns->end) <= right + 1);
          TEST_CHECK(static_cast<float>(columns->end) >= right - 2 * scale);
          break;
        case LabelAlign::Start:
          TEST_CHECK(static_cast<float>(columns->first) >= left - 1);
          TEST_CHECK(static_cast<float>(columns->first) <= left + 2 * scale);
          break;
        }
      }
    }
  }

  int inkCount(const GlPixels& pixels, const ShapeLabel& box, float scale) {
    const int top = std::max(static_cast<int>(std::floor(box.y * scale)), 0);
    const int bottom = std::min(static_cast<int>(std::ceil((box.y + box.height) * scale)), pixels.height);
    int count = 0;
    for (int y = top; y < bottom; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        count += pixels.at(x, y)[3] > 24 ? 1 : 0;
      }
    }
    return count;
  }

  // A semibold label is drawn semibold: wider and with more ink than the same text regular.
  void weightsAreDrawn(SceneCapture& capture) {
    ::Node root;
    auto* view = static_cast<ShapeView*>(root.addChild(std::make_unique<ShapeView>([] { return 0U; })));
    view->setPosition(0, 0);
    view->setSize(kWidth, kHeight);
    auto regular = label("assets", 40, 10, 200, LabelAlign::Start, 14);
    auto semibold = label("assets", 40, 45, 200, LabelAlign::Start, 14);
    semibold.weight = LabelWeight::Semibold;
    SceneShapes scene;
    scene.labels = {regular, semibold};
    view->setScene(std::move(scene));
    for (const float scale : kScales) {
      const auto pixels = capture.capture(root, kWidth, kHeight, scale);
      keepForViewing(pixels, std::format("shape-labels-weight-{}x.png", scale));
      const auto plain = inkColumns(pixels, regular, scale);
      const auto bold = inkColumns(pixels, semibold, scale);
      TEST_CHECK(plain.has_value() && bold.has_value());
      if (!plain || !bold) {
        continue;
      }
      std::println(
          "{}x semibold ink {} px wide, {} px; regular {} px wide, {} px", scale, bold->end - bold->first,
          inkCount(pixels, semibold, scale), plain->end - plain->first, inkCount(pixels, regular, scale)
      );
      TEST_CHECK(bold->end - bold->first > plain->end - plain->first);
      TEST_CHECK(inkCount(pixels, semibold, scale) > inkCount(pixels, regular, scale) * 11 / 10);
    }
  }

  TextFont fontOf(LabelWeight weight) {
    return {
        .weight = weight == LabelWeight::Semibold ? FontWeight::SemiBold : FontWeight::Normal,
        .family = {},
        .features = {}
    };
  }

  // Advances of the label font at 12 px laid out at `scale`, as a part's label metrics take them.
  LabelFont advancesAt(CairoTextRenderer& text, float scale, LabelWeight weight) {
    const auto font = fontOf(weight);
    LabelFont advances;
    for (std::size_t code = 0x20; code < 0x7F; ++code) {
      const char ch = static_cast<char>(code);
      advances.ascii[code] = text.measure(scale, std::string_view(&ch, 1), 12, font).width;
    }
    advances.other = text.measure(scale, "…", 12, font).width;
    return advances;
  }

  struct WholeCase {
    std::string_view text;
    float size = 11;
    LabelWeight weight = LabelWeight::Regular;
  };

  // Counts and measures that pass the fitter whole in a box exactly their summed advances wide draw whole, though
  // their laid-out text runs wider than that sum: the ink ends where it ends in a wide box.
  void wholeLabelsAreNeverCut(SceneCapture& capture) {
    constexpr std::array<WholeCase, 5> kCases{{
        {.text = "762 bytes"},
        {.text = "1,234 items"},
        {.text = "762 bytes", .weight = LabelWeight::Semibold},
        {.text = "+10 more", .size = 13},
        {.text = "assets", .size = 14, .weight = LabelWeight::Semibold},
    }};
    CairoTextRenderer text;
    text.initialize(nullptr, nullptr);
    std::size_t wider = 0;
    for (const float scale : {1.25F, 1.5F, 2.0F}) {
      const std::array<LabelFont, 2> fonts{
          advancesAt(text, scale, LabelWeight::Regular), advancesAt(text, scale, LabelWeight::Semibold)
      };
      const auto summed = [&](std::string_view s, double size, LabelWeight weight) {
        return labelTextWidth(s, fonts[weight == LabelWeight::Semibold ? 1 : 0]) * size / 12;
      };
      std::vector<ShapeLabel> labels;
      for (const auto& each : kCases) {
        const double sum = summed(each.text, each.size, each.weight);
        if (text.measure(scale, each.text, each.size, fontOf(each.weight)).width <= sum) {
          continue;
        }
        ++wider;
        const auto y = static_cast<float>(10 + labels.size() * 20);
        auto tight =
            label(std::string(each.text), 10, y, static_cast<float>(sum) + 0.01F, LabelAlign::Start, each.size);
        tight.fit = LabelFit::Whole;
        tight.weight = each.weight;
        auto wide = tight;
        wide.x = 210;
        wide.width = 200;
        labels.push_back(tight);
        labels.push_back(wide);
      }
      auto shown = labels;
      fitShapeLabels(shown, summed);
      TEST_CHECK(shown.size() == labels.size());
      ::Node root;
      auto* view = static_cast<ShapeView*>(root.addChild(std::make_unique<ShapeView>([] { return 0U; })));
      view->setPosition(0, 0);
      view->setSize(kWidth, kHeight);
      SceneShapes scene;
      scene.labels = shown;
      view->setScene(std::move(scene));
      const auto pixels = capture.capture(root, kWidth, kHeight, scale);
      keepForViewing(pixels, std::format("shape-labels-whole-{}x.png", scale));
      for (std::size_t i = 0; i + 1 < shown.size(); i += 2) {
        GlPixels left = pixels;
        GlPixels right = pixels;
        const auto split = static_cast<std::size_t>(std::lround(205 * scale));
        for (std::size_t y = 0; y < static_cast<std::size_t>(pixels.height); ++y) {
          for (std::size_t x = 0; x < static_cast<std::size_t>(pixels.width); ++x) {
            auto& other = x < split ? right : left;
            other.rgba[(y * static_cast<std::size_t>(pixels.width) + x) * 4 + 3] = 0;
          }
        }
        const auto tight = inkColumns(left, shown[i], scale);
        const auto wide = inkColumns(right, shown[i + 1], scale);
        TEST_CHECK(tight.has_value() && wide.has_value());
        if (!tight || !wide) {
          continue;
        }
        const int shift = static_cast<int>(std::lround(200 * scale));
        std::println(
            "{}x \"{}\" in {:.2f}: ink {} to {}, in a wide box {} to {}", scale, shown[i].text, shown[i].width,
            tight->first, tight->end, wide->first - shift, wide->end - shift
        );
        TEST_CHECK(std::abs(tight->end - (wide->end - shift)) <= 1);
        TEST_CHECK(std::abs(tight->first - (wide->first - shift)) <= 1);
      }
    }
    // This rig's font draws some of them wider than their summed advances.
    TEST_CHECK(wider > 0);
  }

  // Every file category names a glyph the toolkit has.
  void categoryGlyphsExist() {
    for (std::size_t i = 0; i < kFileCategoryCount; ++i) {
      const auto name = categoryGlyph(static_cast<FileCategory>(i));
      TEST_CHECK(GlyphRegistry::contains(name));
    }
    TEST_CHECK(categoryGlyph(FileCategory::Folder) == "folder");
    TEST_CHECK(categoryGlyph(static_cast<FileCategory>(200)) == "file");
  }
} // namespace

int main() {
  categoryGlyphsExist();
  std::string reason;
  const auto capture = SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  alignedInk(*capture);
  weightsAreDrawn(*capture);
  wholeLabelsAreNeverCut(*capture);
  std::println("shape label pixel checks passed");
  return 0;
}
