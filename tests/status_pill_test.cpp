#include "png_writer.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/browser/status_pill.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>

// The status pill as the page builds it, placed with the toolkit's text shaping in a list-sized area: the pill and
// its text stay inside the area, free space leaves before the status is cut, and the status ends in an ellipsis
// only when it alone does not fit.
namespace {
  constexpr std::string_view kFree = "881.3 GiB free";
  constexpr float kMargin = 40; // body around the area, where an overflowing pill would show
  constexpr float kHeight = 80;

  // The page body: the list's area drawn as a card, and the pill placed over it after the body's layout.
  class Body final : public Node {
  public:
    explicit Body(float areaWidth) : m_area{kMargin, kMargin, areaWidth, kHeight} {
      m_card = static_cast<Box*>(addChild(
          ui::box(
              {.fill = colorSpecFromRole(ColorRole::Surface),
               .border = colorSpecFromRole(ColorRole::OutlineVariant),
               .radius = Style::radiusMd}
          )
      ));
      addChild(buildStatusPill(m_pill));
      m_pill.pill->setVisible(true);
    }

    const StatusPill& pill() const noexcept { return m_pill; }
    const LayoutRect& area() const noexcept { return m_area; }
    int width() const { return static_cast<int>(m_area.width + (2 * kMargin)); }

    void show(std::string_view status, bool freeSpace) {
      (void)m_pill.status->setText(status);
      (void)m_pill.freeSpace->setText(kFree);
      m_freeSpace = freeSpace;
    }

  protected:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      setPosition(rect.x, rect.y);
      setSize(rect.width, rect.height);
      m_card->arrange(renderer, m_area);
      arrangeStatusPill(renderer, m_pill, m_area, m_freeSpace);
    }

  private:
    LayoutRect m_area;
    Box* m_card = nullptr;
    StatusPill m_pill;
    bool m_freeSpace = false;
  };

  struct Placed {
    LayoutRect pill;
    LayoutRect status;
    std::optional<LayoutRect> freeSpace; // none while it is hidden
  };

  LayoutRect rectOf(const Node& node) {
    LayoutRect rect{.width = node.width(), .height = node.height()};
    Node::absolutePosition(&node, rect.x, rect.y);
    return rect;
  }

  bool inside(const LayoutRect& inner, const LayoutRect& outer) {
    constexpr float kSlack = 0.01F;
    return inner.x >= outer.x - kSlack
        && inner.y >= outer.y - kSlack
        && inner.x + inner.width <= outer.x + outer.width + kSlack
        && inner.y + inner.height <= outer.y + outer.height + kSlack;
  }

  void keepForViewing(const ::test::GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // Places `status` in an area `areaWidth` wide; every part of the pill lies inside the area.
  Placed place(
      ::test::SceneCapture& capture, float areaWidth, float scale, std::string_view status, bool freeSpace,
      std::string_view name
  ) {
    Body body(areaWidth);
    body.show(status, freeSpace);
    const auto pixels = capture.capture(body, body.width(), static_cast<int>(kHeight + (2 * kMargin)), scale);
    TEST_CHECK(pixels.width > 0);
    if (!name.empty()) {
      keepForViewing(pixels, std::format("status-pill-{}-{}-{}x.png", name, areaWidth, scale));
    }
    const auto& pill = body.pill();
    Placed placed{.pill = rectOf(*pill.pill), .status = rectOf(*pill.status), .freeSpace = std::nullopt};
    if (pill.freeSpace->visible() && pill.freeSpace->participatesInLayout()) {
      placed.freeSpace = rectOf(*pill.freeSpace);
    }
    TEST_CHECK(inside(placed.pill, body.area()));
    TEST_CHECK(inside(placed.status, placed.pill));
    if (placed.freeSpace) {
      TEST_CHECK(inside(*placed.freeSpace, placed.pill));
      TEST_CHECK(placed.status.x + placed.status.width <= placed.freeSpace->x + 0.01F);
    }
    std::println(
        "  {} px area, '{}': pill {:.1f}..{:.1f}, status {:.1f} px, free space {}", areaWidth, status, placed.pill.x,
        placed.pill.x + placed.pill.width, placed.status.width, placed.freeSpace ? "shown" : "hidden"
    );
    return placed;
  }

  // The status's own width, with all the room it could want.
  float natural(::test::SceneCapture& capture, float scale, std::string_view status) {
    constexpr float kRoomy = 2000;
    return place(capture, kRoomy, scale, status, false, {}).status.width;
  }

  void checkAt(::test::SceneCapture& capture, float scale) {
    std::println("scale {}", scale);
    constexpr float kNarrow = 300; // the pill takes 270 px of it, its text 246
    constexpr std::string_view kShort = "13 items";
    constexpr std::string_view kSelected = "128 items · 12 selected · 692.3 KiB";
    constexpr std::string_view kSearch = "Searching… · 1024 results · 3 folders could not be searched · 12 selected";

    const auto alone = place(capture, kNarrow, scale, kShort, true, "short");
    TEST_CHECK(alone.freeSpace.has_value());
    TEST_NEAR(alone.status.width, natural(capture, scale, kShort), 0.5);

    const auto unwanted = place(capture, kNarrow, scale, kShort, false, "");
    TEST_CHECK(!unwanted.freeSpace);

    // the selection fits alone, not beside the free space: the free space leaves and the status stays whole
    const auto selected = place(capture, kNarrow, scale, kSelected, true, "selected");
    TEST_CHECK(!selected.freeSpace);
    TEST_NEAR(selected.status.width, natural(capture, scale, kSelected), 0.5);

    // too long for the pill alone: it ends in an ellipsis at the pill's edge
    const auto search = place(capture, kNarrow, scale, kSearch, true, "search");
    TEST_CHECK(!search.freeSpace);
    TEST_CHECK(search.status.width < natural(capture, scale, kSearch) - 1);
    TEST_CHECK(search.status.width > kNarrow / 2);

    // the list of a 1100 px window at scale 1
    for (const auto status : {kShort, kSelected, kSearch}) {
      (void)place(capture, 333, scale, status, true, "");
    }
    (void)place(capture, 333, scale, "13 items · 1 selected · 692.3 KiB", true, "list-1100");
    // a narrow area still holds the whole pill
    (void)place(capture, 120, scale, kSelected, true, "narrow");
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
  for (const float scale : {1.0F, 1.5F}) {
    checkAt(*capture, scale);
  }
  std::println("status-pill: all checks passed");
  return 0;
}
