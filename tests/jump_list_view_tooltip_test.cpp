#include "jump_list_view_rig.h"

// A jump row's tooltip: its path, sitting by its row, and why a value reads "at least".
namespace {
  using namespace luciole::test::jump_rig;
  using namespace luciole;

  // A row's tooltip gives its path and sits by that row, also far down a scrolled list, not by the list's top.
  void testTooltipSitsByItsRow() {
    Rig rig;
    rig.show({.list = longList(30)});
    const auto top = centerOf(rowAt(*rig.view, "p/f02"));
    for (int notch = 0; notch < 4; ++notch) {
      TEST_CHECK(rig.dispatcher.pointerAxis(top.x, top.y, 0, 0, 20, 1, 120, 0));
      rig.layout();
    }
    rig.layout();
    const JumpRowTile* row = nullptr;
    for (const auto* shown : rowsOf(*rig.view)) {
      const float y = sceneY(shown);
      if (y + shown->height() <= 30 + 300 - 4) { // the lowest row wholly inside the list
        row = shown;
      }
    }
    TEST_CHECK(row != nullptr && row->row().path > "p/f07"); // past the rows the list showed before the wheel
    const float rowY = sceneY(row);
    const float rowBottom = rowY + row->height();
    const auto at = centerOf(row);
    rig.move(at);
    TooltipPresenter presenter([] {});
    presenter.resume();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!presenter.visible() && std::chrono::steady_clock::now() < deadline) { // bounded by the deadline
      TimerManager::instance().tick();
      presenter.sync(rig.renderer, rig.root, at.x, at.y, {rig.root.width(), rig.root.height()});
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    TEST_CHECK(presenter.visible());
    TEST_CHECK(tooltipShows(presenter, row->row().path));
    const bool below = presenter.y() >= rowBottom && presenter.y() <= rowBottom + Style::spaceSm;
    const bool above =
        presenter.y() + presenter.height() <= rowY && presenter.y() + presenter.height() >= rowY - Style::spaceMd;
    TEST_CHECK(below || above);
    presenter.dismiss();
  }

  // A row whose value reads "at least" says why under its path; a row with an exact value gives the path alone.
  void testTooltipSaysWhyAValueIsALowerBound() {
    Rig rig;
    auto list = fixtureList();
    list.rows[0].notCounted = "Not counted: 1 unreadable folder (secret).";
    rig.show({.list = list});
    const auto tipOf = [&](const std::string& path) {
      const auto at = centerOf(rowAt(*rig.view, path));
      rig.move(at);
      TooltipPresenter presenter([] {});
      presenter.resume();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!presenter.visible() && std::chrono::steady_clock::now() < deadline) { // bounded by the deadline
        TimerManager::instance().tick();
        presenter.sync(rig.renderer, rig.root, at.x, at.y, {rig.root.width(), rig.root.height()});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      TEST_CHECK(presenter.visible());
      const bool shown = tooltipShows(presenter, path + "\nNot counted: 1 unreadable folder (secret).");
      const bool plain = tooltipShows(presenter, path);
      presenter.dismiss();
      return std::pair{shown, plain};
    };
    TEST_CHECK(tipOf(list.rows[0].path).first);
    TEST_CHECK(tipOf(list.rows[1].path).second);
  }

} // namespace

int main() {
  testTooltipSitsByItsRow();
  testTooltipSaysWhyAValueIsALowerBound();
  std::println("jump list view tooltip checks passed");
}
