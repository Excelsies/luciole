#include "code_editor_rig.h"
#include "png_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole;
  using namespace luciole::test::editor;
  using luciole::test::tiles::bytesOf;
  using luciole::test::tiles::distance;
  using luciole::test::tiles::inkIn;
  using ::test::GlPixels;

  constexpr std::array<float, 2> kScales{1.0F, 1.5F};

  constexpr std::string_view kCss = ".card {\n"
                                    "  color: #e06c75;\n"
                                    "  background: rgb(40, 44, 52);\n"
                                    "}\n"
                                    "/* a comment */\n"
                                    "@media (width > 600px) { .card { margin: 0 } }\n";

  // Writes the pixels as code-editor-<name>-<scale>x.png when LUCIOLE_CAPTURE_DIR is set.
  void keep(const GlPixels& pixels, std::string_view name, float scale) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("code-editor-{}-{}x.png", name, scale);
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // Pixels within `tolerance` of a color in rows [top, bottom) and columns [0, right).
  std::size_t count(const GlPixels& pixels, const Color& color, int top, int bottom, int right, int tolerance = 0) {
    const auto wanted = bytesOf(color);
    std::size_t found = 0;
    for (int y = std::max(0, top); y < std::min(bottom, pixels.height); ++y) {
      for (int x = 0; x < std::min(right, pixels.width); ++x) {
        found += distance(pixels.at(x, y), wanted) <= tolerance ? 1 : 0;
      }
    }
    return found;
  }

  // Pixels that differ between two captures of one size in rows [top, bottom) and columns [0, right).
  std::size_t differing(const GlPixels& a, const GlPixels& b, int top, int bottom, int right) {
    std::size_t out = 0;
    for (int y = std::max(0, top); y < std::min(bottom, a.height); ++y) {
      for (int x = 0; x < std::min(right, a.width); ++x) {
        out += distance(a.at(x, y), b.at(x, y)) > 24 ? 1 : 0;
      }
    }
    return out;
  }

  // The largest difference in text coverage between two captures in rows [top, bottom) and columns [0, right), each
  // pixel's coverage read from its green channel against its own background. Pixels alike in both are skipped, which
  // leaves the antialiased text.
  double coverageGap(
      const GlPixels& a, const Color& backgroundA, const GlPixels& b, const Color& backgroundB, int top, int bottom,
      int right
  ) {
    const double text = bytesOf(palette.onSurface)[1];
    const double baseA = bytesOf(backgroundA)[1];
    const double baseB = bytesOf(backgroundB)[1];
    double gap = 0;
    for (int y = std::max(0, top); y < std::min(bottom, a.height); ++y) {
      for (int x = 0; x < std::min(right, a.width); ++x) {
        if (a.at(x, y) == b.at(x, y)) {
          continue;
        }
        const double coverageA = (a.at(x, y)[1] - baseA) / (text - baseA);
        const double coverageB = (b.at(x, y)[1] - baseB) / (text - baseB);
        gap = std::max(gap, std::abs(coverageA - coverageB));
      }
    }
    return gap;
  }

  // The brightest pixel in rows [top, bottom) and columns [0, right).
  luciole::test::tiles::Pixel brightest(const GlPixels& pixels, int top, int bottom, int right) {
    luciole::test::tiles::Pixel out{};
    for (int y = std::max(0, top); y < std::min(bottom, pixels.height); ++y) {
      for (int x = 0; x < std::min(right, pixels.width); ++x) {
        const auto pixel = pixels.at(x, y);
        if (pixel[0] + pixel[1] + pixel[2] > out[0] + out[1] + out[2]) {
          out = pixel;
        }
      }
    }
    return out;
  }

  GlPixels preview(::test::SceneCapture& capture, float scale) {
    luciole::test::tiles::Rig rig(capture);
    rig.scale = scale;
    rig.view->show(luciole::test::tiles::textOf(std::string(kCss), "style.css"));
    return rig.settle();
  }

  // A colored file with a selection and the caret, beside the read-only preview of the same file.
  void testColoredFile(::test::SceneCapture& capture, float scale) {
    const auto shown = preview(capture, scale);
    keep(shown, "preview", scale);
    Rig rig(capture, kCss, "style.css");
    rig.scale = scale;
    rig.settle();
    rig.caretAt({1, 2});
    rig.key(XKB_KEY_End, KeyMod::Shift);
    const auto edited = rig.settle();
    keep(edited, "colored", scale);
    const auto caret = rig.view->caretRect();
    const auto row = [&](std::size_t line, float edge) {
      return static_cast<int>(std::lround((8.0F + (static_cast<float>(line) + edge) * caret.height) * scale));
    };
    const int right = edited.width - static_cast<int>(16 * scale);
    // Lines without the selection or the caret are drawn exactly as the preview draws them.
    const std::size_t same = differing(edited, shown, row(2, 0), row(6, 0), right);
    const std::size_t first = differing(edited, shown, 0, row(1, 0), right);
    const std::size_t inkPreview = inkIn(shown, 0, right, row(2, 0), row(6, 0));
    std::println(
        "{}x: {} of {} ink pixels differ from the preview below the selection, {} on the first line", scale, same,
        inkPreview, first
    );
    TEST_CHECK(inkPreview > static_cast<std::size_t>(300 * scale));
    TEST_CHECK(same == 0 && first == 0);
    // The selection lies behind the text of line 1 and nowhere else.
    const auto selection = count(edited, palette.primaryContainer, row(1, 0), row(2, 0), right);
    TEST_CHECK(selection > static_cast<std::size_t>(500 * scale * scale));
    TEST_CHECK(count(edited, palette.primaryContainer, 0, row(1, 0), right) == 0);
    TEST_CHECK(count(edited, palette.primaryContainer, row(2, 0), edited.height, right) == 0);
    // Over the selection the text keeps its place and its color: the same coverage in every pixel as in the preview,
    // and the same brightest pixel.
    const int caretLeft = static_cast<int>(std::floor(caret.x * scale)) - 1;
    const double gap = coverageGap(
        edited, palette.primaryContainer, shown, palette.surfaceContainerLow, row(1, 0), row(2, 0), caretLeft
    );
    const auto brightEdited = brightest(edited, row(1, 0), row(2, 0), caretLeft);
    const auto brightShown = brightest(shown, row(1, 0), row(2, 0), caretLeft);
    std::println(
        "{}x: on the selected line text coverage differs by at most {:.3f}, brightest pixels {} and {} from the text "
        "color",
        scale, gap, distance(brightEdited, bytesOf(palette.onSurface)),
        distance(brightShown, bytesOf(palette.onSurface))
    );
    TEST_CHECK(gap < 0.08);
    TEST_CHECK(distance(brightEdited, brightShown) <= 6);
    // The caret at the selection's end, a line high, in the caret color.
    const int caretX = static_cast<int>(std::floor((caret.x + (caret.width / 2)) * scale));
    const int caretY = static_cast<int>(std::lround((caret.y + (caret.height / 2)) * scale));
    TEST_CHECK(distance(edited.at(caretX, caretY), bytesOf(palette.primary)) < 40);
    TEST_NEAR(caret.y, 8.0F + caret.height, 0.01);
  }

  // A composition in progress is drawn underlined at the caret without entering the text.
  void testComposition(::test::SceneCapture& capture, float scale) {
    Rig rig(capture, "let value = 1;", "main.rs");
    rig.scale = scale;
    rig.settle();
    rig.caretAt({0, 4});
    TextInputEdit edit;
    edit.preeditText = "\u306B\u307B\u3093";
    edit.preeditCursorBegin = -1;
    edit.hasPreedit = true;
    rig.view->textInputApplyEdit(edit);
    const auto pixels = rig.settle();
    keep(pixels, "composing", scale);
    const auto caret = rig.view->caretRect();
    const int bottom = static_cast<int>(std::lround((caret.y + caret.height) * scale));
    const int left = static_cast<int>(std::lround(8.0F * scale));
    TEST_CHECK(rig.buffer.text() == "let value = 1;");
    // The underline runs in the plain text color below the composed characters.
    std::size_t underline = 0;
    for (int y = bottom - static_cast<int>(4 * scale); y < bottom; ++y) {
      underline += count(pixels, palette.onSurface, y, y + 1, static_cast<int>(caret.x * scale), 60);
    }
    TEST_CHECK(underline > static_cast<std::size_t>(20 * scale));
    TEST_CHECK(inkIn(pixels, left, static_cast<int>(caret.x * scale), 0, bottom) > 50);
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  for (const float scale : kScales) {
    testColoredFile(*capture, scale);
    testComposition(*capture, scale);
  }
  std::println("code-editor-pixels: all checks passed");
  return 0;
}
