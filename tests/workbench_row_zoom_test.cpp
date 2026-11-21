#include "test_check.h"
#include "ui/workbench/row_zoom.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench_view_rig.h"

#include <memory>
#include <print>

// Where Ctrl with the wheel resizes the rows: over the page's list, a column ahead or Jump ahead of the real view,
// and nowhere else.
namespace {
  using namespace luciole;
  using namespace luciole::test::view;

  bool over(Rig& rig, const Node* list, const Node& node) {
    const auto at = middle(node);
    return overRowList(*rig.view, list, at.x, at.y);
  }

  void rowListsTakeThePointerAndTheOthersDoNot() {
    Rig rig;
    rig.settle();
    TEST_CHECK(shown(rig.list) && over(rig, rig.list, *rig.list));
    TEST_CHECK(shown(&rig.view->column(0)) && over(rig, rig.list, rig.view->column(0)));
    TEST_CHECK(shown(&rig.view->jumpList()) && over(rig, rig.list, rig.view->jumpList()));
    for (std::size_t slot = 0; slot < 3; ++slot) {
      if (shown(&rig.view->column(slot))) {
        TEST_CHECK(over(rig, rig.list, rig.view->column(slot)));
      }
    }
    TEST_CHECK(!over(rig, rig.list, rig.view->instrument()));
    TEST_CHECK(!over(rig, rig.list, rig.view->ribbon()));
    // the page hides its list (an empty folder) or the view does not show it: its place takes no zoom, the columns
    // and Jump ahead still do
    TEST_CHECK(!over(rig, nullptr, *rig.list));
    TEST_CHECK(over(rig, nullptr, rig.view->jumpList()) && over(rig, nullptr, rig.view->column(0)));
    rig.list->setVisible(false);
    TEST_CHECK(!over(rig, rig.list, *rig.list));
    TEST_CHECK(over(rig, rig.list, rig.view->jumpList()));
  }

  void aPreviewIsNotARowList() {
    Rig rig;
    auto made = std::make_unique<InputArea>();
    const auto* preview = made.get();
    TEST_CHECK(rig.view->setPreview(std::move(made)) == nullptr);
    rig.settle();
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("note.txt"), .folder = false});
    rig.frame();
    rig.frame();
    TEST_CHECK(rig.view->center().column(0) == preview && shown(preview));
    TEST_CHECK(!over(rig, rig.list, *preview));
  }
} // namespace

int main() {
  rowListsTakeThePointerAndTheOthersDoNot();
  aPreviewIsNotARowList();
  std::println("workbench-row-zoom: all checks passed");
  return 0;
}
